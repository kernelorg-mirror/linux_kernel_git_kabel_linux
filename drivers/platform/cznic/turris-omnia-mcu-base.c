// SPDX-License-Identifier: GPL-2.0
/*
 * CZ.NIC's Turris Omnia MCU driver
 *
 * 2023 by Marek Behún <kabel@kernel.org>
 */

#include <linux/device.h>
#include <linux/hex.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/turris-omnia-mcu-interface.h>
#include <linux/types.h>
#include <linux/sysfs.h>

#include "turris-omnia-mcu.h"

#define OMNIA_FW_VERSION_LEN		20
#define OMNIA_FW_VERSION_HEX_LEN	(2 * OMNIA_FW_VERSION_LEN + 1)

static int omnia_get_version_hash(struct omnia_mcu *mcu, bool bootloader,
				  u8 version[static OMNIA_FW_VERSION_HEX_LEN])
{
	u8 reply[OMNIA_FW_VERSION_LEN];
	int err;

	err = omnia_cmd_read(mcu->client, bootloader ? CMD_GET_FW_VERSION_BOOT :
						       CMD_GET_FW_VERSION_APP,
			     reply, sizeof(reply));
	if (err)
		return err;

	version[OMNIA_FW_VERSION_HEX_LEN - 1] = '\0';
	bin2hex(version, reply, OMNIA_FW_VERSION_LEN);

	return 0;
}

static ssize_t fw_version_hash_show(struct device *dev, char *buf,
				    bool bootloader)
{
	struct omnia_mcu *mcu = i2c_get_clientdata(to_i2c_client(dev));
	u8 version[OMNIA_FW_VERSION_HEX_LEN];
	int err;

	err = omnia_get_version_hash(mcu, bootloader, version);
	if (err)
		return err;

	return sysfs_emit(buf, "%s\n", version);
}

static ssize_t fw_version_hash_application_show(struct device *dev,
						struct device_attribute *a,
						char *buf)
{
	return fw_version_hash_show(dev, buf, false);
}
static DEVICE_ATTR_RO(fw_version_hash_application);

static ssize_t fw_version_hash_bootloader_show(struct device *dev,
					       struct device_attribute *a,
					       char *buf)
{
	return fw_version_hash_show(dev, buf, true);
}
static DEVICE_ATTR_RO(fw_version_hash_bootloader);

static ssize_t fw_features_show(struct device *dev, struct device_attribute *a,
				char *buf)
{
	struct omnia_mcu *mcu = i2c_get_clientdata(to_i2c_client(dev));

	return sysfs_emit(buf, "0x%x\n", mcu->features);
}
static DEVICE_ATTR_RO(fw_features);

static ssize_t mcu_type_show(struct device *dev, struct device_attribute *a,
			     char *buf)
{
	struct omnia_mcu *mcu = i2c_get_clientdata(to_i2c_client(dev));

	return sysfs_emit(buf, "%s\n", mcu->type);
}
static DEVICE_ATTR_RO(mcu_type);

static ssize_t reset_selector_show(struct device *dev,
				   struct device_attribute *a, char *buf)
{
	int ret;

	ret = omnia_cmd_read_u8(to_i2c_client(dev), CMD_GET_RESET);
	if (ret < 0)
		return ret;

	return sysfs_emit(buf, "%d\n", ret);
}
static DEVICE_ATTR_RO(reset_selector);

static struct attribute *omnia_mcu_attrs[] = {
	&dev_attr_fw_version_hash_application.attr,
	&dev_attr_fw_version_hash_bootloader.attr,
	&dev_attr_fw_features.attr,
	&dev_attr_mcu_type.attr,
	&dev_attr_reset_selector.attr,
	NULL
};
ATTRIBUTE_GROUPS(omnia_mcu);

static void omnia_mcu_print_version_hash(struct omnia_mcu *mcu, bool bootloader)
{
	const char *type = bootloader ? "bootloader" : "application";
	struct device *dev = &mcu->client->dev;
	u8 version[OMNIA_FW_VERSION_HEX_LEN];
	int err;

	err = omnia_get_version_hash(mcu, bootloader, version);
	if (err) {
		dev_err(dev, "Cannot read MCU %s firmware version: %d\n", type,
			err);
		return;
	}

	dev_info(dev, "MCU %s firmware version hash: %s\n", type, version);
}

static const char *omnia_status_to_mcu_type(uint16_t status)
{
	switch (status & STS_MCU_TYPE_MASK) {
	case STS_MCU_TYPE_STM32:
		return "STM32";
	case STS_MCU_TYPE_GD32:
		return "GD32";
	case STS_MCU_TYPE_MKL:
		return "MKL";
	default:
		return "unknown";
	}
}

static void omnia_info_missing_feature(struct device *dev, const char *feature)
{
	dev_info(dev,
		 "Your board's MCU firmware does not support the %s feature.\n",
		 feature);
}

static int omnia_mcu_read_features(struct omnia_mcu *mcu)
{
	static const struct {
		uint16_t mask;
		const char *name;
	} features[] = {
		{ FEAT_EXT_CMDS,		"extended control and status" },
		{ FEAT_WDT_PING,		"watchdog pinging" },
		{ FEAT_LED_STATE_EXT_MASK,	"peripheral LED pins reading" },
		{ FEAT_NEW_INT_API,		"new interrupt API" },
		{ FEAT_POWEROFF_WAKEUP,		"poweroff and wakeup" },
	};
	struct device *dev = &mcu->client->dev;
	bool suggest_fw_upgrade = false;
	int status;

	/* status word holds MCU type, which we need below */
	status = omnia_cmd_read_u16(mcu->client, CMD_GET_STATUS_WORD);
	if (status < 0)
		return status;

	/* check whether MCU firmware supports the CMD_GET_FEAUTRES command */
	if (status & STS_FEATURES_SUPPORTED) {
		int features;

		features = omnia_cmd_read_u16(mcu->client, CMD_GET_FEATURES);
		if (features < 0)
			return features;

		mcu->features = features;
	} else {
		omnia_info_missing_feature(dev, "feature reading");
		suggest_fw_upgrade = true;
	}

	mcu->type = omnia_status_to_mcu_type(status);
	dev_info(dev, "MCU type %s%s\n", mcu->type,
		 (mcu->features & FEAT_PERIPH_MCU) ?
			", with peripheral resets wired" : "");

	omnia_mcu_print_version_hash(mcu, true);

	if (mcu->features & FEAT_BOOTLOADER)
		dev_warn(dev,
			 "MCU is running bootloader firmware. Was firmware upgrade interrupted?\n");
	else
		omnia_mcu_print_version_hash(mcu, false);

	for (unsigned int i = 0; i < ARRAY_SIZE(features); i++) {
		if (mcu->features & features[i].mask)
			continue;

		omnia_info_missing_feature(dev, features[i].name);
		suggest_fw_upgrade = true;
	}

	if (suggest_fw_upgrade)
		dev_info(dev,
			 "Consider upgrading MCU firmware with the omnia-mcutool utility.\n");

	return 0;
}

static int omnia_mcu_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct omnia_mcu *mcu;
	int err;

	if (!client->irq)
		return dev_err_probe(dev, -EINVAL, "IRQ resource not found\n");

	mcu = devm_kzalloc(dev, sizeof(*mcu), GFP_KERNEL);
	if (!mcu)
		return -ENOMEM;

	mcu->client = client;
	i2c_set_clientdata(client, mcu);

	err = omnia_mcu_read_features(mcu);
	if (err)
		return dev_err_probe(dev, err,
				     "Cannot determine MCU supported features\n");

	return 0;
}

static const struct of_device_id of_omnia_mcu_match[] = {
	{ .compatible = "cznic,turris-omnia-mcu" },
	{}
};

static struct i2c_driver omnia_mcu_driver = {
	.probe		= omnia_mcu_probe,
	.driver		= {
		.name	= "turris-omnia-mcu",
		.of_match_table = of_omnia_mcu_match,
		.dev_groups = omnia_mcu_groups,
	},
};

module_i2c_driver(omnia_mcu_driver);

MODULE_AUTHOR("Marek Behun <kabel@kernel.org>");
MODULE_DESCRIPTION("CZ.NIC's Turris Omnia MCU");
MODULE_LICENSE("GPL");
