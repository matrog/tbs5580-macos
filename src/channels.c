/*
 * Channel list, see channels.h.
 *
 * File format, one service per line, tab separated:
 *   name provider freq_mhz pol sr_ksym delsys sid type ca onid tsid sat pos
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, version 2.
 */
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "channels.h"
#include "rotor.h"

static bool same_transponder(const struct channel *a, const struct channel *b)
{
	int diff = (int)a->freq_khz - (int)b->freq_khz;

	return a->pol_h == b->pol_h && diff > -2000 && diff < 2000 && a->sat == b->sat;
}

int channel_list_add(struct channel_list *l, const struct channel *c)
{
	int i;

	for (i = 0; i < l->n; i++) {
		if (l->ch[i].sid == c->sid && same_transponder(&l->ch[i], c)) {
			l->ch[i] = *c;
			return 0;
		}
	}
	if (l->n == l->cap) {
		int cap = l->cap ? l->cap * 2 : 256;
		struct channel *ch = realloc(l->ch, cap * sizeof(*ch));

		if (!ch)
			return -ENOMEM;
		l->ch = ch;
		l->cap = cap;
	}
	l->ch[l->n++] = *c;
	return 0;
}

static int cmp_channel(const void *a, const void *b)
{
	const struct channel *x = a, *y = b;
	int r = strcasecmp(x->name, y->name);

	if (r)
		return r;
	return (int)x->freq_khz - (int)y->freq_khz;
}

void channel_list_sort(struct channel_list *l)
{
	qsort(l->ch, l->n, sizeof(*l->ch), cmp_channel);
}

int channel_list_merge(struct channel_list *dst, const struct channel_list *src, int sat,
		       bool whole_sat)
{
	int i, j, n = 0;

	for (i = 0; i < dst->n; i++) {
		struct channel *c = &dst->ch[i];
		/* entries without a satellite come from older single-satellite lists */
		bool drop = c->sat == sat || c->sat == SAT_UNKNOWN;

		if (drop && !whole_sat) {
			struct channel tmp = *c;

			drop = false;
			tmp.sat = sat;
			for (j = 0; j < src->n && !drop; j++)
				drop = same_transponder(&tmp, &src->ch[j]);
		}
		if (!drop)
			dst->ch[n++] = *c;
	}
	dst->n = n;
	for (i = 0; i < src->n; i++)
		if (channel_list_add(dst, &src->ch[i]))
			return -ENOMEM;
	return 0;
}

int channel_list_set_rotor(struct channel_list *l, int sat, const struct rotor_target *t)
{
	int i, n = 0;

	for (i = 0; i < l->n; i++) {
		if (l->ch[i].sat == sat) {
			l->ch[i].rotor = *t;
			n++;
		}
	}
	return n;
}

void channel_list_free(struct channel_list *l)
{
	free(l->ch);
	memset(l, 0, sizeof(*l));
}

static void sanitize(char *s)
{
	for (; *s; s++)
		if (*s == '\t' || *s == '\n' || *s == '\r')
			*s = ' ';
}

int channels_save(const char *path, const struct channel_list *l)
{
	FILE *f = fopen(path, "w");
	int i;

	if (!f) {
		LOG("cannot write '%s'\n", path);
		return -errno;
	}
	fprintf(f, "# tbs5580 channel list\n");
	fprintf(f, "# name\tprovider\tfreq_mhz\tpol\tsr_ksym\tdelsys\tsid\ttype\tca\tonid\ttsid\tsat\tpos\n");
	for (i = 0; i < l->n; i++) {
		struct channel c = l->ch[i];
		char sat[16], pos[48];

		sanitize(c.name);
		sanitize(c.provider);
		sat_format(c.sat, sat, sizeof(sat));
		rotor_target_format(&c.rotor, pos, sizeof(pos));
		fprintf(f, "%s\t%s\t%.3f\t%c\t%u\t%s\t%u\t0x%02x\t%d\t%u\t%u\t%s\t%s\n",
			c.name, c.provider, c.freq_khz / 1000.0, c.pol_h ? 'H' : 'V',
			c.symbol_rate / 1000, c.delsys == DELSYS_DVBS2 ? "S2" : "S",
			c.sid, c.type, c.scrambled ? 1 : 0, c.onid, c.tsid, sat, pos);
	}
	fclose(f);
	return 0;
}

int channels_load(const char *path, struct channel_list *l)
{
	FILE *f = fopen(path, "r");
	char line[1024];

	if (!f)
		return -ENOENT;
	while (fgets(line, sizeof(line), f)) {
		char *field[13];
		char *p = line;
		struct channel c;
		int n = 0;

		if (line[0] == '#' || line[0] == '\n')
			continue;
		line[strcspn(line, "\r\n")] = 0;
		while (n < 13 && p) {
			field[n++] = p;
			p = strchr(p, '\t');
			if (p)
				*p++ = 0;
		}
		if (n < 11)
			continue;

		memset(&c, 0, sizeof(c));
		strlcpy(c.name, field[0], sizeof(c.name));
		strlcpy(c.provider, field[1], sizeof(c.provider));
		c.freq_khz = (u32)(strtod(field[2], NULL) * 1000 + 0.5);
		c.pol_h = field[3][0] == 'H' || field[3][0] == 'h';
		c.symbol_rate = atoi(field[4]) * 1000;
		c.delsys = strcasecmp(field[5], "S2") == 0 ? DELSYS_DVBS2 : DELSYS_DVBS;
		c.sid = atoi(field[6]);
		c.type = strtol(field[7], NULL, 0);
		c.scrambled = atoi(field[8]);
		c.onid = atoi(field[9]);
		c.tsid = atoi(field[10]);
		/* 11-column files predate the sat column */
		if (n < 12 || sat_parse(field[11], &c.sat))
			c.sat = SAT_UNKNOWN;
		if (n >= 13)
			rotor_target_parse(field[12], &c.rotor);
		channel_list_add(l, &c);
	}
	fclose(f);
	return 0;
}
