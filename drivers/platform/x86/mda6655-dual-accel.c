// SPDX-License-Identifier: GPL-2.0+

/* 
 * Dual accelerometer driver for MDA6655 devices
 *
 * Copyright 2024, Richard Halkyard <rhalkyard@gmail.com>
 * Copyright 2026, Matthew Garrett <mjg59@srcf.ucam.org>
 * 
 * The MDA6655 is used on 'yoga' style laptop/tablets that use a dual
 * accelerometer setup to determine the angle of its hinge. The Windows driver
 * uses an undocumented ACPI method ('LTSM') to communicate tablet vs. laptop
 * state to the firmware, which in turn issues standard tablet HID events. This
 * driver exposes both MXC6655 accelerometers, and a write-only sysfs property
 * (tablet_mode) that userspace can use to trigger the LTSM method.
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/acpi.h>
#include <linux/bits.h>
#include <linux/dmi.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/sysfs.h>
#include <linux/types.h>

MODULE_DESCRIPTION("MDA6655 Dual Accel device driver");
MODULE_AUTHOR("Richard Halkyard <rhalkyard@gmail.com>");
MODULE_LICENSE("GPL");

struct mda6655 {
	struct i2c_client *lid_mxc6655;
	struct i2c_client *base_mxc6655;
	acpi_handle handle;
};

static acpi_status mda6655_call_ltsm(struct mda6655 *data, int val)
{
	acpi_status ret;
	struct acpi_object_list args;
	union acpi_object mode_arg;

	args.count = 1;
	args.pointer = &mode_arg;

	mode_arg.type = ACPI_TYPE_INTEGER;
	mode_arg.integer.value = val;

	ret = acpi_evaluate_object(data->handle, "LTSM", &args, NULL);
	return ret;
}

static ssize_t mda6655_tablet_mode_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	struct mda6655 *data = dev_get_drvdata(dev);
	int ret;

	switch (buf[0]) {
	case '0':
		ret = mda6655_call_ltsm(data, 0);
		break;
	case '1':
		ret = mda6655_call_ltsm(data, 1);
		break;
	default:
		break;
	}

	if (ret == AE_OK)
		return count;

	dev_err(dev, "Could not call LTSM method: %s\n",
		acpi_format_exception(ret));
	return -EINVAL;
}
static DEVICE_ATTR_WO(mda6655_tablet_mode);

static int mda6655_add(struct acpi_device *adev)
{
	struct fwnode_handle *fwnode = acpi_fwnode_handle(adev);
	struct device *dev = &adev->dev;
	struct i2c_board_info board_info;
	struct mda6655 *data = NULL;
	char name[32];
	int ret;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	dev_set_drvdata(dev, data);

	memset(&board_info, 0, sizeof(board_info));
	strscpy(board_info.type, "mxc4005");
	snprintf(name, sizeof(name), "%s-%s.display", dev_name(dev),
		 board_info.type);
	board_info.dev_name = name;
	data->lid_mxc6655 = i2c_acpi_new_device_by_fwnode(fwnode, 0, &board_info);

	if (IS_ERR(data->lid_mxc6655)) {
		ret = PTR_ERR(data->lid_mxc6655);
		data->lid_mxc6655 = NULL;
		goto out;
	}

	memset(&board_info, 0, sizeof(board_info));
	strscpy(board_info.type, "mxc4005");
	snprintf(name, sizeof(name), "%s-%s.base", dev_name(dev),
		 board_info.type);
	board_info.dev_name = name;
	data->base_mxc6655 = i2c_acpi_new_device_by_fwnode(fwnode, 1, &board_info);

	if (IS_ERR(data->base_mxc6655)) {
		ret = PTR_ERR(data->base_mxc6655);
		data->base_mxc6655 = NULL;
		goto out_unregister_display;
	}

	data->handle = adev->handle;
	if (mda6655_call_ltsm(data, 0) != AE_OK) {
		ret = -ENODEV;
		goto out_unregister_base;
	}

	device_create_file(dev, &(dev_attr_mda6655_tablet_mode));
	return 0;

out_unregister_base:
	i2c_unregister_device(data->base_mxc6655);

out_unregister_display:
	i2c_unregister_device(data->lid_mxc6655);
out:
	kfree(data);
	return ret;
}

static void mda6655_remove(struct acpi_device *adev)
{
	struct mda6655 *data = dev_get_drvdata(&adev->dev);

	device_remove_file(&adev->dev, &(dev_attr_mda6655_tablet_mode));
	mda6655_call_ltsm(data, 0);

	if (data->lid_mxc6655)
		i2c_unregister_device(data->lid_mxc6655);
	if (data->base_mxc6655)
		i2c_unregister_device(data->base_mxc6655);

	kfree(data);
}

static const struct acpi_device_id mda6655_acpi_ids[] = {
	{ "MDA6655", },
	{ }
};
MODULE_DEVICE_TABLE(acpi, mda6655_acpi_ids);

static struct acpi_driver mda6655_driver = {
	.name = "mda6655",
	.class = "MEMSIC",
	.ids = mda6655_acpi_ids,
	.ops = {
		.add = mda6655_add,
		.remove = mda6655_remove,
	},
};
module_acpi_driver(mda6655_driver);
