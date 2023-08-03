/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CZ.NIC's Turris Omnia MCU driver
 *
 * 2023 by Marek Behún <kabel@kernel.org>
 */

#ifndef __TURRIS_OMNIA_MCU_H
#define __TURRIS_OMNIA_MCU_H

#include <linux/i2c.h>
#include <linux/types.h>
#include <asm/byteorder.h>

struct omnia_mcu {
	struct i2c_client *client;
	const char *type;
	u16 features;
};

static inline int omnia_cmd_read(const struct i2c_client *client, u8 cmd, void *reply,
				 unsigned int len)
{
	struct i2c_msg msgs[2];
	int ret;

	msgs[0].addr = client->addr;
	msgs[0].flags = 0;
	msgs[0].len = 1;
	msgs[0].buf = &cmd;
	msgs[1].addr = client->addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = len;
	msgs[1].buf = reply;

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret < 0)
		return ret;
	if (ret != ARRAY_SIZE(msgs))
		return -EIO;

	return 0;
}

static inline int omnia_cmd_read_u16(const struct i2c_client *client, u8 cmd)
{
	__le16 reply;
	int err;

	err = omnia_cmd_read(client, cmd, &reply, sizeof(reply));

	return err ?: le16_to_cpu(reply);
}

static inline int omnia_cmd_read_u8(const struct i2c_client *client, u8 cmd)
{
	u8 reply;
	int err;

	err = omnia_cmd_read(client, cmd, &reply, sizeof(reply));

	return err ?: reply;
}

#endif /* __TURRIS_OMNIA_MCU_H */
