#include <linux/irqdomain.h>
#include <linux/phylink.h>

#include "global2.h"
#include "port.h"
#include "serdes.h"

/* Definitions from drivers/net/phy/marvell.c, which would be good to reuse. */
#define MII_M1011_IEVENT		19
#define MII_M1011_IEVENT_LINK_CHANGE	BIT(10)
#define MII_M1011_IMASK			18
#define MII_M1011_IMASK_LINK_CHANGE	BIT(10)
#define MII_MARVELL_PHY_PAGE		22
#define MII_MARVELL_FIBER_PAGE		1

struct marvell_c22_pcs {
	struct mdio_device mdio;
	struct phylink_pcs phylink_pcs;
	unsigned int irq;
	char name[64];
	bool (*link_check)(struct marvell_c22_pcs *mpcs);
	void (*link_change)(struct marvell_c22_pcs *mpcs, bool up);

	struct mv88e6xxx_chip *chip;
	int port;
};

static struct marvell_c22_pcs *pcs_to_marvell_c22_pcs(struct phylink_pcs *pcs)
{
	return container_of(pcs, struct marvell_c22_pcs, phylink_pcs);
}

static int marvell_c22_pcs_set_fiber_page(struct marvell_c22_pcs *mpcs)
{
	struct mii_bus *bus = mpcs->mdio.bus;
	u16 page;
	int err;

	mutex_lock(&bus->mdio_lock);

	err = __mdiobus_read(bus, mpcs->mdio.addr, MII_MARVELL_PHY_PAGE);
	if (err < 0) {
		dev_err(mpcs->mdio.dev.parent,
			"%s: can't read Serdes page register: %pe\n",
			mpcs->name, ERR_PTR(err));
		return err;
	}

	page = err;

	err = __mdiobus_write(bus, mpcs->mdio.addr, MII_MARVELL_PHY_PAGE,
			      MII_MARVELL_FIBER_PAGE);
	if (err) {
		dev_err(mpcs->mdio.dev.parent,
			"%s: can't set Serdes page register: %pe\n",
			mpcs->name, ERR_PTR(err));
		return err;
	}

	return page;
}

static int marvell_c22_pcs_restore_page(struct marvell_c22_pcs *mpcs,
				      int oldpage, int ret)
{
	struct mii_bus *bus = mpcs->mdio.bus;
	int err;

	if (oldpage >= 0) {
		err = __mdiobus_write(bus, mpcs->mdio.addr,
				      MII_MARVELL_PHY_PAGE, oldpage);
		if (err)
			dev_err(mpcs->mdio.dev.parent,
				"%s: can't restore Serdes page register: %pe\n",
				mpcs->name, ERR_PTR(err));
		if (!err || ret < 0)
			err = ret;
	} else {
		err = oldpage;
	}
	mutex_unlock(&bus->mdio_lock);

	return err;
}

static irqreturn_t marvell_c22_pcs_handle_irq(int irq, void *dev_id)
{
	struct marvell_c22_pcs *mpcs = dev_id;
	irqreturn_t status = IRQ_NONE;
	int err, oldpage;

	oldpage = marvell_c22_pcs_set_fiber_page(mpcs);
	if (oldpage < 0)
		goto fail;

	err = __mdiobus_read(mpcs->mdio.bus, mpcs->mdio.addr, MII_M1011_IEVENT);
	if (err >= 0 && err & MII_M1011_IEVENT_LINK_CHANGE) {
		err = __mdiobus_read(mpcs->mdio.bus, mpcs->mdio.addr, MII_BMSR);
		if (err >= 0)
			mpcs->link_change(mpcs, !!(err & BMSR_LSTATUS));
		status = IRQ_HANDLED;
	}

fail:
	marvell_c22_pcs_restore_page(mpcs, oldpage, 0);

	return status;
}

static int marvell_c22_pcs_modify(struct marvell_c22_pcs *mpcs, u8 reg,
				  u16 mask, u16 val)
{
	int oldpage, err;

	oldpage = marvell_c22_pcs_set_fiber_page(mpcs);
	if (oldpage >= 0)
		err = __mdiobus_modify(mpcs->mdio.bus, mpcs->mdio.addr,
				       reg, mask, val);

	return marvell_c22_pcs_restore_page(mpcs, oldpage, err);
}

static int marvell_c22_pcs_control_irq(struct marvell_c22_pcs *mpcs,
				       bool enable)
{
	u16 val = enable ? MII_M1011_IMASK_LINK_CHANGE : 0;

	return marvell_c22_pcs_modify(mpcs, MII_M1011_IMASK,
				      MII_M1011_IMASK_LINK_CHANGE, val);
}

static int marvell_c22_pcs_enable(struct phylink_pcs *pcs)
{
	struct marvell_c22_pcs *mpcs = pcs_to_marvell_c22_pcs(pcs);
	int err;

	err = marvell_c22_pcs_modify(mpcs, MII_BMCR, BMCR_PDOWN, 0);
	if (err || !mpcs->irq)
		return err;

	return marvell_c22_pcs_control_irq(mpcs, true);
}

static void marvell_c22_pcs_disable(struct phylink_pcs *pcs)
{
	struct marvell_c22_pcs *mpcs = pcs_to_marvell_c22_pcs(pcs);

	marvell_c22_pcs_control_irq(mpcs, false);
	marvell_c22_pcs_modify(mpcs, MII_BMCR, BMCR_PDOWN, BMCR_PDOWN);
}

static void marvell_c22_pcs_get_state(struct phylink_pcs *pcs,
				    struct phylink_link_state *state)
{
	struct marvell_c22_pcs *mpcs = pcs_to_marvell_c22_pcs(pcs);
	int oldpage, bmsr, lpa;

	state->link = false;

	if (mpcs->link_check && !mpcs->link_check(mpcs))
		return;

	oldpage = marvell_c22_pcs_set_fiber_page(mpcs);
	if (oldpage >= 0) {
		bmsr = __mdiobus_read(mpcs->mdio.bus, mpcs->mdio.addr,
				      MII_BMSR);
		lpa = __mdiobus_read(mpcs->mdio.bus, mpcs->mdio.addr, MII_LPA);
	}

	if (marvell_c22_pcs_restore_page(mpcs, oldpage, 0) >= 0 &&
	    bmsr >= 0 && lpa >= 0)
		phylink_mii_c22_pcs_decode_state(state, bmsr, lpa);
}

static int marvell_c22_pcs_config(struct phylink_pcs *pcs,
				unsigned int mode,
			        phy_interface_t interface,
			        const unsigned long *advertise,
			        bool permit_pause_to_mac)
{
	struct marvell_c22_pcs *mpcs = pcs_to_marvell_c22_pcs(pcs);
	int oldpage, adv, err;

	adv = phylink_mii_c22_pcs_encode_advertisement(interface, advertise);
	if (adv < 0)
		return 0;

	oldpage = marvell_c22_pcs_set_fiber_page(mpcs);
	if (oldpage >= 0)
		err = __mdiobus_modify_changed(mpcs->mdio.bus, mpcs->mdio.addr,
					       MII_ADVERTISE, 0xffff, adv);

	return marvell_c22_pcs_restore_page(mpcs, oldpage, err);
}

static void marvell_c22_pcs_an_restart(struct phylink_pcs *pcs)
{
	struct marvell_c22_pcs *mpcs = pcs_to_marvell_c22_pcs(pcs);

	marvell_c22_pcs_modify(mpcs, MII_BMCR, BMCR_ANRESTART, BMCR_ANRESTART);
}

static void marvell_c22_pcs_link_up(struct phylink_pcs *pcs,
				  unsigned int mode,
				  phy_interface_t interface,
				  int speed, int duplex)
{
	struct marvell_c22_pcs *mpcs = pcs_to_marvell_c22_pcs(pcs);
	u16 bmcr;
	int err;

	if (phylink_autoneg_inband(mode))
		return;

	switch (speed) {
	case SPEED_1000:
		bmcr = BMCR_SPEED1000;
		break;
	case SPEED_100:
		bmcr = BMCR_SPEED100;
		break;
	case SPEED_10:
		bmcr = 0;
		break;
	}

	if (duplex == DUPLEX_FULL)
		bmcr |= BMCR_FULLDPLX;

	err = marvell_c22_pcs_modify(mpcs, MII_BMCR, BMCR_SPEED100 |
				     BMCR_FULLDPLX | BMCR_SPEED1000, bmcr);
	if (err)
		dev_err(mpcs->mdio.dev.parent,
			"%s: failed to configure mpcs: %pe\n", mpcs->name,
			ERR_PTR(err));
}

static const struct phylink_pcs_ops marvell_c22_pcs_ops = {
	.pcs_enable = marvell_c22_pcs_enable,
	.pcs_disable = marvell_c22_pcs_disable,
	.pcs_get_state = marvell_c22_pcs_get_state,
	.pcs_config = marvell_c22_pcs_config,
	.pcs_an_restart = marvell_c22_pcs_an_restart,
	.pcs_link_up = marvell_c22_pcs_link_up,
};

static int marvell_c22_pcs_init(struct marvell_c22_pcs *mpcs,
				struct device *dev, struct mii_bus *bus,
				unsigned int addr, unsigned int irq)
{
	int err;

	mpcs->mdio.dev.parent = dev;
	mpcs->mdio.bus = bus;
	mpcs->mdio.addr = addr;
	mpcs->phylink_pcs.ops = &marvell_c22_pcs_ops;
	mpcs->irq = irq;

	if (irq) {
		err = devm_request_threaded_irq(dev, irq, NULL,
						marvell_c22_pcs_handle_irq,
						IRQF_ONESHOT, mpcs->name,
						mpcs);
		if (err)
			return err;
	}

	return 0;
}

static bool mv88e6352_pcs_link_check(struct marvell_c22_pcs *mpcs)
{
	u8 cmode;

	/* Port 4 can be in auto-media mode. Check that the port is
	 * associated with the mpcs.
	 */
	mv88e6xxx_reg_lock(mpcs->chip);
	mpcs->chip->info->ops->port_get_cmode(mpcs->chip, mpcs->port, &cmode);
	mv88e6xxx_reg_unlock(mpcs->chip);

	return cmode == MV88E6XXX_PORT_STS_CMODE_100BASEX ||
	       cmode == MV88E6XXX_PORT_STS_CMODE_1000BASEX ||
	       cmode == MV88E6XXX_PORT_STS_CMODE_SGMII;
}

static void mv88e6352_pcs_link_change(struct marvell_c22_pcs *mpcs, bool up)
{
	dsa_port_phylink_mac_change(mpcs->chip->ds, mpcs->port, up);
}

int mv88e6352_pcs_init(struct mv88e6xxx_chip *chip, int port)
{
	struct marvell_c22_pcs *mpcs;
	struct mii_bus *bus;
	struct device *dev;
	unsigned int irq;
	int err;

	err = mv88e6352_g2_scratch_port_has_serdes(chip, port);
	if (err <= 0)
		return err;

	irq = irq_find_mapping(chip->g2_irq.domain, MV88E6352_SERDES_IRQ);
	bus = mv88e6xxx_default_mdio_bus(chip);
	dev = chip->dev;

	mpcs = devm_kzalloc(dev, sizeof(*mpcs), GFP_KERNEL);
	if (!mpcs)
		return -ENOMEM;

	snprintf(mpcs->name, sizeof(mpcs->name),
		 "mv88e6xxx-%s-serdes-%d", dev_name(dev), port);

	mpcs->link_check = mv88e6352_pcs_link_check;
	mpcs->link_change = mv88e6352_pcs_link_change;
	mpcs->chip = chip;
	mpcs->port = port;

	mv88e6xxx_reg_unlock(chip);
	err = marvell_c22_pcs_init(mpcs, dev, bus, MV88E6352_ADDR_SERDES, irq);
	mv88e6xxx_reg_lock(chip);
	if (err)
		return err;

	dsa_port_phylink_set_pcs(chip->ds, port, &mpcs->phylink_pcs);

	return 0;
}
