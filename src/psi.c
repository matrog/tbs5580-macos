/*
 * MPEG-TS PSI/DVB SI parser, see psi.h.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, version 2.
 */
#include <string.h>
#include <iconv.h>
#include "psi.h"

#define PID_PAT 0x0000
#define PID_NIT 0x0010
#define PID_SDT 0x0011

#define TID_PAT		0x00
#define TID_PMT		0x02
#define TID_NIT_ACT	0x40
#define TID_NIT_OTH	0x41
#define TID_SDT_ACT	0x42

static u32 crc32_mpeg(const u8 *data, int len)
{
	static u32 table[256];
	static bool init;
	u32 crc = 0xffffffff;
	int i, j;

	if (!init) {
		for (i = 0; i < 256; i++) {
			u32 c = (u32)i << 24;

			for (j = 0; j < 8; j++)
				c = (c & 0x80000000) ? (c << 1) ^ 0x04c11db7 : c << 1;
			table[i] = c;
		}
		init = true;
	}
	for (i = 0; i < len; i++)
		crc = (crc << 8) ^ table[((crc >> 24) ^ data[i]) & 0xff];
	return crc;
}

/* ---- text ---------------------------------------------------------------- */

static int put_utf8(char *out, size_t outlen, size_t o, u32 c)
{
	if (c < 0x80) {
		if (o + 1 >= outlen)
			return 0;
		out[o] = c;
		return 1;
	} else if (c < 0x800) {
		if (o + 2 >= outlen)
			return 0;
		out[o] = 0xc0 | (c >> 6);
		out[o + 1] = 0x80 | (c & 0x3f);
		return 2;
	}
	if (o + 3 >= outlen)
		return 0;
	out[o] = 0xe0 | (c >> 12);
	out[o + 1] = 0x80 | ((c >> 6) & 0x3f);
	out[o + 2] = 0x80 | (c & 0x3f);
	return 3;
}

/* ISO/IEC 6937 (the DVB default table): diacritics are prefixes 0xC1-0xCF. */
static void iso6937_to_utf8(const u8 *s, int len, char *out, size_t outlen)
{
	static const u16 combining[16] = {
		0, 0x0300, 0x0301, 0x0302, 0x0303, 0x0304, 0x0306, 0x0307,
		0x0308, 0, 0x030a, 0x0327, 0, 0x030b, 0x0328, 0x030c,
	};
	static const struct { u8 in; u16 out; } specials[] = {
		{ 0xa3, 0x00a3 }, { 0xa5, 0x00a5 }, { 0xa7, 0x00a7 }, { 0xa8, 0x00a4 },
		{ 0xb0, 0x00b0 }, { 0xb1, 0x00b1 }, { 0xb2, 0x00b2 }, { 0xb3, 0x00b3 },
		{ 0xd0, 0x2015 }, { 0xd1, 0x00b9 }, { 0xd2, 0x00ae }, { 0xd3, 0x00a9 },
		{ 0xd4, 0x2122 }, { 0xe1, 0x00c6 }, { 0xe9, 0x00d8 }, { 0xf1, 0x00e6 },
		{ 0xf9, 0x00f8 }, { 0xfb, 0x00df },
	};
	size_t o = 0;
	int i, k, n;

	for (i = 0; i < len; i++) {
		u8 c = s[i];
		u32 u = 0;

		if (c >= 0x80 && c <= 0x9f) {
			if (c == 0x8a)	/* CR/LF */
				u = ' ';
			else
				continue;
		} else if (c >= 0xc1 && c <= 0xcf && i + 1 < len && combining[c - 0xc0]) {
			n = put_utf8(out, outlen, o, s[++i]);
			o += n;
			u = combining[c - 0xc0];
		} else if (c < 0x80) {
			u = c;
		} else {
			for (k = 0; k < (int)ARRAY_SIZE(specials); k++)
				if (specials[k].in == c)
					u = specials[k].out;
			if (!u)
				continue;
		}
		n = put_utf8(out, outlen, o, u);
		if (!n)
			break;
		o += n;
	}
	out[o] = 0;
}

static void iconv_to_utf8(const char *charset, const u8 *s, int len, char *out, size_t outlen)
{
	iconv_t cd = iconv_open("UTF-8", charset);
	char *in = (char *)s, *op = out;
	size_t inleft = len, outleft = outlen - 1;

	if (cd == (iconv_t)-1) {
		iso6937_to_utf8(s, len, out, outlen);
		return;
	}
	while (inleft > 0) {
		if (iconv(cd, &in, &inleft, &op, &outleft) == (size_t)-1) {
			if (errno == E2BIG)
				break;
			in++;	/* skip invalid byte */
			inleft--;
		}
	}
	iconv_close(cd);
	*op = 0;
}

/* Decode a DVB string (EN 300 468 annex A) into UTF-8. */
static void dvb_text(const u8 *s, int len, char *out, size_t outlen)
{
	char charset[16];
	char tmp[512];
	int i, o;

	out[0] = 0;
	if (len <= 0)
		return;

	if (s[0] >= 0x20) {
		iso6937_to_utf8(s, len, tmp, sizeof(tmp));
	} else if (s[0] >= 0x01 && s[0] <= 0x0b) {
		snprintf(charset, sizeof(charset), "ISO-8859-%d", s[0] + 4);
		iconv_to_utf8(charset, s + 1, len - 1, tmp, sizeof(tmp));
	} else if (s[0] == 0x10 && len >= 3) {
		snprintf(charset, sizeof(charset), "ISO-8859-%d", s[2]);
		iconv_to_utf8(charset, s + 3, len - 3, tmp, sizeof(tmp));
	} else if (s[0] == 0x11) {
		iconv_to_utf8("UCS-2BE", s + 1, len - 1, tmp, sizeof(tmp));
	} else if (s[0] == 0x15) {
		iconv_to_utf8("UTF-8", s + 1, len - 1, tmp, sizeof(tmp));
	} else if (s[0] == 0x1f && len >= 2) {
		iso6937_to_utf8(s + 2, len - 2, tmp, sizeof(tmp));
	} else {
		iso6937_to_utf8(s + 1, len - 1, tmp, sizeof(tmp));
	}

	/*
	 * Drop control characters, including the DVB emphasis codes and other
	 * C1 controls (U+0080..U+009F), which some strings carry through the
	 * single-byte charsets as the UTF-8 pair 0xC2 0x80..0x9F.
	 */
	for (i = 0, o = 0; tmp[i] && o < (int)outlen - 1;) {
		u8 b = tmp[i];
		int clen = b < 0x80 ? 1 : b < 0xe0 ? 2 : b < 0xf0 ? 3 : 4;
		bool ctrl = b < 0x20 || b == 0x7f ||
			    (b == 0xc2 && (u8)tmp[i + 1] >= 0x80 && (u8)tmp[i + 1] <= 0x9f);
		int k;

		if (ctrl) {
			i += clen;
			continue;
		}
		for (k = 0; k < clen && tmp[i] && o < (int)outlen - 1; k++)
			out[o++] = tmp[i++];
	}
	while (o > 0 && out[o - 1] == ' ')
		o--;
	out[o] = 0;
}

static u32 bcd(const u8 *b, int digits)
{
	u32 v = 0;
	int i;

	for (i = 0; i < digits; i++) {
		u8 nib = (i & 1) ? (b[i / 2] & 0x0f) : (b[i / 2] >> 4);

		v = v * 10 + nib;
	}
	return v;
}

/* ---- tables -------------------------------------------------------------- */

static void psi_want(struct psi *p, u16 pid)
{
	p->want[pid & 0x1fff] = 1;
}

struct psi_service *psi_find_service(struct psi *p, u16 sid)
{
	int i;

	for (i = 0; i < p->n_services; i++)
		if (p->services[i].sid == sid)
			return &p->services[i];
	return NULL;
}

static struct psi_service *psi_get_service(struct psi *p, u16 sid)
{
	struct psi_service *s = psi_find_service(p, sid);

	if (s || p->n_services >= PSI_MAX_SERVICES)
		return s;
	s = &p->services[p->n_services++];
	memset(s, 0, sizeof(*s));
	s->sid = sid;
	s->pcr_pid = 0x1fff;
	return s;
}

/* Returns true if the section is new for this table version. */
static bool table_section(struct psi_table_state *t, int version, int section, int last)
{
	if (t->version != version) {
		t->version = version;
		memset(t->seen, 0, sizeof(t->seen));
	}
	t->last_section = last;
	if (t->seen[section / 8] & (1 << (section % 8)))
		return false;
	t->seen[section / 8] |= 1 << (section % 8);
	return true;
}

static bool table_complete(const struct psi_table_state *t)
{
	int i;

	if (t->version < 0)
		return false;
	for (i = 0; i <= t->last_section; i++)
		if (!(t->seen[i / 8] & (1 << (i % 8))))
			return false;
	return true;
}

static void parse_pat(struct psi *p, const u8 *sec, int len)
{
	int i;

	p->tsid = sec[3] << 8 | sec[4];
	if (!table_section(&p->pat, (sec[5] >> 1) & 0x1f, sec[6], sec[7]))
		return;

	for (i = 8; i + 4 <= len - 4; i += 4) {
		u16 prog = sec[i] << 8 | sec[i + 1];
		u16 pid = (sec[i + 2] & 0x1f) << 8 | sec[i + 3];
		struct psi_service *s;

		if (prog == 0)
			continue;	/* NIT PID */
		s = psi_get_service(p, prog);
		if (!s)
			continue;
		s->in_pat = true;
		s->pmt_pid = pid;
		psi_want(p, pid);
	}
	p->have_pat = table_complete(&p->pat);
}

static u8 es_kind(u8 stream_type, const u8 *desc, int dlen)
{
	int i;

	switch (stream_type) {
	case 0x01: case 0x02: case 0x10: case 0x1b: case 0x24: case 0x42: case 0xea:
		return PSI_ES_VIDEO;
	case 0x03: case 0x04: case 0x0f: case 0x11: case 0x81: case 0x87:
		return PSI_ES_AUDIO;
	case 0x06:
		for (i = 0; i + 2 <= dlen; i += 2 + desc[i + 1]) {
			switch (desc[i]) {
			case 0x6a: case 0x7a: case 0x7b: case 0x7c:
				return PSI_ES_AUDIO;	/* AC-3, E-AC-3, DTS, AAC */
			case 0x59:
				return PSI_ES_SUBTITLE;
			case 0x56:
				return PSI_ES_TELETEXT;
			case 0x7f:
				if (i + 2 < dlen && desc[i + 2] == 0x15)
					return PSI_ES_AUDIO;	/* AC-4 */
				break;
			}
		}
		break;
	}
	return PSI_ES_OTHER;
}

static void parse_pmt(struct psi *p, const u8 *sec, int len)
{
	u16 sid = sec[3] << 8 | sec[4];
	u8 version = (sec[5] >> 1) & 0x1f;
	struct psi_service *s = psi_find_service(p, sid);
	int pi_len, i, j;

	if (!s || (s->have_pmt && s->pmt_version == version))
		return;

	s->have_pmt = true;
	s->pmt_version = version;
	s->generation++;
	s->pcr_pid = (sec[8] & 0x1f) << 8 | sec[9];
	s->n_es = 0;

	s->caid = 0;
	pi_len = (sec[10] & 0x0f) << 8 | sec[11];
	for (j = 12; j + 2 <= 12 + pi_len && j + 2 <= len - 4; j += 2 + sec[j + 1])
		if (sec[j] == 0x09) {
			s->scrambled = true;
			if (sec[j + 1] >= 2 && !s->caid)
				s->caid = sec[j + 2] << 8 | sec[j + 3];
		}

	for (i = 12 + pi_len; i + 5 <= len - 4;) {
		u8 type = sec[i];
		u16 pid = (sec[i + 1] & 0x1f) << 8 | sec[i + 2];
		int es_len = (sec[i + 3] & 0x0f) << 8 | sec[i + 4];
		const u8 *d = &sec[i + 5];

		if (i + 5 + es_len > len - 4)
			break;
		if (s->n_es < PSI_MAX_ES) {
			struct psi_es *es = &s->es[s->n_es++];

			memset(es, 0, sizeof(*es));
			es->pid = pid;
			es->stream_type = type;
			es->kind = es_kind(type, d, es_len);
			for (j = 0; j + 2 <= es_len; j += 2 + d[j + 1]) {
				if (d[j] == 0x09) {
					s->scrambled = true;
					if (d[j + 1] >= 2 && !s->caid)
						s->caid = d[j + 2] << 8 | d[j + 3];
				}
				if (d[j] == 0x0a && d[j + 1] >= 3)
					memcpy(es->lang, &d[j + 2], 3);
			}
		}
		i += 5 + es_len;
	}
}

static void parse_sdt(struct psi *p, const u8 *sec, int len)
{
	int i, j;

	/* other transport streams can sneak in during a retune */
	if (p->have_pat && (sec[3] << 8 | sec[4]) != p->tsid)
		return;
	if (!table_section(&p->sdt, (sec[5] >> 1) & 0x1f, sec[6], sec[7]))
		return;
	p->onid = sec[8] << 8 | sec[9];

	for (i = 11; i + 5 <= len - 4;) {
		u16 sid = sec[i] << 8 | sec[i + 1];
		bool free_ca = sec[i + 3] & 0x10;
		int dlen = (sec[i + 3] & 0x0f) << 8 | sec[i + 4];
		const u8 *d = &sec[i + 5];
		struct psi_service *s;

		if (i + 5 + dlen > len - 4)
			break;
		s = psi_get_service(p, sid);
		if (s) {
			s->have_sdt = true;
			if (free_ca)
				s->scrambled = true;
			for (j = 0; j + 2 <= dlen; j += 2 + d[j + 1]) {
				int dl = d[j + 1], pl, nl;

				if (d[j] != 0x48 || dl < 3 || j + 2 + dl > dlen)
					continue;
				s->type = d[j + 2];
				pl = d[j + 3];
				if (3 + pl > dl)
					continue;
				dvb_text(&d[j + 4], pl, s->provider, sizeof(s->provider));
				nl = d[j + 4 + pl];
				if (3 + pl + nl > dl)
					continue;
				dvb_text(&d[j + 5 + pl], nl, s->name, sizeof(s->name));
			}
		}
		i += 5 + dlen;
	}
}

static void add_transponder(struct psi *p, const struct psi_transponder *t)
{
	int i;

	for (i = 0; i < p->n_transponders; i++) {
		struct psi_transponder *o = &p->transponders[i];
		int diff = (int)o->freq_khz - (int)t->freq_khz;

		if (o->pol_h == t->pol_h && diff > -2000 && diff < 2000 &&
		    o->orbital == t->orbital && o->east == t->east)
			return;
	}
	if (p->n_transponders < PSI_MAX_TRANSPONDERS)
		p->transponders[p->n_transponders++] = *t;
}

static void parse_nit(struct psi *p, const u8 *sec, int len)
{
	bool actual = sec[0] == TID_NIT_ACT;
	int i, j, nd_len, ts_len;

	if (actual && !table_section(&p->nit, (sec[5] >> 1) & 0x1f, sec[6], sec[7]))
		return;

	nd_len = (sec[8] & 0x0f) << 8 | sec[9];
	for (j = 10; j + 2 <= 10 + nd_len && j + 2 <= len - 4; j += 2 + sec[j + 1])
		if (actual && sec[j] == 0x40)
			dvb_text(&sec[j + 2], sec[j + 1], p->network_name, sizeof(p->network_name));

	i = 10 + nd_len;
	if (i + 2 > len - 4)
		return;
	ts_len = (sec[i] & 0x0f) << 8 | sec[i + 1];
	i += 2;
	while (i + 6 <= len - 4 && ts_len >= 6) {
		u16 tsid = sec[i] << 8 | sec[i + 1];
		u16 onid = sec[i + 2] << 8 | sec[i + 3];
		int dlen = (sec[i + 4] & 0x0f) << 8 | sec[i + 5];
		const u8 *d = &sec[i + 6];

		if (i + 6 + dlen > len - 4)
			break;
		for (j = 0; j + 2 <= dlen; j += 2 + d[j + 1]) {
			struct psi_transponder t;
			u8 pol;

			if (d[j] != 0x43 || d[j + 1] < 11)
				continue;	/* satellite_delivery_system_descriptor */
			memset(&t, 0, sizeof(t));
			t.freq_khz = bcd(&d[j + 2], 8) * 10;
			t.orbital = bcd(&d[j + 6], 4);
			t.east = d[j + 8] & 0x80;
			pol = (d[j + 8] >> 5) & 3;
			t.pol_h = pol == 0 || pol == 2;
			t.delsys = (d[j + 8] & 0x04) ? DELSYS_DVBS2 : DELSYS_DVBS;
			t.symbol_rate = bcd(&d[j + 9], 7) * 100;
			t.tsid = tsid;
			t.onid = onid;
			if (t.freq_khz && t.symbol_rate)
				add_transponder(p, &t);
		}
		i += 6 + dlen;
		ts_len -= 6 + dlen;
	}
}

static void psi_section(struct psi *p, u16 pid, const u8 *sec, int len)
{
	if (len < 12 || !(sec[1] & 0x80))
		return;		/* need a long section with syntax indicator */
	if (!(sec[5] & 0x01))
		return;		/* not current */
	if (crc32_mpeg(sec, len) != 0) {
		DBG(2, "psi: CRC error on PID 0x%04x table 0x%02x\n", pid, sec[0]);
		return;
	}

	switch (sec[0]) {
	case TID_PAT:
		if (pid == PID_PAT)
			parse_pat(p, sec, len);
		break;
	case TID_PMT:
		parse_pmt(p, sec, len);
		break;
	case TID_SDT_ACT:
		if (pid == PID_SDT)
			parse_sdt(p, sec, len);
		break;
	case TID_NIT_ACT:
	case TID_NIT_OTH:
		if (pid == PID_NIT)
			parse_nit(p, sec, len);
		break;
	}
}

/* ---- section reassembly -------------------------------------------------- */

static struct psi_asm *psi_get_asm(struct psi *p, u16 pid)
{
	int i;

	for (i = 0; i < p->n_asm; i++)
		if (p->asm_[i].pid == pid)
			return &p->asm_[i];
	if (p->n_asm >= PSI_MAX_ASM)
		return NULL;
	p->asm_[p->n_asm].pid = pid;
	p->asm_[p->n_asm].active = false;
	p->asm_[p->n_asm].cc = -1;
	p->asm_[p->n_asm].len = 0;
	return &p->asm_[p->n_asm++];
}

static void asm_push(struct psi *p, struct psi_asm *a, const u8 *d, int n)
{
	while (n > 0 && a->active) {
		int need, take;

		if (a->len == 0 && d[0] == 0xff) {
			a->active = false;	/* stuffing */
			return;
		}
		if (a->len < 3) {
			take = 3 - a->len < n ? 3 - a->len : n;
			memcpy(a->buf + a->len, d, take);
			a->len += take;
			d += take;
			n -= take;
			continue;
		}
		need = 3 + ((a->buf[1] & 0x0f) << 8 | a->buf[2]);
		if (need > 4096) {
			a->active = false;
			a->len = 0;
			return;
		}
		take = need - a->len < n ? need - a->len : n;
		memcpy(a->buf + a->len, d, take);
		a->len += take;
		d += take;
		n -= take;
		if (a->len == need) {
			psi_section(p, a->pid, a->buf, a->len);
			a->len = 0;
		}
	}
}

void psi_packet(struct psi *p, const u8 *pkt)
{
	u16 pid = (pkt[1] & 0x1f) << 8 | pkt[2];
	bool pusi = pkt[1] & 0x40;
	int afc = (pkt[3] >> 4) & 3;
	int cc = pkt[3] & 0x0f;
	const u8 *payload;
	int plen;
	struct psi_asm *a;

	if (!p->want[pid] || (pkt[1] & 0x80) || !(afc & 1))
		return;

	payload = pkt + 4;
	if (afc == 3)
		payload += 1 + pkt[4];
	plen = pkt + 188 - payload;
	if (plen <= 0)
		return;

	pthread_mutex_lock(&p->lock);
	a = psi_get_asm(p, pid);
	if (!a)
		goto out;

	if (a->cc >= 0 && cc == a->cc)
		goto out;	/* duplicate */
	if (a->cc >= 0 && cc != ((a->cc + 1) & 0x0f)) {
		a->active = false;
		a->len = 0;
	}
	a->cc = cc;

	if (pusi) {
		int ptr = payload[0];

		if (1 + ptr > plen) {
			a->active = false;
			goto out;
		}
		if (a->active && a->len > 0)
			asm_push(p, a, payload + 1, ptr);
		a->active = true;
		a->len = 0;
		asm_push(p, a, payload + 1 + ptr, plen - 1 - ptr);
	} else if (a->active) {
		asm_push(p, a, payload, plen);
	}
out:
	pthread_mutex_unlock(&p->lock);
}

/* ---- public helpers ------------------------------------------------------ */

static void psi_clear(struct psi *p)
{
	memset(p->want, 0, sizeof(p->want));
	p->n_asm = 0;
	p->tsid = p->onid = 0;
	p->pat.version = p->sdt.version = p->nit.version = -1;
	p->have_pat = false;
	p->network_name[0] = 0;
	p->n_services = 0;
	p->n_transponders = 0;
	psi_want(p, PID_PAT);
	psi_want(p, PID_NIT);
	psi_want(p, PID_SDT);
}

void psi_init(struct psi *p)
{
	pthread_mutex_init(&p->lock, NULL);
	psi_clear(p);
}

void psi_reset(struct psi *p)
{
	pthread_mutex_lock(&p->lock);
	psi_clear(p);
	pthread_mutex_unlock(&p->lock);
}

bool psi_complete(struct psi *p, bool want_nit)
{
	return p->have_pat && table_complete(&p->sdt) &&
	       (!want_nit || table_complete(&p->nit));
}

bool psi_type_is_tv(u8 type)
{
	switch (type) {
	case 0x01: case 0x11: case 0x16: case 0x19: case 0x1c: case 0x1f: case 0x20:
		return true;
	}
	return false;
}

bool psi_type_is_radio(u8 type)
{
	return type == 0x02 || type == 0x07 || type == 0x0a;
}

const char *psi_type_name(u8 type)
{
	switch (type) {
	case 0x01: return "TV";
	case 0x16: return "TV";
	case 0x11: case 0x19: return "HD";
	case 0x1c: case 0x1f: case 0x20: return "UHD";
	case 0x02: case 0x07: case 0x0a: return "Radio";
	case 0x0c: return "Data";
	default: return "Other";
	}
}

void psi_make_pat(u8 pkt[188], u16 tsid, u16 sid, u16 pmt_pid, u8 version, u8 cc)
{
	u8 *s = pkt + 5;
	u32 crc;

	memset(pkt, 0xff, 188);
	pkt[0] = 0x47;
	pkt[1] = 0x40;		/* PUSI, PID 0 */
	pkt[2] = 0x00;
	pkt[3] = 0x10 | (cc & 0x0f);
	pkt[4] = 0x00;		/* pointer field */

	s[0] = TID_PAT;
	s[1] = 0xb0;
	s[2] = 13;		/* 5 header + 4 program + 4 CRC */
	s[3] = tsid >> 8;
	s[4] = tsid & 0xff;
	s[5] = 0xc1 | ((version & 0x1f) << 1);
	s[6] = 0;
	s[7] = 0;
	s[8] = sid >> 8;
	s[9] = sid & 0xff;
	s[10] = 0xe0 | (pmt_pid >> 8);
	s[11] = pmt_pid & 0xff;
	crc = crc32_mpeg(s, 12);
	s[12] = crc >> 24;
	s[13] = crc >> 16;
	s[14] = crc >> 8;
	s[15] = crc;
}
