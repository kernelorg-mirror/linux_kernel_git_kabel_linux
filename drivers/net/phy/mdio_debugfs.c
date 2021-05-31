// SPDX-License-Identifier: GPL-2.0+
/* MDIO Bus debugfs support
 *
 * Copyright (c) 2020 Marek Behun <marek.behun@nic.cz>
 */

#include <linux/debugfs.h>
#include <linux/phy.h>

static int val_get(void *data, u64 *ptr)
{
	struct mii_bus *bus = data;
	int val;

	if (bus->debug_reg & BIT(30))
		val = mdiobus_c45_read(bus, bus->debug_addr,
				       (bus->debug_reg >> 16) & 0xff,
				       bus->debug_reg & 0xffff);
	else
		val = mdiobus_read(bus, bus->debug_addr, bus->debug_reg);
	if (val < 0)
		return val;

	*ptr = val;

	return 0;
}

static int val_set(void *data, u64 val)
{
	struct mii_bus *bus = data;
	int res;

	if (val > 0xffff)
		return -EINVAL;

	if (bus->debug_reg & BIT(30))
		res = mdiobus_c45_write(bus, bus->debug_addr,
					(bus->debug_reg >> 16) & 0xff,
					bus->debug_reg & 0xffff, val);
	else
		res = mdiobus_write(bus, bus->debug_addr, bus->debug_reg, val);
	if (res < 0)
		return res;

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(val_fops, val_get, val_set, "0x%04llx\n");

static struct dentry *mdiobus_dentry;

int mdiobus_register_debugfs(struct mii_bus *bus)
{
	struct dentry *dir, *entry;

	dir = debugfs_create_dir(dev_name(&bus->dev), mdiobus_dentry);
	if (IS_ERR(dir))
		return PTR_ERR(dir);

	debugfs_create_u32("addr", 0600, dir, &bus->debug_addr);
	debugfs_create_u32("reg", 0600, dir, &bus->debug_reg);

	entry = debugfs_create_file_unsafe("val", 0600, dir, bus, &val_fops);
	if (IS_ERR(entry))
		goto fail;

	return 0;

fail:
	debugfs_remove_recursive(dir);
	return PTR_ERR(entry);
}

void mdiobus_unregister_debugfs(struct mii_bus *bus)
{
	struct dentry *dir;

	dir = debugfs_lookup(dev_name(&bus->dev), mdiobus_dentry);
	if (!dir)
		return;

	debugfs_remove_recursive(dir);
}

int mdiobus_debugfs_init(void)
{
	struct dentry *dentry;

	dentry = debugfs_create_dir("mdio_bus", NULL);
	if (IS_ERR(dentry))
		return PTR_ERR(dentry);

	mdiobus_dentry = dentry;

	return 0;
}

void mdiobus_debugfs_exit(void)
{
	if (mdiobus_dentry) {
		debugfs_remove(mdiobus_dentry);
		mdiobus_dentry = NULL;
	}
}
