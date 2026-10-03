/*
 * DiSEqC 1.2 positioner and USALS (GotoX) support.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, version 2.
 */
#ifndef ROTOR_H
#define ROTOR_H

#include <limits.h>
#include "common.h"

/* Orbital positions are kept in tenths of a degree, east positive: 192 = 19.2E. */
#define SAT_UNKNOWN INT_MIN

int sat_parse(const char *s, int *pos);
void sat_format(int pos, char *buf, size_t len);

enum rotor_mode {
	ROTOR_NONE,
	ROTOR_USALS,
	ROTOR_DISEQC12,
};

#define ROTOR_MAX_POSITIONS 64

struct rotor_config {
	enum rotor_mode mode;
	double lat, lon;	/* site, degrees, north/east positive (USALS) */
	double speed;		/* degrees per second, to estimate the travel time */
	int fixed_index;	/* --rotor N: stored position to use, 0 = from the map */
	int n_positions;	/* --positions 13E=1,19.2E=2 (DiSEqC 1.2) */
	struct {
		int sat;
		int index;
	} positions[ROTOR_MAX_POSITIONS];
};

int rotor_parse_site(const char *s, struct rotor_config *rc);
int rotor_parse_positions(const char *s, struct rotor_config *rc);

/* Motor angle for USALS, degrees (positive = west of south). */
double usals_angle(double lat, double lon, int sat);

/*
 * Builds the DiSEqC message moving the dish to satellite sat.
 * Returns the message length, 0 if the rotor is not configured for sat.
 */
int rotor_goto_msg(const struct rotor_config *rc, int sat, u8 msg[6]);
/*
 * How to point the dish at one satellite, stored with each channel:
 * a DiSEqC 1.2 stored position, or USALS with the site coordinates.
 */
struct rotor_target {
	int pos;		/* DiSEqC 1.2 stored position, 0 = none */
	bool usals;
	double lat, lon;	/* site, for USALS */
};

/* The target for sat according to --rotor/--positions/--usals. */
void rotor_target_for(const struct rotor_config *rc, int sat, struct rotor_target *t);
bool rotor_target_valid(const struct rotor_target *t);
/* Message moving the dish to sat; 0 if the target is empty. */
int rotor_target_msg(const struct rotor_target *t, int sat, u8 msg[6]);
/* "-", "2" or "usals:45.46N,9.19E" (channels.conf pos column). */
void rotor_target_format(const struct rotor_target *t, char *buf, size_t len);
int rotor_target_parse(const char *s, struct rotor_target *t);

/*
 * Manual command: halt, east[:N], west[:N], drive-east:SEC, drive-west:SEC,
 * limit-east, limit-west, limits-off, store:N, goto:N, reference, gotox:POS.
 * Returns the message length or -1.
 */
int rotor_manual_msg(const struct rotor_config *rc, const char *spec, u8 msg[6]);

/* Estimated travel time between two satellites, ms (unknown start: worst case). */
int rotor_travel_ms(const struct rotor_config *rc, int from, int to);

/* Last position the dish was sent to, persisted in ./tbs5580_rotor.state. */
int rotor_load_state(void);
void rotor_save_state(int sat);

#endif
