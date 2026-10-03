/*
 * TurboSight TBS 5580 USB bridge (Cypress FX2), user-space port over libusb.
 * Based on linux/drivers/media/usb/dvb-usb/tbs5580.c
 * Copyright (c) 2017 Davin zhang <smiledavin@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, version 2.
 */
#include <stdlib.h>
#include <string.h>
#include "tbs5580.h"

#define TBS5580_READ_MSG 0
#define TBS5580_WRITE_MSG 1

#define USB_TIMEOUT_MS 2000

/* Vendor control transfer, equivalent of tbs5580_op_rw(). Returns bytes or <0. */
static int tbs5580_op_rw(struct tbs5580 *d, u8 request, u16 value, u16 index,
			 u8 *data, u16 len, int flags)
{
	u8 type = LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE |
		  (flags == TBS5580_READ_MSG ? LIBUSB_ENDPOINT_IN : LIBUSB_ENDPOINT_OUT);
	int ret;

	ret = libusb_control_transfer(d->h, type, request, value, index, data, len,
				      USB_TIMEOUT_MS);
	if (g_verbose >= 3) {
		char prefix[64];
		snprintf(prefix, sizeof(prefix), "usb %s req=%02x val=%04x ret=%d:",
			 flags == TBS5580_READ_MSG ? "rd" : "wr", request, value, ret);
		hexdump(prefix, data, ret > 0 ? ret : 0);
	}
	if (ret < 0)
		DBG(1, "usb request 0x%02x failed: %s\n", request, libusb_error_name(ret));
	return ret;
}

static int tbs5580_i2c_xfer(void *priv, struct i2c_msg *msg, int num)
{
	struct tbs5580 *d = priv;
	u8 buf6[64];
	u8 inbuf[64];
	int ret = -EINVAL;
	int i;

	pthread_mutex_lock(&d->lock);

	switch (num) {
	case 2:
		if (msg[1].len > sizeof(inbuf))
			break;
		buf6[0] = msg[1].len;		/* length */
		buf6[1] = msg[0].addr << 1;	/* demod addr */
		buf6[2] = msg[0].buf[0];	/* register */
		ret = tbs5580_op_rw(d, 0x90, 0, 0, buf6, 3, TBS5580_WRITE_MSG);
		if (ret < 0)
			break;
		ret = tbs5580_op_rw(d, 0x91, 0, 0, inbuf, buf6[0], TBS5580_READ_MSG);
		if (ret < 0)
			break;
		memcpy(msg[1].buf, inbuf, msg[1].len);
		break;
	case 1:
		switch (msg[0].addr) {
		case TBS5580_ADDR_DEMOD:
		case TBS5580_ADDR_SAT_TUNER:
		case TBS5580_ADDR_TER_TUNER:
			if (msg[0].flags == 0) {
				if (msg[0].len + 2 > sizeof(buf6))
					break;
				buf6[0] = msg[0].len + 1;	/* length */
				buf6[1] = msg[0].addr << 1;	/* addr */
				for (i = 0; i < msg[0].len; i++)
					buf6[2 + i] = msg[0].buf[i];
				ret = tbs5580_op_rw(d, 0x80, 0, 0, buf6, msg[0].len + 2,
						    TBS5580_WRITE_MSG);
			} else {
				if (msg[0].len > sizeof(inbuf))
					break;
				buf6[0] = msg[0].len;			/* length */
				buf6[1] = (msg[0].addr << 1) | 0x01;	/* addr */
				ret = tbs5580_op_rw(d, 0x93, 0, 0, buf6, 2, TBS5580_WRITE_MSG);
				if (ret < 0)
					break;
				ret = tbs5580_op_rw(d, 0x91, 0, 0, inbuf, buf6[0],
						    TBS5580_READ_MSG);
				if (ret < 0)
					break;
				memcpy(msg[0].buf, inbuf, msg[0].len);
			}
			break;
		}
		break;
	}

	pthread_mutex_unlock(&d->lock);

	if (ret < 0)
		return ret == -EINVAL ? -EINVAL : -EIO;
	return num;
}

static int tbs5580_cmd(struct tbs5580 *d, u8 request, u8 *buf, u16 len, int flags)
{
	int ret;

	pthread_mutex_lock(&d->lock);
	ret = tbs5580_op_rw(d, request, 0, 0, buf, len, flags);
	pthread_mutex_unlock(&d->lock);
	return ret < 0 ? -EIO : 0;
}

static int read_file(const char *path, u8 **data, size_t *size)
{
	FILE *f = fopen(path, "rb");
	long n;

	if (!f)
		return -ENOENT;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	*data = malloc(n > 0 ? n : 1);
	if (!*data) {
		fclose(f);
		return -ENOMEM;
	}
	*size = fread(*data, 1, n, f);
	fclose(f);
	return (long)*size == n ? 0 : -EIO;
}

/* Equivalent of tbs5580_load_firmware(): raw 8051 image written to FX2 RAM. */
static int tbs5580_load_firmware(struct tbs5580 *d, const char *path)
{
	u8 *fw;
	size_t size, i;
	u8 reset;
	int ret = 0;

	ret = read_file(path, &fw, &size);
	if (ret) {
		LOG("cannot read FX2 firmware '%s'\n", path);
		return ret;
	}

	LOG("downloading TBS5580 firmware (%zu bytes)\n", size);

	/* stop the CPU */
	reset = 1;
	tbs5580_op_rw(d, 0xa0, 0x7f92, 0, &reset, 1, TBS5580_WRITE_MSG);
	tbs5580_op_rw(d, 0xa0, 0xe600, 0, &reset, 1, TBS5580_WRITE_MSG);

	for (i = 0; i < size; i += 0x40) {
		u16 chunk = (size - i) < 0x40 ? (u16)(size - i) : 0x40;

		if (tbs5580_op_rw(d, 0xa0, (u16)i, 0, fw + i, chunk,
				  TBS5580_WRITE_MSG) != chunk) {
			LOG("error while transferring firmware\n");
			ret = -EIO;
			break;
		}
	}

	/* restart the CPU */
	reset = 0;
	if (ret || tbs5580_op_rw(d, 0xa0, 0x7f92, 0, &reset, 1, TBS5580_WRITE_MSG) != 1) {
		LOG("could not restart the USB controller CPU\n");
		ret = -EIO;
	}
	if (ret || tbs5580_op_rw(d, 0xa0, 0xe600, 0, &reset, 1, TBS5580_WRITE_MSG) != 1) {
		LOG("could not restart the USB controller CPU\n");
		ret = -EIO;
	}

	msleep(100);
	free(fw);
	return ret;
}

static int tbs5580_reopen(struct tbs5580 *d)
{
	uint64_t deadline = now_ms() + 5000;

	if (d->h) {
		libusb_close(d->h);
		d->h = NULL;
	}
	while (now_ms() < deadline) {
		d->h = libusb_open_device_with_vid_pid(d->ctx, TBS5580_VID, TBS5580_PID);
		if (d->h)
			return 0;
		msleep(200);
	}
	return -ENODEV;
}

/* Select the alternate setting of interface 0 that carries the TS endpoint. */
static int tbs5580_setup_interface(struct tbs5580 *d)
{
	struct libusb_config_descriptor *cfg;
	int ret, alt, e, found = -1;

	ret = libusb_claim_interface(d->h, 0);
	if (ret) {
		LOG("cannot claim USB interface 0: %s\n", libusb_error_name(ret));
		return -EBUSY;
	}
	d->claimed = true;

	if (libusb_get_active_config_descriptor(libusb_get_device(d->h), &cfg) == 0) {
		if (cfg->bNumInterfaces > 0) {
			const struct libusb_interface *intf = &cfg->interface[0];

			for (alt = 0; alt < intf->num_altsetting && found < 0; alt++) {
				const struct libusb_interface_descriptor *id = &intf->altsetting[alt];

				for (e = 0; e < id->bNumEndpoints; e++)
					if (id->endpoint[e].bEndpointAddress == TBS5580_TS_ENDPOINT)
						found = id->bAlternateSetting;
			}
		}
		libusb_free_config_descriptor(cfg);
	}

	if (found < 0) {
		LOG("warning: TS endpoint 0x%02x not found in descriptors\n", TBS5580_TS_ENDPOINT);
	} else if (found != 0) {
		DBG(1, "selecting alternate setting %d\n", found);
		ret = libusb_set_interface_alt_setting(d->h, 0, found);
		if (ret)
			LOG("cannot select alt setting %d: %s\n", found, libusb_error_name(ret));
	}

	/* as usb_urb_init() does */
	libusb_clear_halt(d->h, TBS5580_TS_ENDPOINT);
	return 0;
}

int tbs5580_open(struct tbs5580 *d, const char *fx2_fw_path)
{
	u8 buf[3];
	int ret;

	memset(d, 0, sizeof(*d));
	pthread_mutex_init(&d->lock, NULL);
	d->i2c.xfer = tbs5580_i2c_xfer;
	d->i2c.priv = d;

	ret = libusb_init(&d->ctx);
	if (ret) {
		LOG("libusb_init failed: %s\n", libusb_error_name(ret));
		return -EIO;
	}

	d->h = libusb_open_device_with_vid_pid(d->ctx, TBS5580_VID, TBS5580_PID);
	if (!d->h) {
		LOG("TBS5580 (%04x:%04x) not found\n", TBS5580_VID, TBS5580_PID);
		return -ENODEV;
	}

	if (fx2_fw_path) {
		ret = tbs5580_load_firmware(d, fx2_fw_path);
		if (ret)
			return ret;

		/*
		 * The Linux driver sets no_reconnect, i.e. the device keeps
		 * running without re-enumerating. Be tolerant anyway.
		 */
		if (tbs5580_op_rw(d, 0xa8, 0, 0, buf, 3, TBS5580_READ_MSG) ==
		    LIBUSB_ERROR_NO_DEVICE) {
			LOG("device re-enumerated after firmware load, reopening\n");
			ret = tbs5580_reopen(d);
			if (ret) {
				LOG("device did not come back\n");
				return ret;
			}
		}
	}

	return tbs5580_setup_interface(d);
}

void tbs5580_close(struct tbs5580 *d)
{
	tbs5580_stop_stream(d);
	if (d->h) {
		if (d->claimed)
			libusb_release_interface(d->h, 0);
		libusb_close(d->h);
		d->h = NULL;
	}
	if (d->ctx) {
		libusb_exit(d->ctx);
		d->ctx = NULL;
	}
	pthread_mutex_destroy(&d->lock);
}

/* CI slot status, as tbs5580_poll_slot_status(). */
static bool tbs5580_cam_present(struct tbs5580 *d)
{
	u8 buf[3] = { 0 };

	if (tbs5580_cmd(d, 0xa8, buf, 3, TBS5580_READ_MSG))
		return false;
	return buf[0] == 0xa9 && buf[1] == 1 && buf[2] == 1;
}

int tbs5580_board_init(struct tbs5580 *d)
{
	u8 buf[2];
	int ret = 0;

	/* sequence from tbs5580_frontend_attach() */
	buf[0] = 1;
	buf[1] = 0;
	ret |= tbs5580_cmd(d, 0x8a, buf, 2, TBS5580_WRITE_MSG);

	buf[0] = 0;
	buf[1] = 0;
	ret |= tbs5580_cmd(d, 0xb7, buf, 2, TBS5580_WRITE_MSG);
	buf[0] = 8;
	buf[1] = 1;
	ret |= tbs5580_cmd(d, 0x8a, buf, 2, TBS5580_WRITE_MSG);

	/*
	 * CI support (EN50221) is not ported yet: route the TS around the
	 * CAM slot, as tbs5580_init() does when no CAM is inserted.
	 */
	if (tbs5580_cam_present(d))
		LOG("CAM detected in CI slot: not supported yet, bypassing it\n");
	buf[0] = 2;
	buf[1] = 0;
	ret |= tbs5580_cmd(d, 0xa6, buf, 2, TBS5580_WRITE_MSG);

	return ret ? -EIO : 0;
}

int tbs5580_set_voltage(struct tbs5580 *d, bool v18)
{
	u8 buf[2] = { 3, v18 ? 1 : 0 };

	return tbs5580_cmd(d, 0x8a, buf, 2, TBS5580_WRITE_MSG);
}

int tbs5580_read_eeprom(struct tbs5580 *d, u8 eeprom[256])
{
	u8 ibuf[3];
	int i, ret = 0;

	pthread_mutex_lock(&d->lock);
	for (i = 0; i < 256; i++) {
		ibuf[0] = 1;			/* length */
		ibuf[1] = TBS5580_ADDR_EEPROM;	/* eeprom addr */
		ibuf[2] = i;			/* register */
		if (tbs5580_op_rw(d, 0x90, 0, 0, ibuf, 3, TBS5580_WRITE_MSG) < 0 ||
		    tbs5580_op_rw(d, 0x91, 0, 0, ibuf, 1, TBS5580_READ_MSG) < 0) {
			ret = -EIO;
			break;
		}
		eeprom[i] = ibuf[0];
	}
	pthread_mutex_unlock(&d->lock);
	return ret;
}

void tbs5580_print_usb_info(struct tbs5580 *d)
{
	libusb_device *dev = libusb_get_device(d->h);
	struct libusb_device_descriptor dd;
	struct libusb_config_descriptor *cfg;
	static const char *speeds[] = { "unknown", "low", "full", "high", "super", "super+" };
	int speed = libusb_get_device_speed(dev);
	int i, a, e;

	libusb_get_device_descriptor(dev, &dd);
	LOG("USB %04x:%04x bus %d addr %d, speed %s, bcdDevice %04x\n",
	    dd.idVendor, dd.idProduct, libusb_get_bus_number(dev),
	    libusb_get_device_address(dev),
	    speed >= 0 && speed < (int)ARRAY_SIZE(speeds) ? speeds[speed] : "?",
	    dd.bcdDevice);

	if (libusb_get_active_config_descriptor(dev, &cfg))
		return;
	for (i = 0; i < cfg->bNumInterfaces; i++) {
		for (a = 0; a < cfg->interface[i].num_altsetting; a++) {
			const struct libusb_interface_descriptor *id = &cfg->interface[i].altsetting[a];

			LOG("  interface %d alt %d class %02x\n", id->bInterfaceNumber,
			    id->bAlternateSetting, id->bInterfaceClass);
			for (e = 0; e < id->bNumEndpoints; e++) {
				const struct libusb_endpoint_descriptor *ep = &id->endpoint[e];
				static const char *types[] = { "control", "iso", "bulk", "interrupt" };

				LOG("    endpoint 0x%02x %s maxpacket %d\n", ep->bEndpointAddress,
				    types[ep->bmAttributes & 3], ep->wMaxPacketSize);
			}
		}
	}
	libusb_free_config_descriptor(cfg);
}

/* ---- streaming ---------------------------------------------------------- */

static void LIBUSB_CALL tbs5580_xfer_cb(struct libusb_transfer *t)
{
	struct tbs5580 *d = t->user_data;

	if (t->status == LIBUSB_TRANSFER_COMPLETED) {
		if (t->actual_length > 0 && d->data_cb)
			d->data_cb(t->buffer, t->actual_length, d->data_opaque);
		if (d->streaming && libusb_submit_transfer(t) == 0)
			return;
	} else if (t->status != LIBUSB_TRANSFER_CANCELLED) {
		LOG("TS transfer error: %s\n", libusb_error_name(t->status));
		if (d->streaming && t->status != LIBUSB_TRANSFER_NO_DEVICE &&
		    libusb_submit_transfer(t) == 0)
			return;
	}
	__sync_fetch_and_sub(&d->active_xfers, 1);
}

static void *tbs5580_event_thread(void *arg)
{
	struct tbs5580 *d = arg;
	struct timeval tv = { 0, 100000 };

	while (d->event_thread_running)
		libusb_handle_events_timeout_completed(d->ctx, &tv, NULL);
	return NULL;
}

int tbs5580_start_stream(struct tbs5580 *d, int n_xfers, int xfer_size,
			 tbs5580_data_cb cb, void *opaque)
{
	int i, ret;

	d->data_cb = cb;
	d->data_opaque = opaque;
	d->n_xfers = n_xfers;
	d->xfer_size = xfer_size;
	d->xfers = calloc(n_xfers, sizeof(*d->xfers));
	if (!d->xfers)
		return -ENOMEM;

	d->streaming = true;
	for (i = 0; i < n_xfers; i++) {
		u8 *buf = malloc(xfer_size);

		d->xfers[i] = libusb_alloc_transfer(0);
		if (!buf || !d->xfers[i]) {
			free(buf);
			ret = -ENOMEM;
			goto err;
		}
		libusb_fill_bulk_transfer(d->xfers[i], d->h, TBS5580_TS_ENDPOINT, buf,
					  xfer_size, tbs5580_xfer_cb, d, 0);
		d->xfers[i]->flags = LIBUSB_TRANSFER_FREE_BUFFER;
		ret = libusb_submit_transfer(d->xfers[i]);
		if (ret) {
			LOG("cannot submit TS transfer: %s\n", libusb_error_name(ret));
			ret = -EIO;
			goto err;
		}
		__sync_fetch_and_add(&d->active_xfers, 1);
	}

	d->event_thread_running = true;
	if (pthread_create(&d->event_thread, NULL, tbs5580_event_thread, d)) {
		d->event_thread_running = false;
		ret = -ENOMEM;
		goto err;
	}
	return 0;
err:
	tbs5580_stop_stream(d);
	return ret;
}

void tbs5580_stop_stream(struct tbs5580 *d)
{
	struct timeval tv = { 0, 50000 };
	uint64_t deadline;
	int i;

	if (!d->xfers)
		return;

	d->streaming = false;
	for (i = 0; i < d->n_xfers; i++)
		if (d->xfers[i])
			libusb_cancel_transfer(d->xfers[i]);

	if (d->event_thread_running) {
		d->event_thread_running = false;
		pthread_join(d->event_thread, NULL);
	}

	deadline = now_ms() + 2000;
	while (d->active_xfers > 0 && now_ms() < deadline)
		libusb_handle_events_timeout_completed(d->ctx, &tv, NULL);

	if (d->active_xfers == 0) {
		for (i = 0; i < d->n_xfers; i++)
			if (d->xfers[i])
				libusb_free_transfer(d->xfers[i]);
	}
	free(d->xfers);
	d->xfers = NULL;
	d->n_xfers = 0;
}
