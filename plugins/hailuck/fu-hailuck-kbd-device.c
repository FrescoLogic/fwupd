/*
 * Copyright (C) 2020 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#include "config.h"

#include <string.h>

#include "fu-chunk.h"
#include "fu-hailuck-common.h"
#include "fu-hailuck-kbd-device.h"
#include "fu-hailuck-kbd-firmware.h"
#include "fu-hailuck-tp-device.h"

struct _FuHaiLuckKbdDevice {
	FuHidDevice		 parent_instance;
};

G_DEFINE_TYPE (FuHaiLuckKbdDevice, fu_hailuck_kbd_device, FU_TYPE_HID_DEVICE)

static gboolean
fu_hailuck_kbd_device_detach (FuDevice *device, GError **error)
{
	guint8 data[6] = {
		FU_HAILUCK_KBD_REPORT_ID_SHORT,
		0x75
	};
	fu_device_set_status (device, FWUPD_STATUS_DEVICE_RESTART);
	if (!fu_hid_device_set_report (FU_HID_DEVICE (device), 0x05,
				       data, sizeof(data), 1000,
				       FU_HID_DEVICE_FLAG_IS_FEATURE, error))
		return FALSE;
	fu_device_add_flag (device, FWUPD_DEVICE_FLAG_WAIT_FOR_REPLUG);
	return TRUE;
}

static gboolean
fu_hailuck_kbd_device_attach (FuDevice *device, GError **error)
{
	guint8 data[6] = {
		FU_HAILUCK_KBD_REPORT_ID_SHORT,
		0x55, 0x55, 0x55, 0x55, 0x55
	};
	fu_device_set_status (device, FWUPD_STATUS_DEVICE_RESTART);
	if (!fu_hid_device_set_report (FU_HID_DEVICE (device), 0x05,
				       data, sizeof(data), 1000,
				       FU_HID_DEVICE_FLAG_IS_FEATURE, error))
		return FALSE;
	if (!g_usb_device_reset (fu_usb_device_get_dev (FU_USB_DEVICE (device)), error))
		return FALSE;
	fu_device_add_flag (device, FWUPD_DEVICE_FLAG_WAIT_FOR_REPLUG);
	return TRUE;
}

static gboolean
fu_hailuck_kbd_device_probe (FuUsbDevice *device, GError **error)
{
	g_autofree gchar *devid = NULL;
	g_autoptr(FuHaiLuckTpDevice) tp_device = fu_hailuck_tp_device_new (FU_DEVICE (device));

	/* add extra keyboard-specific GUID */
	devid = g_strdup_printf ("USB\\VID_%04X&PID_%04X&MODE_KBD",
				 fu_usb_device_get_vid (device),
				 fu_usb_device_get_pid (device));
	fu_device_add_instance_id (FU_DEVICE (device), devid);

	/* add touchpad */
	fu_device_add_child (FU_DEVICE (device), FU_DEVICE (tp_device));

	/* success */
	return TRUE;
}

static gboolean
fu_hailuck_kbd_device_setup (FuDevice *device, GError **error)
{
//	FuHaiLuckKbdDevice *self = FU_HAILUCK_KBD_DEVICE (device);

	/* success */
	return TRUE;
}

static gboolean
fu_hailuck_kbd_device_read_block_start (FuHaiLuckKbdDevice *self,
					guint16 length,
					GError **error)
{
	guint8 buf[6] = {
		FU_HAILUCK_KBD_REPORT_ID_SHORT,
		FU_HAILUCK_CMD_READ_BLOCK_START,
		0x00,
		0x00,
		length & 0xFF,
		(length >> 8) & 0xFF,
	};
	return fu_hid_device_set_report (FU_HID_DEVICE (self), 0x05,
					 buf, sizeof(buf), 100,
					 FU_HID_DEVICE_FLAG_IS_FEATURE, error);
}

static gboolean
fu_hailuck_kbd_device_read_block (FuHaiLuckKbdDevice *self,
				  guint8 *data, gsize data_sz,
				  GError **error)
{
	gsize bufsz = data_sz + 2;
	g_autofree guint8 *buf = g_malloc0 (bufsz);

	buf[0] = FU_HAILUCK_KBD_REPORT_ID_LONG;
	buf[1] = FU_HAILUCK_CMD_READ_BLOCK;
	if (!fu_hid_device_get_report (FU_HID_DEVICE (self), 0x06,
				       buf, bufsz, 2000,
				       FU_HID_DEVICE_FLAG_IS_FEATURE,
				       error))
		return FALSE;
	if (!fu_memcpy_safe (data, data_sz, 0x0,	/* dst */
			     buf, bufsz, 0x02,		/* src */
			     data_sz, error))
		return FALSE;

	/* success */
	g_usleep (10000);
	return TRUE;
}

static GBytes *
fu_hailuck_kbd_device_dump_firmware (FuDevice *device, GError **error)
{
	FuHaiLuckKbdDevice *self = FU_HAILUCK_KBD_DEVICE (device);
	gsize fwsz = fu_device_get_firmware_size_max (device);
	g_autoptr(GByteArray) fwbuf = g_byte_array_new ();
	g_autoptr(GPtrArray) chunks = NULL;

	/* tell device amount of data to send */
	fu_device_set_status (FU_DEVICE (self), FWUPD_STATUS_DEVICE_READ);
	if (!fu_hailuck_kbd_device_read_block_start (self, fwsz, error))
		return FALSE;

	/* recieve data back */
	fu_byte_array_set_size (fwbuf, fwsz);
	chunks = fu_chunk_array_new (fwbuf->data, fwbuf->len, 0x0, 0x0, 2048);
	for (guint i = 0; i < chunks->len; i++) {
		FuChunk *chk = g_ptr_array_index (chunks, i);
		if (!fu_hailuck_kbd_device_read_block (self,
						       (guint8 *) chk->data,
						       chk->data_sz,
						       error))
			return FALSE;
		fu_device_set_progress_full (device, i, chunks->len - 1);
	}

	/* success */
	return g_byte_array_free_to_bytes (g_steal_pointer (&fwbuf));
}

static gboolean
fu_hailuck_kbd_device_erase (FuHaiLuckKbdDevice *self, GError **error)
{
	guint8 buf[6] = {
		FU_HAILUCK_KBD_REPORT_ID_SHORT,
		FU_HAILUCK_CMD_ERASE,
		0x45, 0x45, 0x45, 0x45,
	};
	if (!fu_hid_device_set_report (FU_HID_DEVICE (self), 0x05,
				       buf, sizeof(buf), 100,
				       FU_HID_DEVICE_FLAG_IS_FEATURE, error))
		return FALSE;
	fu_device_sleep_with_progress (FU_DEVICE (self), 2);
	return TRUE;
}

static gboolean
fu_hailuck_kbd_device_write_block_start (FuHaiLuckKbdDevice *self,
					 guint16 length, GError **error)
{
	guint8 buf[6] = {
		FU_HAILUCK_KBD_REPORT_ID_SHORT,
		FU_HAILUCK_CMD_WRITE_BLOCK_START,
		0x00,
		0x00,
		length & 0xFF,
		(length >> 8) & 0xFF,
	};
	return fu_hid_device_set_report (FU_HID_DEVICE (self), 0x05,
					 buf, sizeof(buf), 100,
					 FU_HID_DEVICE_FLAG_IS_FEATURE, error);
}

static gboolean
fu_hailuck_kbd_device_write_block (FuHaiLuckKbdDevice *self,
				   const guint8 *data, gsize data_sz,
				   GError **error)
{
	gsize bufsz = data_sz + 2;
	g_autofree guint8 *buf = g_malloc0 (bufsz);

	buf[0] = FU_HAILUCK_KBD_REPORT_ID_LONG;
	buf[1] = FU_HAILUCK_CMD_WRITE_BLOCK;

	if (!fu_memcpy_safe (buf, bufsz, 0x02,		/* dst */
			     data, data_sz, 0x0,	/* src */
			     data_sz, error))
		return FALSE;
	if (!fu_hid_device_set_report (FU_HID_DEVICE (self), 0x06,
				       buf, bufsz, 2000,
				       FU_HID_DEVICE_FLAG_IS_FEATURE,
				       error))
		return FALSE;

	/* success */
	g_usleep (10000);
	return TRUE;
}

static FuFirmware *
fu_hailuck_kbd_device_prepare_firmware (FuDevice *device,
				     GBytes *fw,
				     FwupdInstallFlags flags,
				     GError **error)
{
	g_autoptr(FuFirmware) firmware = fu_hailuck_kbd_firmware_new ();
	if (!fu_firmware_parse (firmware, fw, flags, error))
		return NULL;
	return g_steal_pointer (&firmware);
}

static gboolean
fu_hailuck_kbd_device_write_firmware (FuDevice *device,
				      FuFirmware *firmware,
				      FwupdInstallFlags flags,
				      GError **error)
{
	FuHaiLuckKbdDevice *self = FU_HAILUCK_KBD_DEVICE (device);
	FuChunk *chk0;
	g_autoptr(GBytes) fw = NULL;
	g_autoptr(GBytes) fw_new = NULL;
	g_autoptr(GPtrArray) chunks = NULL;
	g_autofree guint8 *chk0_data = NULL;

	/* get default image */
	fw = fu_firmware_get_image_default_bytes (firmware, error);
	if (fw == NULL)
		return FALSE;

	/* erase all contents */
	fu_device_set_status (device, FWUPD_STATUS_DEVICE_ERASE);
	if (!fu_hailuck_kbd_device_erase (self, error))
		return FALSE;

	/* tell device amount of data to expect */
	fu_device_set_status (device, FWUPD_STATUS_DEVICE_WRITE);
	if (!fu_hailuck_kbd_device_write_block_start (self, g_bytes_get_size (fw), error))
		return FALSE;

	/* build packets */
	chunks = fu_chunk_array_new_from_bytes (fw, 0x0, 0x00, 2048);

	/* intentionally corrupt first chunk so that CRC fails */
	chk0 = g_ptr_array_index (chunks, 0);
	chk0_data = g_memdup (chk0->data, chk0->data_sz);
	chk0_data[0] = 0x00;
	if (!fu_hailuck_kbd_device_write_block (self, chk0_data, chk0->data_sz, error))
		return FALSE;

	/* send the rest of the chunks */
	for (guint i = 1; i < chunks->len; i++) {
		FuChunk *chk = g_ptr_array_index (chunks, i);
		if (!fu_hailuck_kbd_device_write_block (self, chk->data, chk->data_sz, error))
			return FALSE;
		fu_device_set_progress_full (device, i, chunks->len);
	}

	/* retry write of first block */
	if (!fu_hailuck_kbd_device_write_block_start (self, g_bytes_get_size (fw), error))
		return FALSE;
	if (!fu_hailuck_kbd_device_write_block (self, chk0->data, chk0->data_sz, error))
		return FALSE;
	fu_device_set_progress_full (device, chunks->len, chunks->len);

	/* verify */
	fw_new = fu_hailuck_kbd_device_dump_firmware (device, error);
	return fu_common_bytes_compare (fw, fw_new, error);
}

static void
fu_hailuck_kbd_device_init (FuHaiLuckKbdDevice *self)
{
	fu_device_set_firmware_size (FU_DEVICE (self), 0x4000);
	fu_device_set_protocol (FU_DEVICE (self), "com.simowealth.hailuck");
	fu_device_set_remove_delay (FU_DEVICE (self),
				    FU_DEVICE_REMOVE_DELAY_RE_ENUMERATE);
}

static void
fu_hailuck_kbd_device_class_init (FuHaiLuckKbdDeviceClass *klass)
{
	FuDeviceClass *klass_device = FU_DEVICE_CLASS (klass);
	FuUsbDeviceClass *klass_usb_device = FU_USB_DEVICE_CLASS (klass);
	klass_device->dump_firmware = fu_hailuck_kbd_device_dump_firmware;
	klass_device->prepare_firmware = fu_hailuck_kbd_device_prepare_firmware;
	klass_device->write_firmware = fu_hailuck_kbd_device_write_firmware;
	klass_device->attach = fu_hailuck_kbd_device_attach;
	klass_device->detach = fu_hailuck_kbd_device_detach;
	klass_device->setup = fu_hailuck_kbd_device_setup;
	klass_usb_device->probe = fu_hailuck_kbd_device_probe;
}
