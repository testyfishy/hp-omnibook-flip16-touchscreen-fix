/*
 * HID over I2C ACPI Subclass
 *
 * Copyright (c) 2012 Benjamin Tissoires <benjamin.tissoires@gmail.com>
 * Copyright (c) 2012 Ecole Nationale de l'Aviation Civile, France
 * Copyright (c) 2012 Red Hat, Inc
 *
 * This code was forked out of the core code, which was partly based on
 * "USB HID support for Linux":
 *
 *  Copyright (c) 1999 Andreas Gal
 *  Copyright (c) 2000-2005 Vojtech Pavlik <vojtech@suse.cz>
 *  Copyright (c) 2005 Michael Haboustak <mike-@cinci.rr.com> for Concept2, Inc
 *  Copyright (c) 2007-2008 Oliver Neukum
 *  Copyright (c) 2006-2010 Jiri Kosina
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file COPYING in the main directory of this archive for
 * more details.
 */

#include <linux/acpi.h>
#include <linux/device.h>
#include <linux/dmi.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pm.h>
#include <linux/uuid.h>

#include "i2c-hid.h"

struct i2c_hid_acpi {
	struct i2chid_ops ops;
	struct acpi_device *adev;
};

static const struct acpi_device_id i2c_hid_acpi_blacklist[] = {
	/*
	 * The CHPN0001 ACPI device, which is used to describe the Chipone
	 * ICN8505 controller, has a _CID of PNP0C50 but is not HID compatible.
	 */
	{ "CHPN0001" },
	/*
	 * The IDEA5002 ACPI device causes high interrupt usage and spurious
	 * wakeups from suspend.
	 */
	{ "IDEA5002" },
	{ }
};

/* HID I²C Device: 3cdff6f7-4267-4555-ad05-b30a3d8938de */
static guid_t i2c_hid_guid =
	GUID_INIT(0x3CDFF6F7, 0x4267, 0x4555,
		  0xAD, 0x05, 0xB3, 0x0A, 0x3D, 0x89, 0x38, 0xDE);

static int i2c_hid_acpi_get_descriptor(struct i2c_hid_acpi *ihid_acpi)
{
	struct acpi_device *adev = ihid_acpi->adev;
	acpi_handle handle = acpi_device_handle(adev);
	union acpi_object *obj;
	u16 hid_descriptor_address;

	if (acpi_match_device_ids(adev, i2c_hid_acpi_blacklist) == 0)
		return -ENODEV;

	obj = acpi_evaluate_dsm_typed(handle, &i2c_hid_guid, 1, 1, NULL,
				      ACPI_TYPE_INTEGER);
	if (!obj) {
		acpi_handle_err(handle, "Error _DSM call to get HID descriptor address failed\n");
		return -ENODEV;
	}

	hid_descriptor_address = obj->integer.value;
	ACPI_FREE(obj);

	return hid_descriptor_address;
}

/*
 * HP OmniBook X Flip 16 (boards 8DA0/8DA1, Lunar Lake, ELAN2514 touchscreen):
 * the touch panel's ACPI power resource _ON (PTPL) evaluates
 * \_SB.SGRA(TPI2, TPIP) with TPIP = 0, which clears PADCFG0 bit 20
 * (GPIROUTIOXAPIC) of the INT pad GPP_E_18.  Linux evaluates _ON at boot
 * and on every D3->D0 transition, so the IO-APIC input floats asserted:
 * a ~42k IRQ/s storm and touch never reaches the CPU.  Re-set the bit with
 * the firmware's own helper before the core talks to the device.
 */
static const struct dmi_system_id i2c_hid_acpi_hp_elan_irq_route[] = {
	{ .matches = { DMI_MATCH(DMI_SYS_VENDOR, "HP"), DMI_MATCH(DMI_BOARD_NAME, "8DA1") } },
	{ .matches = { DMI_MATCH(DMI_SYS_VENDOR, "HP"), DMI_MATCH(DMI_BOARD_NAME, "8DA0") } },
	{ }
};

static void i2c_hid_acpi_hp_route_irq(struct i2c_hid_acpi *ihid_acpi)
{
	acpi_handle handle = acpi_device_handle(ihid_acpi->adev);
	union acpi_object args[2] = {
		{ .integer = { .type = ACPI_TYPE_INTEGER } },
		{ .integer = { .type = ACPI_TYPE_INTEGER, .value = 1 } },
	};
	struct acpi_object_list arg_list = { .count = 2, .pointer = args };
	unsigned long long gpli;
	acpi_status status;

	if (!dmi_check_system(i2c_hid_acpi_hp_elan_irq_route))
		return;
	if (strcmp(acpi_device_hid(ihid_acpi->adev), "ELAN2514"))
		return;
	if (!acpi_has_method(NULL, "\\_SB.SGRA"))
		return;
	status = acpi_evaluate_integer(NULL, "\\GPLI", NULL, &gpli);
	if (ACPI_FAILURE(status))
		return;
	args[0].integer.value = gpli;
	status = acpi_evaluate_object(NULL, "\\_SB.SGRA", &arg_list, NULL);
	acpi_handle_info(handle, "HP quirk: routed touch INT pad 0x%llx to the IO-APIC (%s)\n",
			 gpli, acpi_format_exception(status));
}

static int i2c_hid_acpi_power_up(struct i2chid_ops *ops)
{
	struct i2c_hid_acpi *ihid_acpi = container_of(ops, struct i2c_hid_acpi, ops);

	i2c_hid_acpi_hp_route_irq(ihid_acpi);
	return 0;
}

static void i2c_hid_acpi_restore_sequence(struct i2chid_ops *ops)
{
	struct i2c_hid_acpi *ihid_acpi = container_of(ops, struct i2c_hid_acpi, ops);

	i2c_hid_acpi_get_descriptor(ihid_acpi);
}

static void i2c_hid_acpi_shutdown_tail(struct i2chid_ops *ops)
{
	struct i2c_hid_acpi *ihid_acpi = container_of(ops, struct i2c_hid_acpi, ops);

	acpi_device_set_power(ihid_acpi->adev, ACPI_STATE_D3_COLD);
}

static int i2c_hid_acpi_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct i2c_hid_acpi *ihid_acpi;
	u16 hid_descriptor_address;
	int ret;

	ihid_acpi = devm_kzalloc(&client->dev, sizeof(*ihid_acpi), GFP_KERNEL);
	if (!ihid_acpi)
		return -ENOMEM;

	ihid_acpi->adev = ACPI_COMPANION(dev);
	ihid_acpi->ops.power_up = i2c_hid_acpi_power_up;
	ihid_acpi->ops.shutdown_tail = i2c_hid_acpi_shutdown_tail;
	ihid_acpi->ops.restore_sequence = i2c_hid_acpi_restore_sequence;

	ret = i2c_hid_acpi_get_descriptor(ihid_acpi);
	if (ret < 0)
		return ret;
	hid_descriptor_address = ret;

	acpi_device_fix_up_power(ihid_acpi->adev);

	return i2c_hid_core_probe(client, &ihid_acpi->ops,
				  hid_descriptor_address, 0);
}

static const struct acpi_device_id i2c_hid_acpi_match[] = {
	{ "ACPI0C50" },
	{ "PNP0C50" },
	{ }
};
MODULE_DEVICE_TABLE(acpi, i2c_hid_acpi_match);

static struct i2c_driver i2c_hid_acpi_driver = {
	.driver = {
		.name	= "i2c_hid_acpi",
		.pm	= &i2c_hid_core_pm,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
		.acpi_match_table = i2c_hid_acpi_match,
	},

	.probe		= i2c_hid_acpi_probe,
	.remove		= i2c_hid_core_remove,
	.shutdown	= i2c_hid_core_shutdown,
};

module_i2c_driver(i2c_hid_acpi_driver);

MODULE_DESCRIPTION("HID over I2C ACPI driver");
MODULE_AUTHOR("Benjamin Tissoires <benjamin.tissoires@gmail.com>");
MODULE_LICENSE("GPL");
