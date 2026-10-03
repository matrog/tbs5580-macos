/*
 * TBS 5580 satellite frontend: FX2 bridge + Si2183 demod + AV2018 tuner,
 * plus LNB/DiSEqC/rotor handling. Mirrors what dvb-core does on Linux.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, version 2.
 */
#ifndef FRONTEND_H
#define FRONTEND_H

#include <signal.h>
#include "tbs5580.h"
#include "si2183.h"
#include "av201x.h"
#include "rotor.h"

/* Set by SIGINT/SIGTERM, makes long operations return early. */
extern volatile sig_atomic_t g_stop;
/* Set to abort the current blind scan (TUI Esc), without quitting. */
extern volatile sig_atomic_t g_scan_stop;

struct lnb {
	u32 lof1, lof2, slof;	/* kHz */
};

struct fe_config {
	const char *fw_dir;
	bool no_fw;
	bool info;		/* print USB/EEPROM details while opening */
	struct lnb lnb;
	int diseqc;		/* 0 = none, 1..4 committed port */
	enum fe_inversion inversion;
	struct rotor_config rotor;
};

struct tune_req {
	u32 freq_khz;		/* transponder frequency */
	bool pol_h;
	u32 symbol_rate;	/* symbols/s */
	enum fe_delsys delsys;	/* DELSYS_NONE = try S2 then S */
	u32 stream_id;
	int sat;		/* orbital position for the rotor, SAT_UNKNOWN = don't move */
	struct rotor_target rotor;	/* how to reach sat; empty = from the rotor config */
};

struct frontend {
	struct tbs5580 usb;
	struct si2183 demod;
	struct av201x tuner;
	struct fe_params params;
	bool usb_open, demod_init, tuner_init;
	bool lnb_set, lnb_pol_h, lnb_hiband;
	int rotor_sat;		/* where the dish was last sent */
};

typedef void (*blindscan_cb)(void *opaque, u32 freq_khz, bool pol_h, u32 symbol_rate,
			     enum fe_delsys delsys);

int lnb_parse(const char *s, struct lnb *lnb);
/* Is the transponder receivable with this LNB (IF within 950-2150 MHz)? */
bool lnb_in_range(const struct lnb *lnb, u32 freq_khz);

int frontend_open(struct frontend *fe, const struct fe_config *cfg);
void frontend_close(struct frontend *fe);
/* Moves the rotor if needed, sets up the LNB and tunes; 0 once locked within timeout_ms. */
int frontend_tune(struct frontend *fe, const struct fe_config *cfg,
		  const struct tune_req *req, int timeout_ms);
void frontend_read_status(struct frontend *fe, struct fe_status *st);
/* Sends a manual positioner command (see rotor_manual_msg()). */
int frontend_motor(struct frontend *fe, const struct fe_config *cfg, const char *spec);
/*
 * Points the dish to sat with target t (or the rotor config when t is empty);
 * returns the expected travel time in ms, 0 if not moved.
 */
int frontend_rotor_goto(struct frontend *fe, const struct fe_config *cfg, int sat,
			const struct rotor_target *t);
/* Hardware blind scan of one polarization and LNB band; calls cb for each transponder. */
int frontend_blindscan(struct frontend *fe, const struct fe_config *cfg, bool pol_h,
		       bool hiband, u32 sr_min, u32 sr_max, blindscan_cb cb, void *opaque);
const char *delsys_name(int delsys);

#endif
