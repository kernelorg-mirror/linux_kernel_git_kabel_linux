/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CZ.NIC's Turris Omnia MCU driver
 *
 * 2023 by Marek Behún <kabel@kernel.org>
 */

#ifndef __TURRIS_OMNIA_MCU_H
#define __TURRIS_OMNIA_MCU_H

#include <linux/bitops.h>
#include <linux/gpio/driver.h>
#include <linux/i2c.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/workqueue.h>
#include <asm/byteorder.h>

struct omnia_mcu {
	struct i2c_client *client;
	const char *type;
	u16 features;

	/* GPIO chip */
	struct gpio_chip gc;
	struct mutex lock;
	u32 mask, rising, falling, both, cached, is_cached;
	/* Old MCU firmware handling needs the following */
	struct delayed_work button_release_emul_work;
	u16 last_status;
	bool button_pressed_emul;
};

static inline int omnia_cmd_write(const struct i2c_client *client, void *cmd,
				  size_t len)
{
	int ret;

	ret = i2c_master_send(client, cmd, len);

	return ret < 0 ? ret : 0;
}

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

/* Returns 0 on success */
static inline int omnia_cmd_read_bits(const struct i2c_client *client, u8 cmd,
				      u32 bits, u32 *dst)
{
	__le32 reply;
	int err;

	if (!bits) {
		*dst = 0;
		return 0;
	}

	err = omnia_cmd_read(client, cmd, &reply, (__fls(bits) >> 3) + 1);
	if (err)
		return err;

	*dst = le32_to_cpu(reply) & bits;

	return 0;
}

static inline int omnia_cmd_read_bit(const struct i2c_client *client, u8 cmd,
				     u32 bit)
{
	u32 reply;
	int err;

	err = omnia_cmd_read_bits(client, cmd, bit, &reply);
	if (err)
		return err;

	return !!reply;
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

extern const struct attribute_group omnia_mcu_gpio_group;

int omnia_mcu_register_gpiochip(struct omnia_mcu *mcu);

#endif /* __TURRIS_OMNIA_MCU_H */
