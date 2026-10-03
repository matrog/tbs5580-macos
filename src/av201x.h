/*
 * AV201x Airoha Technology silicon tuner driver,
 * user-space port of linux/drivers/media/tuners/av201x.c
 *
 * Copyright (C) 2014 Luis Alves <ljalvs@gmail.com>
 *
 *    This program is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 */
#ifndef AV201X_H
#define AV201X_H

#include "common.h"

typedef enum av201x_id {
	ID_AV2011,
	ID_AV2012,
	ID_AV2018,
} av201x_id_t;

struct av201x_config {
	/* tuner i2c address */
	u8 i2c_address;
	/* tuner type */
	av201x_id_t id;
	/* crystal freq in kHz */
	u32 xtal_freq;
};

struct av201x {
	struct av201x_config cfg;
	struct i2c_bus *i2c;
};

void av201x_attach(struct av201x *priv, struct i2c_bus *i2c, const struct av201x_config *cfg);
int av201x_init(struct av201x *priv);
int av201x_sleep(struct av201x *priv);
int av201x_set_params(struct av201x *priv, const struct fe_params *c);
/* Converts the demod AGC level into RF input level, 0.001 dBm. */
s32 av201x_get_rf_strength(u16 agc);

#endif
