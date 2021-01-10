#ifndef MDIOBUS_DEBUGFS_H
#define MDIOBUS_DEBUGFS_H

#include <linux/kernel.h>
#include <linux/phy.h>

#if IS_ENABLED(CONFIG_MDIO_BUS_DEBUGFS)

int mdiobus_register_debugfs(struct mii_bus *bus);
void mdiobus_unregister_debugfs(struct mii_bus *bus);
int mdiobus_debugfs_init(void);
void mdiobus_debugfs_exit(void);

#else

static inline int mdiobus_register_debugfs(struct mii_bus *bus)
{
	return 0;
}

static inline void mdiobus_unregister_debugfs(struct mii_bus *bus)
{
}

static inline int mdiobus_debugfs_init(void)
{
	return 0;
}

static inline void mdiobus_debugfs_exit(void)
{
}

#endif

#endif
