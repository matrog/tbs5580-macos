/*
 * TurboSight TBS 5580 USB bridge (Cypress FX2), user-space port over libusb.
 * Based on linux/drivers/media/usb/dvb-usb/tbs5580.c
 * Copyright (c) 2017 Davin zhang <smiledavin@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, version 2.
 */
#ifndef TBS5580_H
#define TBS5580_H

#include <stddef.h>
#include <pthread.h>
#include <libusb.h>
#include "common.h"

#define TBS5580_VID 0x734c
#define TBS5580_PID 0x5580

#define TBS5580_TS_ENDPOINT 0x82

/* I2C addresses on the FX2 bus */
#define TBS5580_ADDR_DEMOD	0x67	/* Si2183 */
#define TBS5580_ADDR_SAT_TUNER	0x62	/* AV2018, behind the demod I2C gate */
#define TBS5580_ADDR_TER_TUNER	0x61	/* Si2157, behind the demod I2C gate */
#define TBS5580_ADDR_EEPROM	0xa0	/* 8-bit address, as used by the FX2 firmware */

typedef void (*tbs5580_data_cb)(const u8 *buf, int len, void *opaque);

struct tbs5580 {
	libusb_context *ctx;
	libusb_device_handle *h;
	bool claimed;
	pthread_mutex_t lock;	/* serializes vendor requests (ca_mutex + i2c_mutex) */
	struct i2c_bus i2c;

	/* streaming */
	struct libusb_transfer **xfers;
	int n_xfers;
	int xfer_size;
	volatile int active_xfers;
	volatile bool streaming;
	pthread_t event_thread;
	bool event_thread_running;
	tbs5580_data_cb data_cb;
	void *data_opaque;
};

int tbs5580_open(struct tbs5580 *d, const char *fx2_fw_path);
void tbs5580_close(struct tbs5580 *d);

/* Board setup done by tbs5580_frontend_attach() after the chips are attached. */
int tbs5580_board_init(struct tbs5580 *d);

/* LNB voltage: true = 18V (horizontal), false = 13V (vertical). */
int tbs5580_set_voltage(struct tbs5580 *d, bool v18);

int tbs5580_read_eeprom(struct tbs5580 *d, u8 eeprom[256]);
void tbs5580_print_usb_info(struct tbs5580 *d);

int tbs5580_start_stream(struct tbs5580 *d, int n_xfers, int xfer_size,
			 tbs5580_data_cb cb, void *opaque);
void tbs5580_stop_stream(struct tbs5580 *d);

#endif
