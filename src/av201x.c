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
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 */
#include <string.h>
#include "av201x.h"

enum av201x_regs_addr {
	REG_FN		= 0x00,
	REG_BWFILTER	= 0x05,
	REG_TUNER_STAT	= 0x0b,
	REG_TUNER_CTRL	= 0x0c,
	REG_FT_CTRL	= 0x25,
};

/* REG_TUNER_STAT */
#define AV201X_PLLLOCK		(1<<0)

/* REG_TUNER_CTRL */
#define AV201X_SLEEP		(1<<5)
#define AV201X_RFLP		(1<<6)

/* REG_FT_CTRL */
#define AV201X_FT_EN		(1<<1)
#define AV201X_FT_BLK		(1<<2)

struct av201x_regtable {
	u8 addr;
	u8 setmask;
	u8 clrmask;
	int sleep;
};

static const struct av201x_regtable av201x_inittuner0[] = {
	{0x00, 0x38, 0xff, 0},
	{0x01, 0x00, 0xff, 0},
	{0x02, 0x00, 0xff, 0},
	{0x03, 0x50, 0xff, 0},
	{0x04, 0x1f, 0xff, 0},
	{0x05, 0xa3, 0xff, 0},
	{0x06, 0xfd, 0xff, 0},
	{0x07, 0x58, 0xff, 0},
	{0x08, 0x36, 0xff, 0},
	{0x09, 0xc2, 0xff, 0},
	{0x0a, 0x88, 0xff, 0},
	{0x0b, 0xb4, 0xff, 20},
	{0x0d, 0x40, 0xff, 0},
};

static const struct av201x_regtable av201x_inittuner1a[] = {
	{0x0e, 0x94, 0xff, 0},
	{0x0f, 0x9a, 0xff, 0},
};

static const struct av201x_regtable av201x_inittuner1b[] = {
	{0x0e, 0x5b, 0xff, 0},
	{0x0f, 0x6a, 0xff, 0},
};

static const struct av201x_regtable av201x_inittuner2[] = {
	{0x10, 0x66, 0xff, 0},
	{0x11, 0x40, 0xff, 0},
	{0x12, 0x80, 0xff, 0},
	{0x13, 0x2b, 0xff, 0},
	{0x14, 0x6a, 0xff, 0},
	{0x15, 0x50, 0xff, 0},
	{0x16, 0x91, 0xff, 0},
	{0x17, 0x27, 0xff, 0},
	{0x18, 0x8f, 0xff, 0},
	{0x19, 0xcc, 0xff, 0},
	{0x1a, 0x21, 0xff, 0},
	{0x1b, 0x10, 0xff, 0},
	{0x1c, 0x80, 0xff, 0},
	{0x1d, 0x02, 0xff, 0},
	{0x1e, 0xf5, 0xff, 0},
	{0x1f, 0x7f, 0xff, 0},
	{0x20, 0x4a, 0xff, 0},
	{0x21, 0x9b, 0xff, 0},
	{0x22, 0xe0, 0xff, 0},
	{0x23, 0xe0, 0xff, 0},
	{0x24, 0x36, 0xff, 0},
	{0x25, 0x00, 0xff, 0},
	{0x26, 0xab, 0xff, 0},
	{0x27, 0x97, 0xff, 0},
	{0x28, 0xc5, 0xff, 0},
	{0x29, 0xa8, 0xff, 20},
};

/* write multiple (continuous) registers */
static int av201x_wrm(struct av201x *priv, u8 *buf, int len)
{
	int ret;
	struct i2c_msg msg = {
		.addr = priv->cfg.i2c_address,
		.flags = 0, .buf = buf, .len = len };

	DBG(2, "av201x: i2c wrm @0x%02x (len=%d)\n", buf[0], len);

	ret = i2c_transfer(priv->i2c, &msg, 1);
	if (ret < 0) {
		LOG("av201x: i2c wrm err(%i) @0x%02x (len=%d)\n", ret, buf[0], len);
		return ret;
	}
	return 0;
}

/* write one register */
static int av201x_wr(struct av201x *priv, u8 addr, u8 data)
{
	u8 buf[] = { addr, data };

	return av201x_wrm(priv, buf, 2);
}

/* read multiple (continuous) registers starting at addr */
static int av201x_rdm(struct av201x *priv, u8 addr, u8 *buf, int len)
{
	int ret;
	struct i2c_msg msg[] = {
		{ .addr = priv->cfg.i2c_address, .flags = 0,
			.buf = &addr, .len = 1 },
		{ .addr = priv->cfg.i2c_address, .flags = I2C_M_RD,
			.buf = buf, .len = len }
	};

	DBG(2, "av201x: i2c rdm @0x%02x (len=%d)\n", addr, len);

	ret = i2c_transfer(priv->i2c, msg, 2);
	if (ret < 0) {
		LOG("av201x: i2c rdm err(%i) @0x%02x (len=%d)\n", ret, addr, len);
		return ret;
	}
	return 0;
}

/* read one register */
static int av201x_rd(struct av201x *priv, u8 addr, u8 *data)
{
	return av201x_rdm(priv, addr, data, 1);
}

/* read register, apply masks, write back */
static int av201x_regmask(struct av201x *priv, u8 reg, u8 setmask, u8 clrmask)
{
	int ret;
	u8 b = 0;

	if (clrmask != 0xff) {
		ret = av201x_rd(priv, reg, &b);
		if (ret)
			return ret;
		b &= ~clrmask;
	}
	return av201x_wr(priv, reg, b | setmask);
}

static int av201x_wrtable(struct av201x *priv, const struct av201x_regtable *regtable, int len)
{
	int ret, i;

	for (i = 0; i < len; i++) {
		ret = av201x_regmask(priv, regtable[i].addr,
			regtable[i].setmask, regtable[i].clrmask);
		if (ret)
			return ret;
		if (regtable[i].sleep)
			msleep(regtable[i].sleep);
	}
	return 0;
}

int av201x_init(struct av201x *priv)
{
	int ret;

	ret = av201x_wrtable(priv, av201x_inittuner0, ARRAY_SIZE(av201x_inittuner0));

	switch (priv->cfg.id) {
	case ID_AV2011:
		ret |= av201x_wrtable(priv, av201x_inittuner1a, ARRAY_SIZE(av201x_inittuner1a));
		break;
	case ID_AV2012:
	default:
		ret |= av201x_wrtable(priv, av201x_inittuner1b, ARRAY_SIZE(av201x_inittuner1b));
		break;
	}

	ret |= av201x_wrtable(priv, av201x_inittuner2, ARRAY_SIZE(av201x_inittuner2));

	ret |= av201x_wr(priv, REG_TUNER_CTRL, 0x96);

	msleep(120);

	if (ret)
		LOG("av201x: init failed\n");
	return ret;
}

int av201x_sleep(struct av201x *priv)
{
	int ret = av201x_regmask(priv, REG_TUNER_CTRL, AV201X_SLEEP, 0);

	if (ret)
		DBG(1, "av201x: sleep failed\n");
	return ret;
}

int av201x_set_params(struct av201x *priv, const struct fe_params *c)
{
	u32 n, bw, bf;
	u8 buf[5];
	int ret;

	DBG(1, "av201x: frequency=%d symbol_rate=%d\n", c->frequency, c->symbol_rate);

	/*
	   ** PLL setup **
	   RF = (pll_N * ref_freq) / pll_M
	   pll_M = fixed 0x10000
	   PLL output is divided by 2
	   REG_FN = pll_M<24:0>
	*/
	buf[0] = REG_FN;
	n = DIV_ROUND_CLOSEST(c->frequency, priv->cfg.xtal_freq);
	buf[1] = (n > 0xff) ? 0xff : (u8) n;
	n = DIV_ROUND_CLOSEST((c->frequency / 1000) << 17, priv->cfg.xtal_freq / 1000);
	buf[2] = (u8) (n >> 9);
	buf[3] = (u8) (n >> 1);
	buf[4] = (u8) (((n << 7) & 0x80) | 0x50);
	ret = av201x_wrm(priv, buf, 5);
	if (ret)
		goto exit;

	msleep(20);

	/* set bandwidth */
	bw = (c->symbol_rate / 1000) * 135/200;
	if (c->symbol_rate < 6500000)
		bw += 6000;
	bw += 2000;
	bw *= 108/100;	/* integer math: no-op, kept as in the Linux driver */

	/* check limits (4MHz < bw < 40MHz) */
	if (bw > 40000)
		bw = 40000;
	else if (bw < 4000)
		bw = 4000;

	/* bandwidth step = 211kHz */
	bf = DIV_ROUND_CLOSEST(bw * 127, 21100);
	ret = av201x_wr(priv, REG_BWFILTER, (u8) bf);

	/* enable fine tune agc */
	ret |= av201x_wr(priv, REG_FT_CTRL, AV201X_FT_EN | AV201X_FT_BLK);

	ret |= av201x_wr(priv, REG_TUNER_CTRL, 0x96);
	msleep(20);
exit:
	if (ret)
		LOG("av201x: set_params failed\n");
	return ret;
}

static const int AV201x_agc[]          = {  0,  82,  100,  116,  140,  162,  173,  187,  210,  223,  254,  255};
static const int AV201x_level_dBm_10[] = { 90, -50, -263, -361, -463, -563, -661, -761, -861, -891, -904, -910};

s32 av201x_get_rf_strength(u16 agc)
{
	int if_agc = agc, index, slope;
	const int *x = AV201x_agc, *y = AV201x_level_dBm_10;
	int table_length = ARRAY_SIZE(AV201x_agc);

	/* Finding in which segment the if_agc value is */
	for (index = 1; index < table_length - 1; index++)
		if (x[index] > if_agc)
			break;

	/* Computing segment slope */
	slope = ((y[index] - y[index - 1]) * 1000) / (x[index] - x[index - 1]);
	/* Linear approximation of rssi value in segment (0.1 dBm units) */
	return (y[index - 1] + ((if_agc - x[index - 1]) * slope + 500) / 1000) * 100;
}

void av201x_attach(struct av201x *priv, struct i2c_bus *i2c, const struct av201x_config *cfg)
{
	memset(priv, 0, sizeof(*priv));
	priv->cfg = *cfg;
	priv->i2c = i2c;
}
