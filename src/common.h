/*
 * Small shared helpers replacing the bits of the Linux kernel API used by
 * the original TBS drivers (i2c_msg, msleep, logging).
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, version 2.
 */
#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <errno.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t s32;

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define DIV_ROUND_CLOSEST(x, d) (((x) + ((d) / 2)) / (d))

#define I2C_M_RD 0x0001

struct i2c_msg {
	u16 addr;
	u16 flags;
	u16 len;
	u8 *buf;
};

/* An I2C bus: either the FX2 bus itself or the tuner bus behind the demod gate. */
struct i2c_bus {
	int (*xfer)(void *priv, struct i2c_msg *msgs, int num);
	void *priv;
};

static inline int i2c_transfer(struct i2c_bus *bus, struct i2c_msg *msgs, int num)
{
	return bus->xfer(bus->priv, msgs, num);
}

extern int g_verbose;

#define LOG(...) fprintf(stderr, __VA_ARGS__)
#define DBG(lvl, ...) do { if (g_verbose >= (lvl)) fprintf(stderr, __VA_ARGS__); } while (0)

void msleep(unsigned int ms);
uint64_t now_ms(void);
void hexdump(const char *prefix, const u8 *buf, int len);

/* Delivery systems supported by this port (satellite path only for now). */
enum fe_delsys {
	DELSYS_NONE = 0,
	DELSYS_DVBS,
	DELSYS_DVBS2,
	DELSYS_DSS,
};

#define NO_STREAM_ID_FILTER (~0U)

enum fe_inversion {
	INVERSION_OFF,
	INVERSION_ON,
	INVERSION_AUTO,
};

/* Equivalent of the subset of dtv_frontend_properties used by si2183/av201x. */
struct fe_params {
	enum fe_delsys delsys;
	u32 frequency;		/* kHz, IF frequency for satellite (950..2150 MHz) */
	u32 symbol_rate;	/* symbols/s */
	enum fe_inversion inversion;
	u32 stream_id;		/* ISI | PLS code << 8 | PLS mode << 26 */
};

struct fe_status {
	bool signal;
	bool carrier;
	bool lock;
	bool cnr_valid;
	s32 cnr_mdb;		/* 0.001 dB */
	bool strength_valid;
	s32 strength_mdbm;	/* 0.001 dBm */
	const char *modulation;
	const char *fec;
	const char *rolloff;
	const char *pilot;
};

#endif
