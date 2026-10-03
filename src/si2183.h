/*
 * Silicon Labs Si2183(2) DVB-T/T2/C/C2/S/S2 demodulator driver,
 * user-space port of linux/drivers/media/dvb-frontends/si2183.c
 * (Luis Alves <ljalvs@gmail.com>). Satellite path only.
 *
 *    This program is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 */
#ifndef SI2183_H
#define SI2183_H

#include <pthread.h>
#include "common.h"

#define SI2183_TS_PARALLEL	0x06
#define SI2183_TS_SERIAL	0x03

struct si2183_config {
	u8 i2c_addr;
	u8 ts_mode;
	bool ts_clock_inv;
	bool ts_clock_gapped;
	int start_clk_mode;	/* 0: terrestrial 1: satellite */
	u8 agc_mode;
	const char *fw_path;	/* dvb-demod-si2183-b60-01.fw */
};

struct si2183 {
	struct si2183_config cfg;
	struct i2c_bus *i2c;		/* parent bus */
	struct i2c_bus tuner_i2c;	/* bus behind the demod I2C gate */
	pthread_mutex_t i2c_mutex;
	bool fw_loaded;
	bool active;
	enum fe_delsys delivery_system;
	u8 stat_resp;

	/* hardware blind scan */
	u32 bs_seek;
	bool bs_started;
};

struct si2183_bs_result {
	u32 frequency;		/* kHz, IF */
	u32 symbol_rate;	/* symbols/s */
	enum fe_delsys delsys;
};

/* Called when the scan engine wants the tuner on another IF frequency (kHz). */
typedef void (*si2183_tune_cb)(void *opaque, u32 frequency);

/* Equivalent of si2183_probe(): no I/O, sets up the gated tuner bus. */
void si2183_attach(struct si2183 *dev, struct i2c_bus *i2c, const struct si2183_config *cfg);

int si2183_init(struct si2183 *dev);
int si2183_sleep(struct si2183 *dev);

/* Must be called after the tuner has been programmed (as si2183_set_frontend does). */
int si2183_set_frontend(struct si2183 *dev, const struct fe_params *c);
int si2183_read_status(struct si2183 *dev, const struct fe_params *c, struct fe_status *st);
/* Satellite AGC level, input for av201x_get_rf_strength(). */
int si2183_read_sat_agc(struct si2183 *dev, u16 *agc);
int si2183_read_ber(struct si2183 *dev, u32 *ber);
int si2183_read_ucblocks(struct si2183 *dev, u32 *ucblocks);

/*
 * Hardware DVB-S/S2 blind scan of the IF range fmin..fmax (kHz). After
 * si2183_blindscan_start(), each si2183_blindscan_next() returns 1 with the
 * next transponder found, 0 at the end of the range, <0 on error.
 */
int si2183_blindscan_start(struct si2183 *dev, u32 fmin, u32 fmax, u32 sr_min, u32 sr_max);
int si2183_blindscan_next(struct si2183 *dev, si2183_tune_cb tune, void *opaque,
			  struct si2183_bs_result *res, volatile int *stop);
void si2183_blindscan_abort(struct si2183 *dev);

int si2183_set_tone(struct si2183 *dev, bool on);
int si2183_diseqc_send_burst(struct si2183 *dev, bool mini_b);
int si2183_diseqc_send_msg(struct si2183 *dev, const u8 *msg, int msg_len);

#endif
