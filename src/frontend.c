/*
 * TBS 5580 satellite frontend, see frontend.h.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, version 2.
 */
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <limits.h>
#include <libgen.h>
#include <sys/stat.h>
#include <mach-o/dyld.h>
#include "frontend.h"

#define FX2_FIRMWARE	"dvb-usb-id5580.fw"
#define DEMOD_FIRMWARE	"dvb-demod-si2183-b60-01.fw"

int lnb_parse(const char *s, struct lnb *lnb)
{
	char *end;
	double a, b, c;

	if (strcasecmp(s, "universal") == 0) {
		lnb->lof1 = 9750000;
		lnb->lof2 = 10600000;
		lnb->slof = 11700000;
		return 0;
	}
	a = strtod(s, &end);
	if (*end == 0) {
		lnb->lof1 = lnb->lof2 = (u32)(a * 1000);
		lnb->slof = 0;
		return 0;
	}
	if (*end != ',')
		return -1;
	b = strtod(end + 1, &end);
	if (*end != ',')
		return -1;
	c = strtod(end + 1, &end);
	if (*end != 0)
		return -1;
	lnb->lof1 = (u32)(a * 1000);
	lnb->lof2 = (u32)(b * 1000);
	lnb->slof = (u32)(c * 1000);
	return 0;
}

const char *delsys_name(int d)
{
	switch (d) {
	case DELSYS_DVBS: return "DVB-S";
	case DELSYS_DVBS2: return "DVB-S2";
	case DELSYS_DSS: return "DSS";
	default: return "?";
	}
}

static bool file_exists(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* Look for firmware in --fw-dir, ./firmware, <exe>/firmware, <exe>/../share/tbs5580. */
static bool find_firmware(const char *fw_dir, const char *name, char *out, size_t outlen)
{
	char exe[PATH_MAX], real[PATH_MAX];
	char c1[PATH_MAX], c2[PATH_MAX];
	uint32_t size = sizeof(exe);
	const char *candidates[4];
	int n = 0, i;

	if (fw_dir)
		candidates[n++] = fw_dir;
	candidates[n++] = "firmware";
	if (_NSGetExecutablePath(exe, &size) == 0 && realpath(exe, real)) {
		const char *dir = dirname(real);

		snprintf(c1, sizeof(c1), "%s/firmware", dir);
		snprintf(c2, sizeof(c2), "%s/../share/tbs5580", dir);
		candidates[n++] = c1;
		candidates[n++] = c2;
	}
	for (i = 0; i < n; i++) {
		snprintf(out, outlen, "%s/%s", candidates[i], name);
		if (file_exists(out))
			return true;
	}
	LOG("firmware '%s' not found (use --fw-dir)\n", name);
	return false;
}

int frontend_open(struct frontend *fe, const struct fe_config *cfg)
{
	static char fx2_fw[PATH_MAX], demod_fw[PATH_MAX];
	struct si2183_config demod_cfg = {
		.i2c_addr = TBS5580_ADDR_DEMOD,
		.ts_mode = SI2183_TS_PARALLEL,
		.ts_clock_gapped = true,
		.agc_mode = 0x5,
		.fw_path = demod_fw,
	};
	struct av201x_config tuner_cfg = {
		.i2c_address = TBS5580_ADDR_SAT_TUNER,
		.id = ID_AV2018,
		.xtal_freq = 27000,
	};
	int ret;

	memset(fe, 0, sizeof(*fe));
	fe->params.inversion = cfg->inversion;
	fe->params.stream_id = NO_STREAM_ID_FILTER;

	if (!find_firmware(cfg->fw_dir, DEMOD_FIRMWARE, demod_fw, sizeof(demod_fw)))
		return -ENOENT;
	if (!cfg->no_fw && !find_firmware(cfg->fw_dir, FX2_FIRMWARE, fx2_fw, sizeof(fx2_fw)))
		return -ENOENT;

	fe->rotor_sat = rotor_load_state();

	ret = tbs5580_open(&fe->usb, cfg->no_fw ? NULL : fx2_fw);
	fe->usb_open = true;
	if (ret)
		return ret;

	if (cfg->info)
		tbs5580_print_usb_info(&fe->usb);

	/*
	 * dvb-usb reads the MAC address from the EEPROM before attaching the
	 * frontend; the demod firmware download is unreliable without it.
	 */
	{
		u8 eeprom[256];

		if (tbs5580_read_eeprom(&fe->usb, eeprom) == 0) {
			if (g_verbose >= 1 && cfg->info)
				hexdump("EEPROM:", eeprom, 256);
			if (cfg->info)
				LOG("MAC address %02x:%02x:%02x:%02x:%02x:%02x\n", eeprom[16],
				    eeprom[17], eeprom[18], eeprom[19], eeprom[20], eeprom[21]);
		} else {
			LOG("cannot read EEPROM\n");
		}
	}

	si2183_attach(&fe->demod, &fe->usb.i2c, &demod_cfg);
	av201x_attach(&fe->tuner, &fe->demod.tuner_i2c, &tuner_cfg);

	ret = tbs5580_board_init(&fe->usb);
	if (ret) {
		LOG("board init failed\n");
		return ret;
	}

	/* dvb_frontend_init(): demod first, then tuner */
	ret = si2183_init(&fe->demod);
	if (ret)
		return ret;
	fe->demod_init = true;

	ret = av201x_init(&fe->tuner);
	if (ret)
		return ret;
	fe->tuner_init = true;
	LOG("Airoha AV2018 tuner initialized\n");
	return 0;
}

void frontend_close(struct frontend *fe)
{
	tbs5580_stop_stream(&fe->usb);
	if (fe->tuner_init)
		av201x_sleep(&fe->tuner);
	if (fe->demod_init)
		si2183_sleep(&fe->demod);
	if (fe->usb_open)
		tbs5580_close(&fe->usb);
	fe->usb_open = fe->demod_init = fe->tuner_init = false;
}

static u32 lnb_lof(const struct lnb *lnb, u32 freq_khz, bool *hiband)
{
	*hiband = lnb->slof && freq_khz >= lnb->slof;
	return *hiband ? lnb->lof2 : lnb->lof1;
}

static u32 lnb_if(u32 freq_khz, u32 lof)
{
	return freq_khz > lof ? freq_khz - lof : lof - freq_khz;
}

bool lnb_in_range(const struct lnb *lnb, u32 freq_khz)
{
	bool hiband;
	u32 ifreq = lnb_if(freq_khz, lnb_lof(lnb, freq_khz, &hiband));

	return ifreq >= 950000 && ifreq <= 2150000;
}

/* Voltage, committed DiSEqC switch and 22 kHz tone for one polarization/band. */
static int frontend_set_lnb(struct frontend *fe, const struct fe_config *cfg, bool pol_h,
			    bool hiband)
{
	int ret;

	if (fe->lnb_set && fe->lnb_pol_h == pol_h && fe->lnb_hiband == hiband)
		return 0;

	/* same order as dvb-apps/tune-s2: tone off, voltage, diseqc, tone */
	ret = si2183_set_tone(&fe->demod, false);
	ret |= tbs5580_set_voltage(&fe->usb, pol_h);
	msleep(fe->lnb_set ? 20 : 100);	/* let the LNB power up/settle */

	if (cfg->diseqc) {
		u8 msg[4] = { 0xe0, 0x10, 0x38, 0xf0 };

		msg[3] |= ((cfg->diseqc - 1) & 3) << 2;
		msg[3] |= pol_h ? 2 : 0;
		msg[3] |= hiband ? 1 : 0;
		ret |= si2183_diseqc_send_msg(&fe->demod, msg, 4);
		msleep(15);
	}

	ret |= si2183_set_tone(&fe->demod, hiband);
	msleep(15);

	DBG(1, "LNB: %s, %s band%s\n", pol_h ? "18V (H)" : "13V (V)",
	    hiband ? "high" : "low", cfg->diseqc ? ", DiSEqC" : "");

	fe->lnb_set = ret == 0;
	fe->lnb_pol_h = pol_h;
	fe->lnb_hiband = hiband;
	return ret ? -EIO : 0;
}

static int frontend_setup_lnb(struct frontend *fe, const struct fe_config *cfg,
			      const struct tune_req *req)
{
	bool hiband;
	u32 lof = lnb_lof(&cfg->lnb, req->freq_khz, &hiband);

	fe->params.frequency = lnb_if(req->freq_khz, lof);
	if (fe->params.frequency < 950000 || fe->params.frequency > 2150000) {
		LOG("IF frequency %u MHz out of range (950-2150): check frequency and LNB\n",
		    fe->params.frequency / 1000);
		return -EINVAL;
	}
	DBG(1, "LOF %u MHz, IF %u MHz\n", lof / 1000, fe->params.frequency / 1000);
	return frontend_set_lnb(fe, cfg, req->pol_h, hiband);
}

/* ---- rotor --------------------------------------------------------------- */

static int frontend_rotor_send(struct frontend *fe, const struct fe_config *cfg,
			       const u8 *msg, int len)
{
	int ret;

	if (g_verbose >= 1)
		hexdump("DiSEqC positioner:", msg, len);

	/* positioner commands need the tone off; 18V makes most motors faster */
	ret = si2183_set_tone(&fe->demod, false);
	ret |= tbs5580_set_voltage(&fe->usb, true);
	msleep(fe->lnb_set ? 20 : 100);

	if (cfg->diseqc) {
		/* route the command through the committed switch first */
		u8 sw[4] = { 0xe0, 0x10, 0x38, 0xf2 };

		sw[3] |= ((cfg->diseqc - 1) & 3) << 2;
		ret |= si2183_diseqc_send_msg(&fe->demod, sw, 4);
		msleep(15);
	}
	ret |= si2183_diseqc_send_msg(&fe->demod, msg, len);
	msleep(15);

	/* voltage and tone must be set again on the next tune */
	fe->lnb_set = false;
	return ret ? -EIO : 0;
}

int frontend_motor(struct frontend *fe, const struct fe_config *cfg, const char *spec)
{
	u8 msg[6];
	int len = rotor_manual_msg(&cfg->rotor, spec, msg);
	const char *arg = strchr(spec, ':');
	int sat, i;

	if (len < 0) {
		LOG("invalid motor command '%s'%s\n", spec,
		    strncasecmp(spec, "gotox", 5) == 0 ? " (gotox needs --usals)" : "");
		return -EINVAL;
	}
	LOG("motor: %s\n", spec);
	if (frontend_rotor_send(fe, cfg, msg, len))
		return -EIO;

	/* keep track of where the dish is, when we know it */
	fe->rotor_sat = SAT_UNKNOWN;
	if (strncasecmp(spec, "gotox:", 6) == 0 && sat_parse(arg + 1, &sat) == 0) {
		fe->rotor_sat = sat;
	} else if (strncasecmp(spec, "goto:", 5) == 0) {
		for (i = 0; i < cfg->rotor.n_positions; i++)
			if (cfg->rotor.positions[i].index == atoi(arg + 1))
				fe->rotor_sat = cfg->rotor.positions[i].sat;
	}
	if (strncasecmp(spec, "store", 5) != 0 && strncasecmp(spec, "limit", 5) != 0)
		rotor_save_state(fe->rotor_sat);
	return 0;
}

int frontend_rotor_goto(struct frontend *fe, const struct fe_config *cfg, int sat,
			const struct rotor_target *t)
{
	struct rotor_target target;
	struct rotor_config rc = cfg->rotor;
	char name[16], how[48];
	u8 msg[6];
	int len, travel;

	if (sat == SAT_UNKNOWN || sat == fe->rotor_sat)
		return 0;
	if (t && rotor_target_valid(t))
		target = *t;
	else
		rotor_target_for(&cfg->rotor, sat, &target);
	if (!rotor_target_valid(&target))
		return 0;

	sat_format(sat, name, sizeof(name));
	len = rotor_target_msg(&target, sat, msg);
	if (len == 0)
		return 0;

	/* travel time estimate with the method actually used */
	if (target.usals) {
		rc.mode = ROTOR_USALS;
		rc.lat = target.lat;
		rc.lon = target.lon;
	} else {
		rc.mode = ROTOR_DISEQC12;
	}
	travel = rotor_travel_ms(&rc, fe->rotor_sat, sat);
	rotor_target_format(&target, how, sizeof(how));
	LOG("moving dish to %s (%s%s, up to %d s)\n", name, target.usals ? "" : "position ",
	    how, (travel + 999) / 1000);
	if (frontend_rotor_send(fe, cfg, msg, len))
		return -EIO;
	fe->rotor_sat = sat;
	rotor_save_state(sat);
	return travel;
}

void frontend_read_status(struct frontend *fe, struct fe_status *st)
{
	u16 agc;

	if (si2183_read_status(&fe->demod, &fe->params, st))
		return;
	if (si2183_read_sat_agc(&fe->demod, &agc) == 0) {
		st->strength_valid = true;
		st->strength_mdbm = av201x_get_rf_strength(agc);
	}
}

int frontend_tune(struct frontend *fe, const struct fe_config *cfg,
		  const struct tune_req *req, int timeout_ms)
{
	static const int auto_list[] = { DELSYS_DVBS2, DELSYS_DVBS };
	struct fe_status st = { 0 };
	uint64_t deadline, arrival;
	int attempt = 0, travel;

	travel = frontend_rotor_goto(fe, cfg, req->sat, &req->rotor);
	if (travel < 0)
		return -EIO;
	arrival = now_ms() + travel;
	deadline = arrival + timeout_ms;

	if (frontend_setup_lnb(fe, cfg, req))
		return -EINVAL;

	fe->params.symbol_rate = req->symbol_rate;
	fe->params.stream_id = req->stream_id;

	while (!g_stop && now_ms() < deadline) {
		uint64_t window_end;

		fe->params.delsys = req->delsys ? req->delsys : auto_list[attempt % 2];
		attempt++;
		DBG(1, "tuning %s %.3f MHz %c SR %u ks/s\n", delsys_name(fe->params.delsys),
		    req->freq_khz / 1000.0, req->pol_h ? 'H' : 'V', req->symbol_rate / 1000);

		/* si2183_set_frontend(): tuner first, then demod */
		if (av201x_set_params(&fe->tuner, &fe->params) ||
		    si2183_set_frontend(&fe->demod, &fe->params)) {
			LOG("tuning failed\n");
			return -EIO;
		}

		window_end = req->delsys ? deadline : now_ms() + 1500;
		while (!g_stop && now_ms() < window_end && now_ms() < deadline) {
			msleep(200);
			frontend_read_status(fe, &st);
			if (!st.lock)
				continue;
			if (now_ms() < arrival) {
				/* the dish may still be moving through the beam */
				msleep(1500);
				frontend_read_status(fe, &st);
				if (!st.lock)
					continue;
			}
			return 0;
		}
		DBG(1, "no lock (signal=%d carrier=%d)\n", st.signal, st.carrier);
	}
	return -ETIMEDOUT;
}

/* ---- blind scan ---------------------------------------------------------- */

static void frontend_bs_tune(void *opaque, u32 frequency)
{
	struct frontend *fe = opaque;
	struct fe_params p = fe->params;

	/* widest tuner filter (40 MHz): the demod looks for carriers itself */
	p.frequency = frequency;
	p.symbol_rate = 60000000;
	av201x_set_params(&fe->tuner, &p);
}

int frontend_blindscan(struct frontend *fe, const struct fe_config *cfg, bool pol_h,
		       bool hiband, u32 sr_min, u32 sr_max, blindscan_cb cb, void *opaque)
{
	const struct lnb *lnb = &cfg->lnb;
	u32 lof = hiband ? lnb->lof2 : lnb->lof1;
	u32 fmin = 950000, fmax = 2150000;
	bool inverted = lof < 6000000;	/* C band: the LO is above the signal */
	struct si2183_bs_result r;
	int ret;

	if (hiband && !lnb->slof)
		return 0;
	if (lnb->slof) {
		/* only the part of the IF range that belongs to this band */
		if (hiband && lnb->slof > lof && lnb->slof - lof > fmin)
			fmin = lnb->slof - lof;
		if (!hiband && lnb->slof > lof && lnb->slof - lof < fmax)
			fmax = lnb->slof - lof;
	}

	ret = frontend_set_lnb(fe, cfg, pol_h, hiband);
	if (ret)
		return ret;
	ret = si2183_blindscan_start(&fe->demod, fmin, fmax, sr_min, sr_max);
	if (ret)
		return ret;

	while ((ret = si2183_blindscan_next(&fe->demod, frontend_bs_tune, fe, &r,
					    (volatile int *)&g_scan_stop)) > 0) {
		u32 freq = inverted ? lof - r.frequency : lof + r.frequency;

		cb(opaque, freq, pol_h, r.symbol_rate, r.delsys);
	}
	if (ret < 0)
		si2183_blindscan_abort(&fe->demod);
	return ret == -EINTR ? 0 : ret;
}
