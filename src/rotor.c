/*
 * DiSEqC 1.2 positioner and USALS (GotoX) support.
 * The USALS angle calculation comes from tune-s2/diseqc.c (UDL, GPL v2).
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, version 2.
 */
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include "rotor.h"

int sat_parse(const char *s, int *pos)
{
	char *end;
	double v = strtod(s, &end);

	if (end == s)
		return -1;
	if (*end == 'E' || *end == 'e')
		end++;
	else if (*end == 'W' || *end == 'w') {
		v = -v;
		end++;
	}
	if (*end != 0 || v < -180 || v > 180)
		return -1;
	*pos = (int)lround(v * 10);
	return 0;
}

void sat_format(int pos, char *buf, size_t len)
{
	if (pos == SAT_UNKNOWN) {
		snprintf(buf, len, "-");
		return;
	}
	snprintf(buf, len, "%d.%d%c", abs(pos) / 10, abs(pos) % 10, pos < 0 ? 'W' : 'E');
}

static int parse_coord(const char *s, char pos_c, char neg_c, double *v)
{
	char *end;

	*v = strtod(s, &end);
	if (end == s)
		return -1;
	if (*end == pos_c || *end == (pos_c | 0x20))
		end++;
	else if (*end == neg_c || *end == (neg_c | 0x20)) {
		*v = -*v;
		end++;
	}
	return *end == 0 ? 0 : -1;
}

int rotor_parse_site(const char *s, struct rotor_config *rc)
{
	char lat[32], lon[32];
	const char *comma = strchr(s, ',');

	if (!comma || (size_t)(comma - s) >= sizeof(lat) || strlen(comma + 1) >= sizeof(lon))
		return -1;
	memcpy(lat, s, comma - s);
	lat[comma - s] = 0;
	strcpy(lon, comma + 1);
	if (parse_coord(lat, 'N', 'S', &rc->lat) || parse_coord(lon, 'E', 'W', &rc->lon) ||
	    fabs(rc->lat) > 90 || fabs(rc->lon) > 180)
		return -1;
	rc->mode = ROTOR_USALS;
	return 0;
}

int rotor_parse_positions(const char *s, struct rotor_config *rc)
{
	char buf[1024], *tok, *save;

	strlcpy(buf, s, sizeof(buf));
	for (tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
		char *eq = strchr(tok, '=');
		int sat, idx;

		if (!eq)
			return -1;
		*eq = 0;
		idx = atoi(eq + 1);
		if (sat_parse(tok, &sat) || idx < 1 || idx > 255 ||
		    rc->n_positions >= ROTOR_MAX_POSITIONS)
			return -1;
		rc->positions[rc->n_positions].sat = sat;
		rc->positions[rc->n_positions].index = idx;
		rc->n_positions++;
	}
	if (rc->mode == ROTOR_NONE)
		rc->mode = ROTOR_DISEQC12;
	return 0;
}

static double radian(double d)
{
	return d * M_PI / 180;
}

static double degree(double r)
{
	return r * 180 / M_PI;
}

double usals_angle(double lat, double lon, int sat)
{
	const double r_eq = 6378.14;	/* Earth radius */
	const double r_sat = 42164.57;	/* distance from earth centre to satellite */
	double site_lat = radian(lat), site_long = radian(lon), sat_long = radian(sat / 10.0);
	double dish[3] = { r_eq * cos(site_lat), 0, r_eq * sin(site_lat) };
	double satv[3] = { r_sat * cos(site_long - sat_long), r_sat * sin(site_long - sat_long), 0 };
	double pointing[3] = { satv[0] - dish[0], satv[1] - dish[1], satv[2] - dish[2] };

	return degree(atan(pointing[1] / pointing[0]));
}

static int usals_msg(double angle, u8 msg[6])
{
	int sixteenths = (int)(fabs(angle) * 16.0 + 0.5);

	msg[0] = 0xe0;
	msg[1] = 0x31;
	msg[2] = 0x6e;
	msg[3] = (angle > 0.0 ? 0xd0 : 0xe0) | ((sixteenths >> 8) & 0x0f);
	msg[4] = sixteenths & 0xff;
	return 5;
}

static int msg3(u8 msg[6], u8 cmd)
{
	msg[0] = 0xe0;
	msg[1] = 0x31;
	msg[2] = cmd;
	return 3;
}

static int msg4(u8 msg[6], u8 cmd, u8 data)
{
	msg3(msg, cmd);
	msg[3] = data;
	return 4;
}

void rotor_target_for(const struct rotor_config *rc, int sat, struct rotor_target *t)
{
	int i;

	memset(t, 0, sizeof(*t));
	if (rc->mode == ROTOR_USALS) {
		t->usals = true;
		t->lat = rc->lat;
		t->lon = rc->lon;
	} else if (rc->mode == ROTOR_DISEQC12) {
		t->pos = rc->fixed_index;
		for (i = 0; !t->pos && i < rc->n_positions; i++)
			if (rc->positions[i].sat == sat)
				t->pos = rc->positions[i].index;
	}
}

bool rotor_target_valid(const struct rotor_target *t)
{
	return t->usals || t->pos > 0;
}

int rotor_target_msg(const struct rotor_target *t, int sat, u8 msg[6])
{
	if (t->usals && sat != SAT_UNKNOWN)
		return usals_msg(usals_angle(t->lat, t->lon, sat), msg);
	if (t->pos > 0)
		return msg4(msg, 0x6b, (u8)t->pos);
	return 0;
}

void rotor_target_format(const struct rotor_target *t, char *buf, size_t len)
{
	if (t->usals)
		snprintf(buf, len, "usals:%.2f%c,%.2f%c", fabs(t->lat), t->lat < 0 ? 'S' : 'N',
			 fabs(t->lon), t->lon < 0 ? 'W' : 'E');
	else if (t->pos > 0)
		snprintf(buf, len, "%d", t->pos);
	else
		snprintf(buf, len, "-");
}

int rotor_target_parse(const char *s, struct rotor_target *t)
{
	struct rotor_config rc = { 0 };

	memset(t, 0, sizeof(*t));
	if (strcmp(s, "-") == 0 || *s == 0)
		return 0;
	if (strncasecmp(s, "usals:", 6) == 0) {
		if (rotor_parse_site(s + 6, &rc))
			return -1;
		t->usals = true;
		t->lat = rc.lat;
		t->lon = rc.lon;
		return 0;
	}
	{
		char *end;
		long v = strtol(s, &end, 10);

		if (*end != 0 || v < 0 || v > 255)
			return -1;
		t->pos = (int)v;
	}
	return 0;
}

int rotor_goto_msg(const struct rotor_config *rc, int sat, u8 msg[6])
{
	int i;

	if (sat == SAT_UNKNOWN)
		return 0;
	switch (rc->mode) {
	case ROTOR_USALS:
		return usals_msg(usals_angle(rc->lat, rc->lon, sat), msg);
	case ROTOR_DISEQC12:
		if (rc->fixed_index)
			return msg4(msg, 0x6b, rc->fixed_index);
		for (i = 0; i < rc->n_positions; i++)
			if (rc->positions[i].sat == sat)
				return msg4(msg, 0x6b, rc->positions[i].index);
		return 0;
	default:
		return 0;
	}
}

int rotor_manual_msg(const struct rotor_config *rc, const char *spec, u8 msg[6])
{
	const char *arg = strchr(spec, ':');
	size_t n = arg ? (size_t)(arg - spec) : strlen(spec);
	int v = arg ? atoi(arg + 1) : 0;

#define IS(name) (n == strlen(name) && strncasecmp(spec, name, n) == 0)
	if (IS("halt") || IS("stop"))
		return msg3(msg, 0x60);
	if (IS("limits-off"))
		return msg3(msg, 0x63);
	if (IS("limit-east"))
		return msg3(msg, 0x66);
	if (IS("limit-west"))
		return msg3(msg, 0x67);
	if (IS("east") || IS("west")) {
		/* 0x80-0xff = number of steps, two's complement */
		if (!arg)
			v = 1;
		if (v < 1 || v > 127)
			return -1;
		return msg4(msg, IS("east") ? 0x68 : 0x69, (u8)(0x100 - v));
	}
	if (IS("drive-east") || IS("drive-west")) {
		/* 0x01-0x7f = timeout in seconds */
		if (v < 1 || v > 127)
			return -1;
		return msg4(msg, IS("drive-east") ? 0x68 : 0x69, (u8)v);
	}
	if (IS("store") || IS("goto")) {
		if (v < 1 || v > 255)
			return -1;
		return msg4(msg, IS("store") ? 0x6a : 0x6b, (u8)v);
	}
	if (IS("reference"))
		return msg4(msg, 0x6b, 0);
	if (IS("gotox")) {
		int sat;

		if (!arg || sat_parse(arg + 1, &sat) || rc->mode != ROTOR_USALS)
			return -1;
		return usals_msg(usals_angle(rc->lat, rc->lon, sat), msg);
	}
#undef IS
	return -1;
}

int rotor_travel_ms(const struct rotor_config *rc, int from, int to)
{
	double speed = rc->speed > 0 ? rc->speed : 1.5;
	double deg;

	if (from == SAT_UNKNOWN || to == SAT_UNKNOWN)
		return 45000;
	if (rc->mode == ROTOR_USALS)
		deg = fabs(usals_angle(rc->lat, rc->lon, to) - usals_angle(rc->lat, rc->lon, from));
	else
		deg = abs(to - from) / 10.0;
	return (int)(deg / speed * 1000) + 1000;
}

/* Kept in the working directory, next to channels.conf. */
#define ROTOR_STATE_FILE "tbs5580_rotor.state"

int rotor_load_state(void)
{
	char line[64];
	FILE *f;
	int sat;

	f = fopen(ROTOR_STATE_FILE, "r");
	if (!f)
		return SAT_UNKNOWN;
	if (!fgets(line, sizeof(line), f) || sscanf(line, "%d", &sat) != 1)
		sat = SAT_UNKNOWN;
	fclose(f);
	return sat;
}

void rotor_save_state(int sat)
{
	FILE *f;

	f = fopen(ROTOR_STATE_FILE, "w");
	if (!f)
		return;
	fprintf(f, "%d\n", sat);
	fclose(f);
}
