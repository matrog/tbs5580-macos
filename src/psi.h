/*
 * MPEG-TS PSI/DVB SI parser: PAT, PMT, SDT and NIT.
 * Collects the services of the current transponder and the transponders
 * announced in the NIT.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, version 2.
 */
#ifndef PSI_H
#define PSI_H

#include <pthread.h>
#include "common.h"

#define PSI_MAX_ES		32
#define PSI_MAX_SERVICES	512
#define PSI_MAX_TRANSPONDERS	512
#define PSI_MAX_ASM		64

struct psi_es {
	u16 pid;
	u8 stream_type;
	u8 kind;		/* PSI_ES_* */
	char lang[4];
};

enum {
	PSI_ES_OTHER,
	PSI_ES_VIDEO,
	PSI_ES_AUDIO,
	PSI_ES_SUBTITLE,
	PSI_ES_TELETEXT,
};

struct psi_service {
	u16 sid;
	u16 pmt_pid;
	u16 pcr_pid;
	u8 type;		/* SDT service_type, 0 if unknown */
	bool in_pat;
	bool have_pmt;
	bool have_sdt;
	bool scrambled;		/* SDT free_CA_mode or CA descriptor in the PMT */
	u8 pmt_version;
	u16 caid;		/* CA_system_id of the first CA descriptor, 0 if clear */
	u32 generation;		/* bumped whenever the PMT changes */
	char name[128];
	char provider[128];
	int n_es;
	struct psi_es es[PSI_MAX_ES];
};

struct psi_transponder {
	u32 freq_khz;
	u32 symbol_rate;	/* symbols/s */
	bool pol_h;		/* H or circular L */
	enum fe_delsys delsys;
	u16 orbital;		/* 0.1 degree, BCD decoded */
	bool east;
	u16 onid, tsid;
};

/* Section completeness tracking (one table/version). */
struct psi_table_state {
	int version;		/* -1 = nothing yet */
	int last_section;
	u8 seen[32];		/* bitmap of section numbers */
};

struct psi_asm {
	u16 pid;
	bool active;
	int cc;
	int len;
	u8 buf[4096 + 188];
};

struct psi {
	pthread_mutex_t lock;
	u8 want[8192];		/* PIDs carrying sections we parse */
	struct psi_asm asm_[PSI_MAX_ASM];
	int n_asm;

	u16 tsid;
	u16 onid;
	struct psi_table_state pat, sdt, nit;
	bool have_pat;
	char network_name[128];

	struct psi_service services[PSI_MAX_SERVICES];
	int n_services;

	struct psi_transponder transponders[PSI_MAX_TRANSPONDERS];
	int n_transponders;
};

void psi_init(struct psi *p);
void psi_reset(struct psi *p);
/* Feed one aligned 188-byte TS packet. */
void psi_packet(struct psi *p, const u8 *pkt);

/* With p->lock held. */
bool psi_complete(struct psi *p, bool want_nit);
struct psi_service *psi_find_service(struct psi *p, u16 sid);

/* Service type helpers (EN 300 468 table 87). */
bool psi_type_is_tv(u8 type);
bool psi_type_is_radio(u8 type);
const char *psi_type_name(u8 type);

/* Builds a 188-byte PAT packet announcing a single program. */
void psi_make_pat(u8 pkt[188], u16 tsid, u16 sid, u16 pmt_pid, u8 version, u8 cc);

#endif
