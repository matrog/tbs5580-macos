/*
 * Channel list produced by --scan, stored as a tab-separated text file.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, version 2.
 */
#ifndef CHANNELS_H
#define CHANNELS_H

#include "common.h"
#include "rotor.h"

struct channel {
	char name[128];
	char provider[128];
	u32 freq_khz;
	u32 symbol_rate;
	bool pol_h;
	enum fe_delsys delsys;
	u16 sid;
	u16 onid;
	u16 tsid;
	u8 type;
	bool scrambled;
	int sat;		/* orbital position, tenths of degree east, SAT_UNKNOWN */
	struct rotor_target rotor;	/* how to point the dish at sat */
};

struct channel_list {
	struct channel *ch;
	int n;
	int cap;
};

/* Adds or replaces (same transponder and service id). */
int channel_list_add(struct channel_list *l, const struct channel *c);
void channel_list_sort(struct channel_list *l);
/*
 * Adds src to dst, first dropping from dst the channels of satellite sat (or
 * of an unknown satellite): all of them with whole_sat, otherwise only those
 * on the transponders present in src.
 */
int channel_list_merge(struct channel_list *dst, const struct channel_list *src, int sat,
		       bool whole_sat);
void channel_list_free(struct channel_list *l);

/* Sets the rotor target of every channel of satellite sat; returns how many. */
int channel_list_set_rotor(struct channel_list *l, int sat, const struct rotor_target *t);

int channels_save(const char *path, const struct channel_list *l);
int channels_load(const char *path, struct channel_list *l);

#endif
