/*
 * tbs5580: user-space driver for the TurboSight TBS 5580 USB tuner on macOS.
 * Tunes DVB-S/S2 transponders, scans channel lists and streams services.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, version 2.
 */
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdarg.h>
#include <ncurses.h>

#include <getopt.h>
#include <spawn.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/wait.h>
#include "frontend.h"
#include "tsout.h"
#include "psi.h"
#include "channels.h"

extern char **environ;

int g_verbose;
volatile sig_atomic_t g_stop;
volatile sig_atomic_t g_scan_stop;	/* abort the current scan, not the program */

/* Scan progress sink: set by the TUI to draw on screen, else logs to stderr. */
static void (*g_scan_progress)(const char *line);

static void scan_report(const char *fmt, ...)
{
	char buf[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (g_scan_progress)
		g_scan_progress(buf);
	else
		LOG("%s\n", buf);
}

static bool scan_stopped(void)
{
	return g_stop || g_scan_stop;
}

void msleep(unsigned int ms)
{
	usleep(ms * 1000);
}

uint64_t now_ms(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

void hexdump(const char *prefix, const u8 *buf, int len)
{
	int i;

	fprintf(stderr, "%s", prefix);
	for (i = 0; i < len; i++)
		fprintf(stderr, " %02x", buf[i]);
	fprintf(stderr, "\n");
}

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
	g_scan_stop = 1;
}

enum mode {
	MODE_TUNE,
	MODE_INFO,
	MODE_SCAN,
	MODE_INTERACTIVE,
	MODE_BLINDSCAN,
	MODE_MOTOR,
	MODE_SET_POSITION,
};

#define MAX_MOTOR_CMDS 16

struct options {
	enum mode mode;
	struct tune_req req;
	bool have_pol;
	int sid;
	const char *output;
	const char *udp;
	int http_port;
	int duration;
	int lock_timeout;
	const char *channels;
	bool scan_single;
	const char *player;
	bool no_player;
	int n_xfers;
	int xfer_size;
	u32 sr_min, sr_max;
	bool no_services;
	const char *set_position;
	const char *motor[MAX_MOTOR_CMDS];
	int n_motor;
};

struct app {
	struct options opt;
	struct fe_config cfg;
	struct frontend fe;
	struct psi psi;
	struct tsout *out;
};

static void usage(const char *prog)
{
	fprintf(stderr,
"Usage: %s [options]\n"
"\n"
"Modes:\n"
"  (default)               tune one transponder, monitor and/or output the TS\n"
"      --scan              scan channels starting from -f/-p/-s, following the NIT\n"
"      --blindscan         hardware blind scan of the satellite, then scan channels\n"
"  -i, --interactive       pick channels from the scanned list and play them in VLC\n"
"      --info              initialize the hardware, print device info and exit\n"
"\n"
"Tuning (DVB-S/S2):\n"
"  -f, --freq MHZ          transponder frequency, e.g. 11766\n"
"  -p, --pol H|V           polarization (H = 18V, V = 13V)\n"
"  -s, --sr KSYM           symbol rate in ksym/s, e.g. 27500\n"
"  -d, --delsys S|S2|auto  delivery system (default auto: tries S2 then S)\n"
"  -l, --lnb TYPE          universal (default), a single LOF in MHz (e.g. 10750,\n"
"                          5150 for C band) or LOF1,LOF2,SLOF in MHz\n"
"  -D, --diseqc N          DiSEqC 1.0 committed port 1-4\n"
"      --isi N             DVB-S2 multistream input stream id\n"
"      --pls-gold N        DVB-S2 PLS gold code (with --isi)\n"
"      --pls-root N        DVB-S2 PLS root code (with --isi)\n"
"      --inversion on|off|auto  spectral inversion (default auto, as on Linux)\n"
"\n"
"Motorized dish (DiSEqC 1.2 / USALS):\n"
"      --sat POS           satellite to point at, e.g. 13E, 19.2E, 30W\n"
"      --usals LAT,LON     site coordinates for USALS, e.g. 45.46N,9.19E\n"
"      --rotor N           DiSEqC 1.2: go to stored position N\n"
"      --positions LIST    DiSEqC 1.2 positions, e.g. 13E=1,19.2E=2\n"
"      --rotor-speed DEG   degrees per second, to estimate travel time (default 1.5)\n"
"      --set-position SAT=N|usals:LAT,LON  how to reach SAT, stored on its channels\n"
"                          in the channel list (no hardware needed)\n"
"      --motor CMD         halt, east[:N], west[:N], drive-east:SEC, drive-west:SEC,\n"
"                          goto:N, store:N, reference, gotox:POS, limit-east,\n"
"                          limit-west, limits-off (repeatable; alone = just move)\n"
"\n"
"Blind scan:\n"
"      --sr-range MIN-MAX  symbol rates to look for, ksym/s (default 1000-45000)\n"
"      --no-services       only list the transponders, do not scan channels\n"
"                          (-p restricts the scan to one polarization)\n"
"\n"
"Output (without any output only the signal is monitored):\n"
"  -o, --output FILE       write the TS to FILE, '-' for stdout\n"
"  -u, --udp HOST:PORT     send the TS over UDP (7 packets per datagram)\n"
"  -H, --http PORT         serve over HTTP: /  whole TS, /<sid>  one channel,\n"
"                          /playlist.m3u  channels of the transponder\n"
"  -S, --sid N             only output service N to -o/-u\n"
"  -t, --time SEC          stop after SEC seconds\n"
"\n"
"Channel list:\n"
"  -c, --channels FILE     channel list file (default channels.conf)\n"
"      --scan-single       scan only the given transponder, ignore the NIT\n"
"      --player CMD        player command for -i, the URL is appended\n"
"                          (default: open -a VLC)\n"
"      --no-player         -i only prints the URL\n"
"\n"
"Other:\n"
"      --fw-dir DIR        firmware directory\n"
"      --no-fw             do not upload the FX2 firmware\n"
"      --lock-timeout SEC  give up if no lock within SEC seconds (default 10)\n"
"      --usb-buffers N,SIZE  number and size of USB bulk transfers (default 16,32768)\n"
"  -v, --verbose           more debug output (repeat up to 3 times)\n"
"  -h, --help              this help\n"
"\n"
"Default options can be put in ./tbs5580.rc (e.g. --positions 13E=1,7E=2).\n"
"\n"
"Examples:\n"
"  %s --scan -f 11766 -p H -s 27500        # scan the satellite, writes channels.conf\n"
"  %s --blindscan --sat 13E                # blind scan, adds 13E to channels.conf\n"
"  %s -i                                   # choose a channel, plays in VLC\n"
"  %s -f 11766 -p H -s 27500 -H 8001       # then http://localhost:8001/playlist.m3u\n"
"  %s -f 10788 -p V -s 22000 -S 5004 -o - | vlc -\n",
	prog, prog, prog, prog, prog, prog);
}

/* ---- streaming helpers --------------------------------------------------- */

static void on_ts_data(const u8 *buf, int len, void *opaque)
{
	tsout_push(opaque, buf, len);
}

static int start_stream(struct app *a)
{
	return tbs5580_start_stream(&a->fe.usb, a->opt.n_xfers, a->opt.xfer_size,
				    on_ts_data, a->out);
}

/* Tune and restart streaming so that no data from the old transponder survives. */
static int retune(struct app *a, const struct tune_req *req, int timeout_ms)
{
	int ret;

	tbs5580_stop_stream(&a->fe.usb);
	ret = frontend_tune(&a->fe, &a->cfg, req, timeout_ms);
	if (ret)
		return ret;
	tsout_reset(a->out);
	return start_stream(a);
}

static void print_lock(struct app *a)
{
	struct fe_status st;

	frontend_read_status(&a->fe, &st);
	if (!st.lock)
		return;
	LOG("LOCKED: %s %s %s", delsys_name(a->fe.params.delsys), st.modulation, st.fec);
	if (st.rolloff)
		LOG(" rolloff %s pilot %s", st.rolloff, st.pilot);
	LOG(", C/N %.1f dB", st.cnr_mdb / 1000.0);
	if (st.strength_valid)
		LOG(", RF %.1f dBm", st.strength_mdbm / 1000.0);
	LOG("\n");
}

static void format_status(struct app *a, char *line, size_t size, uint64_t *last_bytes,
			  uint64_t *last_t)
{
	struct fe_status st;
	struct tsout_stats ts;
	uint64_t t = now_ms();
	int n = 0;

	frontend_read_status(&a->fe, &st);
	n += snprintf(line + n, size - n, "%s",
		      st.lock ? "LOCK" : st.carrier ? "CARRIER" : st.signal ? "SIGNAL" : "NO SIGNAL");
	if (st.cnr_valid)
		n += snprintf(line + n, size - n, " | C/N %4.1f dB", st.cnr_mdb / 1000.0);
	if (st.strength_valid)
		n += snprintf(line + n, size - n, " | RF %5.1f dBm", st.strength_mdbm / 1000.0);
	if (a->out) {
		tsout_get_stats(a->out, &ts);
		n += snprintf(line + n, size - n, " | TS %6.2f Mbit/s",
			      (ts.bytes_in - *last_bytes) * 8.0 / 1000.0 / (t - *last_t + 1));
		if (ts.sync_losses || ts.tei_packets || ts.bytes_dropped)
			n += snprintf(line + n, size - n, " | sync loss %llu TEI %llu drop %llu",
				      (unsigned long long)ts.sync_losses,
				      (unsigned long long)ts.tei_packets,
				      (unsigned long long)ts.bytes_dropped);
		if (ts.http_clients)
			n += snprintf(line + n, size - n, " | %d client%s",
				      ts.http_clients, ts.http_clients > 1 ? "s" : "");
		*last_bytes = ts.bytes_in;
	}
	*last_t = t;
}

/* Wait until PAT and SDT (and optionally NIT) are complete. */
static bool wait_psi(struct app *a, bool want_nit, int timeout_ms)
{
	uint64_t deadline = now_ms() + timeout_ms;
	bool done = false;

	while (!g_stop && !done && now_ms() < deadline) {
		msleep(100);
		pthread_mutex_lock(&a->psi.lock);
		done = psi_complete(&a->psi, want_nit);
		pthread_mutex_unlock(&a->psi.lock);
	}
	return done;
}

/* Service type, falling back to the PMT content when the SDT has none. */
static u8 service_type(const struct psi_service *s)
{
	bool video = false, audio = false;
	int i;

	if (s->type)
		return s->type;
	for (i = 0; i < s->n_es; i++) {
		video |= s->es[i].kind == PSI_ES_VIDEO;
		audio |= s->es[i].kind == PSI_ES_AUDIO;
	}
	return video ? 0x01 : audio ? 0x02 : 0;
}

static void print_services(struct app *a)
{
	int i;

	pthread_mutex_lock(&a->psi.lock);
	LOG("services on this transponder (tsid %u):\n", a->psi.tsid);
	for (i = 0; i < a->psi.n_services; i++) {
		struct psi_service *s = &a->psi.services[i];

		if (!s->in_pat)
			continue;
		LOG("  %5u  %-5s %-32s %s%s\n", s->sid, psi_type_name(service_type(s)),
		    s->name[0] ? s->name : "?", s->provider, s->scrambled ? "  [scrambled]" : "");
	}
	pthread_mutex_unlock(&a->psi.lock);
}

/* ---- mode: tune ---------------------------------------------------------- */

static int mode_tune(struct app *a)
{
	struct options *opt = &a->opt;
	uint64_t t0, last_t, last_bytes = 0;
	bool services_printed = false;

	if (opt->output && tsout_add_file(a->out, opt->output))
		return 1;
	if (opt->udp && tsout_add_udp(a->out, opt->udp))
		return 1;
	if (opt->http_port && tsout_add_http(a->out, opt->http_port))
		return 1;
	tsout_set_sid(a->out, opt->sid);
	if (tsout_start(a->out))
		return 1;

	LOG("tuning %.3f MHz %c SR %u ks/s\n", opt->req.freq_khz / 1000.0,
	    opt->req.pol_h ? 'H' : 'V', opt->req.symbol_rate / 1000);
	if (retune(a, &opt->req, opt->lock_timeout * 1000)) {
		if (!g_stop)
			LOG("no lock within %d s\n", opt->lock_timeout);
		return 1;
	}
	print_lock(a);
	if (opt->http_port)
		LOG("serving TS on http://localhost:%d/ (channels: /playlist.m3u)\n",
		    opt->http_port);
	if (opt->udp)
		LOG("sending TS to udp://%s\n", opt->udp);

	t0 = last_t = now_ms();
	while (!g_stop) {
		uint64_t next = now_ms() + 1000;
		char line[256];

		while (!g_stop && now_ms() < next)
			msleep(50);
		if (g_stop)
			break;

		if (!services_printed) {
			pthread_mutex_lock(&a->psi.lock);
			services_printed = psi_complete(&a->psi, false);
			pthread_mutex_unlock(&a->psi.lock);
			if (services_printed)
				print_services(a);
		}

		format_status(a, line, sizeof(line), &last_bytes, &last_t);
		LOG("[%5llus] %s\n", (unsigned long long)((now_ms() - t0) / 1000), line);

		if (opt->duration && now_ms() - t0 >= (uint64_t)opt->duration * 1000)
			break;
		if (opt->output && !opt->udp && !opt->http_port) {
			struct tsout_stats ts;

			tsout_get_stats(a->out, &ts);
			if (ts.file_error) {
				LOG("output closed\n");
				break;
			}
		}
	}
	return 0;
}

/* ---- mode: scan ---------------------------------------------------------- */

static bool queued(const struct psi_transponder *q, int n, const struct psi_transponder *t)
{
	int i;

	for (i = 0; i < n; i++) {
		int diff = (int)q[i].freq_khz - (int)t->freq_khz;

		if (q[i].pol_h == t->pol_h && diff > -2000 && diff < 2000)
			return true;
	}
	return false;
}

/*
 * Tunes each transponder of the queue and collects its services into list.
 * With follow_nit the transponders announced in the NIT of the first one are
 * queued too. Returns the satellite position (from --sat or the NIT).
 */
static int scan_queue(struct app *a, struct psi_transponder *queue, int nq, bool follow_nit,
		      struct channel_list *list)
{
	int sat = a->opt.req.sat;
	int i, j;

	for (i = 0; i < nq && !scan_stopped(); i++) {
		struct psi_transponder *tp = &queue[i];
		struct tune_req req = {
			.freq_khz = tp->freq_khz,
			.pol_h = tp->pol_h,
			.symbol_rate = tp->symbol_rate,
			.delsys = tp->delsys,
			.stream_id = NO_STREAM_ID_FILTER,
			.sat = a->opt.req.sat,
			.rotor = a->opt.req.rotor,
		};
		/* the NIT is also needed to learn the satellite position */
		bool want_nit = (follow_nit && i == 0) || sat == SAT_UNKNOWN;
		int base = list->n;
		int count;

		scan_report("[%3d/%3d] %8.3f %c %5u %-6s  tuning...", i + 1, nq,
			    tp->freq_khz / 1000.0, tp->pol_h ? 'H' : 'V', tp->symbol_rate / 1000,
			    tp->delsys ? delsys_name(tp->delsys) : "auto");

		if (retune(a, &req, i == 0 ? a->opt.lock_timeout * 1000 : 3000)) {
			scan_report("[%3d/%3d] %8.3f %c %5u  no lock", i + 1, nq,
				    tp->freq_khz / 1000.0, tp->pol_h ? 'H' : 'V',
				    tp->symbol_rate / 1000);
			continue;
		}
		tp->delsys = a->fe.params.delsys;

		if (!wait_psi(a, want_nit, want_nit ? 15000 : 8000))
			DBG(1, "(incomplete tables) ");

		pthread_mutex_lock(&a->psi.lock);

		/*
		 * Satellite position from the NIT entry describing this very
		 * transponder: same frequency and polarization. Networks list
		 * transport streams of other satellites too, even with the same ids.
		 */
		if (sat == SAT_UNKNOWN) {
			for (j = 0; j < a->psi.n_transponders; j++) {
				struct psi_transponder *t = &a->psi.transponders[j];
				int diff = (int)t->freq_khz - (int)tp->freq_khz;

				if (t->pol_h == tp->pol_h && diff > -3000 && diff < 3000 &&
				    t->tsid == a->psi.tsid) {
					sat = t->east ? t->orbital : -(int)t->orbital;
					break;
				}
			}
		}

		for (j = 0; j < a->psi.n_services; j++) {
			struct psi_service *s = &a->psi.services[j];
			struct channel c;

			if (!s->in_pat)
				continue;
			memset(&c, 0, sizeof(c));
			if (s->name[0])
				strlcpy(c.name, s->name, sizeof(c.name));
			else
				snprintf(c.name, sizeof(c.name), "Service %u", s->sid);
			strlcpy(c.provider, s->provider, sizeof(c.provider));
			c.freq_khz = tp->freq_khz;
			c.symbol_rate = tp->symbol_rate;
			c.pol_h = tp->pol_h;
			c.delsys = tp->delsys;
			c.sid = s->sid;
			c.onid = a->psi.onid;
			c.tsid = a->psi.tsid;
			c.type = service_type(s);
			c.scrambled = s->scrambled;
			c.sat = sat;
			channel_list_add(list, &c);
		}

		if (follow_nit) {
			int added = 0;

			for (j = 0; j < a->psi.n_transponders && nq < PSI_MAX_TRANSPONDERS; j++) {
				struct psi_transponder *t = &a->psi.transponders[j];
				int tsat = t->east ? t->orbital : -(int)t->orbital;

				if (sat != SAT_UNKNOWN && tsat != sat)
					continue;
				if (!lnb_in_range(&a->cfg.lnb, t->freq_khz) || queued(queue, nq, t))
					continue;
				queue[nq++] = *t;
				added++;
			}
			if (i == 0 && sat != SAT_UNKNOWN) {
				char name[16];

				sat_format(sat, name, sizeof(name));
				scan_report("satellite %s, network '%s', %d transponders in NIT",
					    name, a->psi.network_name, added + 1);
			}
		}
		pthread_mutex_unlock(&a->psi.lock);

		count = list->n - base;
		scan_report("[%3d/%3d] %8.3f %c %5u %-6s  %d services", i + 1, nq,
			    tp->freq_khz / 1000.0, tp->pol_h ? 'H' : 'V', tp->symbol_rate / 1000,
			    tp->delsys ? delsys_name(tp->delsys) : "auto", count);

		/* list every channel found, wrapped to a readable width */
		{
			char line[160] = "";

			for (j = base; j < list->n; j++) {
				const char *nm = list->ch[j].name;

				if (line[0] && strlen(line) + strlen(nm) + 2 > 92) {
					scan_report("%s", line);
					line[0] = 0;
				}
				if (line[0])
					strlcat(line, ", ", sizeof(line));
				else
					strlcpy(line, "    ", sizeof(line));
				strlcat(line, nm, sizeof(line));
			}
			if (line[0])
				scan_report("%s", line);
		}
	}

	tbs5580_stop_stream(&a->fe.usb);
	return sat;
}

/* Merges the scan result into the channel file, replacing the same satellite. */
static int save_scan(struct app *a, struct channel_list *list, int sat, bool whole_sat)
{
	struct channel_list all = { 0 };
	char name[16];
	int i, tv = 0, radio = 0, fta = 0, ret = 1;

	for (i = 0; i < list->n; i++) {
		list->ch[i].sat = sat;
		/* remember how the dish was pointed: DiSEqC 1.2 position or USALS */
		rotor_target_for(&a->cfg.rotor, sat, &list->ch[i].rotor);
		tv += psi_type_is_tv(list->ch[i].type);
		radio += psi_type_is_radio(list->ch[i].type);
		fta += !list->ch[i].scrambled && (psi_type_is_tv(list->ch[i].type) ||
						  psi_type_is_radio(list->ch[i].type));
	}
	if (list->n == 0) {
		scan_report("no channels found");
		return 1;
	}

	channels_load(a->opt.channels, &all);
	if (channel_list_merge(&all, list, sat, whole_sat) == 0) {
		channel_list_sort(&all);
		ret = channels_save(a->opt.channels, &all) ? 1 : 0;
	}
	sat_format(sat, name, sizeof(name));
	if (ret == 0)
		scan_report("%d services on %s (%d TV, %d radio, %d FTA) saved, %d channels total%s",
			    list->n, name, tv, radio, fta, all.n,
			    scan_stopped() ? " - interrupted" : "");
	channel_list_free(&all);
	return ret;
}

static int mode_scan(struct app *a)
{
	static struct psi_transponder queue[PSI_MAX_TRANSPONDERS];
	struct channel_list list = { 0 };
	struct options *opt = &a->opt;
	int sat, ret;

	if (tsout_start(a->out))
		return 1;

	queue[0].freq_khz = opt->req.freq_khz;
	queue[0].pol_h = opt->req.pol_h;
	queue[0].symbol_rate = opt->req.symbol_rate;
	queue[0].delsys = opt->req.delsys;

	sat = scan_queue(a, queue, 1, !opt->scan_single, &list);
	/* merge per-transponder: a scan updates what it saw, never wipes the rest */
	ret = save_scan(a, &list, sat, false);
	channel_list_free(&list);
	return ret;
}

/* ---- mode: blind scan ---------------------------------------------------- */

struct blind_state {
	struct psi_transponder *queue;
	int nq;
};

static void on_blind_found(void *opaque, u32 freq_khz, bool pol_h, u32 symbol_rate,
			   enum fe_delsys delsys)
{
	struct blind_state *b = opaque;
	struct psi_transponder t = {
		.freq_khz = freq_khz,
		.pol_h = pol_h,
		.symbol_rate = symbol_rate,
		.delsys = delsys,
	};
	bool dup = queued(b->queue, b->nq, &t);

	scan_report("  %9.3f %c %6u %-6s%s", freq_khz / 1000.0, pol_h ? 'H' : 'V',
		    symbol_rate / 1000, delsys_name(delsys), dup ? "  (duplicate)" : "");
	if (!dup && b->nq < PSI_MAX_TRANSPONDERS)
		b->queue[b->nq++] = t;
}

static int cmp_tp(const void *x, const void *y)
{
	const struct psi_transponder *a = x, *b = y;

	if (a->pol_h != b->pol_h)
		return a->pol_h - b->pol_h;
	return (int)a->freq_khz - (int)b->freq_khz;
}

static int mode_blindscan(struct app *a)
{
	static struct psi_transponder queue[PSI_MAX_TRANSPONDERS];
	struct blind_state b = { queue, 0 };
	struct options *opt = &a->opt;
	struct channel_list list = { 0 };
	bool pols[2] = { false, true };
	int npol = 2, nband = a->cfg.lnb.slof ? 2 : 1;
	uint64_t t0 = now_ms();
	int p, band, travel, sat, ret;

	if (opt->have_pol) {
		pols[0] = opt->req.pol_h;
		npol = 1;
	}

	travel = frontend_rotor_goto(&a->fe, &a->cfg, opt->req.sat, &opt->req.rotor);
	if (travel < 0)
		return 1;
	if (travel > 0) {
		uint64_t until = now_ms() + travel;

		scan_report("moving the dish...");
		while (!scan_stopped() && now_ms() < until)
			msleep(100);
	}

	for (p = 0; p < npol && !scan_stopped(); p++) {
		for (band = 0; band < nband && !scan_stopped(); band++) {
			int before = b.nq;

			scan_report("blind scan: %s, %s band", pols[p] ? "horizontal" : "vertical",
				    nband == 1 ? "full" : band ? "high" : "low");
			ret = frontend_blindscan(&a->fe, &a->cfg, pols[p], band == 1, opt->sr_min,
						 opt->sr_max, on_blind_found, &b);
			if (ret)
				scan_report("blind scan failed (%d)", ret);
			scan_report("  -> %d new transponders", b.nq - before);
		}
	}
	scan_report("%d transponders found in %llu s", b.nq,
		    (unsigned long long)((now_ms() - t0) / 1000));
	if (opt->no_services || scan_stopped() || b.nq == 0)
		return b.nq ? 0 : 1;

	qsort(queue, b.nq, sizeof(queue[0]), cmp_tp);
	if (tsout_start(a->out))
		return 1;
	sat = scan_queue(a, queue, b.nq, false, &list);
	ret = save_scan(a, &list, sat, false);
	channel_list_free(&list);
	return ret;
}

/* ---- mode: interactive (ncurses TUI) ------------------------------------- */

static bool channel_visible(const struct channel *c, bool show_all)
{
	return show_all || (!c->scrambled && (psi_type_is_tv(c->type) || psi_type_is_radio(c->type)));
}

static void launch_player(struct app *a, const char *url)
{
	char cmd[1024];
	pid_t pid;
	int status;

	if (a->opt.no_player)
		return;
	if (a->opt.player) {
		snprintf(cmd, sizeof(cmd), "%s '%s' >/dev/null 2>&1 &", a->opt.player, url);
		if (system(cmd) != 0)
			return;
		return;
	}
	char *argv[] = { "open", "-a", "VLC", (char *)url, NULL };

	if (posix_spawn(&pid, "/usr/bin/open", NULL, NULL, argv, environ) == 0)
		waitpid(pid, &status, 0);
}

static bool same_tp(const struct channel *a, const struct channel *b)
{
	int diff = (int)a->freq_khz - (int)b->freq_khz;

	return a->pol_h == b->pol_h && diff > -2000 && diff < 2000 &&
	       a->symbol_rate == b->symbol_rate && a->sat == b->sat;
}

/* Do the audio/video PIDs of the service carry data? Watches them for 2 s. */
static bool service_on_air(struct app *a, u16 sid)
{
	u16 pids[PSI_MAX_ES];
	u32 before[PSI_MAX_ES];
	struct psi_service *s;
	int n = 0, i;

	pthread_mutex_lock(&a->psi.lock);
	s = psi_find_service(&a->psi, sid);
	for (i = 0; s && i < s->n_es; i++)
		if (s->es[i].kind == PSI_ES_VIDEO || s->es[i].kind == PSI_ES_AUDIO)
			pids[n++] = s->es[i].pid;
	pthread_mutex_unlock(&a->psi.lock);
	if (n == 0)
		return true;	/* nothing to judge on */

	for (i = 0; i < n; i++)
		before[i] = tsout_pid_packets(a->out, pids[i]);
	msleep(2000);
	for (i = 0; i < n; i++)
		if (tsout_pid_packets(a->out, pids[i]) != before[i])
			return true;
	return false;
}

/* ---- the TUI itself ------------------------------------------------------ */

enum { CP_HEADER = 1, CP_SEL, CP_SCR, CP_LOCK, CP_DIM, CP_PLAYING };

struct tui {
	struct app *a;
	struct channel_list *list;
	int *filt;		/* indices into list, after search/visibility */
	int filt_cap;
	int nfilt;
	int sel;		/* selected row in filt */
	int top;		/* first visible row in filt */
	char search[64];
	bool show_all;
	int sat_filter;		/* only this satellite when sat_filter_on */
	bool sat_filter_on;
	int sort_mode;		/* 0 name, 1 frequency, 2 satellite */
	struct channel cur;	/* currently tuned channel */
	bool cur_valid;
	int cur_idx;		/* list index of the playing channel, -1 */
	char msg[160];
	bool msg_err;
	uint64_t last_bytes, last_t;
};

static const struct channel_list *g_sort_list;
static int g_sort_mode;

static int tui_filt_cmp(const void *a, const void *b)
{
	const struct channel *x = &g_sort_list->ch[*(const int *)a];
	const struct channel *y = &g_sort_list->ch[*(const int *)b];
	int r;

	if (g_sort_mode == 1 && x->freq_khz != y->freq_khz)
		return (int)x->freq_khz - (int)y->freq_khz;
	if (g_sort_mode == 2 && x->sat != y->sat)
		return x->sat - y->sat;
	r = strcasecmp(x->name, y->name);
	if (r)
		return r;
	return (int)x->freq_khz - (int)y->freq_khz;
}

static void tui_filter(struct tui *t)
{
	int i, sel_list = t->sel < t->nfilt ? t->filt[t->sel] : -1;

	t->nfilt = 0;
	for (i = 0; i < t->list->n; i++) {
		struct channel *c = &t->list->ch[i];

		if (t->sat_filter_on && c->sat != t->sat_filter)
			continue;
		if (!channel_visible(c, t->show_all))
			continue;
		if (t->search[0] && !strcasestr(c->name, t->search) &&
		    !strcasestr(c->provider, t->search))
			continue;
		t->filt[t->nfilt++] = i;
	}

	g_sort_list = t->list;
	g_sort_mode = t->sort_mode;
	qsort(t->filt, t->nfilt, sizeof(int), tui_filt_cmp);

	/* keep the selection on the same channel when possible */
	t->sel = 0;
	for (i = 0; i < t->nfilt; i++)
		if (t->filt[i] == sel_list)
			t->sel = i;
	if (t->sel >= t->nfilt)
		t->sel = t->nfilt ? t->nfilt - 1 : 0;
}

static void tui_draw(struct tui *t)
{
	int rows = LINES, cols = COLS;
	int listh = rows - 4;
	int i, y;
	char buf[512];

	if (listh < 1)
		listh = 1;
	if (t->sel < t->top)
		t->top = t->sel;
	if (t->sel >= t->top + listh)
		t->top = t->sel - listh + 1;

	erase();

	/* header */
	if (has_colors())
		attron(COLOR_PAIR(CP_HEADER));
	else
		attron(A_REVERSE);
	{
		char satf[24] = "";

		if (t->sat_filter_on) {
			char sn[16];

			sat_format(t->sat_filter, sn, sizeof(sn));
			snprintf(satf, sizeof(satf), "  [%s]", sn);
		}
		static const char *sortname[] = { "name", "freq", "sat" };

		snprintf(buf, sizeof(buf), " tbs5580  -  %d/%d channels%s%s  sort:%s ",
			 t->nfilt, t->list->n, t->show_all ? "  [all]" : "  [free]", satf,
			 sortname[t->sort_mode]);
	}
	mvprintw(0, 0, "%-*s", cols, buf);
	if (has_colors())
		attroff(COLOR_PAIR(CP_HEADER));
	else
		attroff(A_REVERSE);

	/* search line */
	mvprintw(1, 0, "Search: %s", t->search);
	clrtoeol();

	/* channel list */
	for (i = 0; i < listh; i++) {
		int fi = t->top + i;
		struct channel *c;
		char sat[16];
		bool sel = (fi == t->sel);

		y = 2 + i;
		if (fi >= t->nfilt) {
			move(y, 0);
			clrtoeol();
			continue;
		}
		c = &t->list->ch[t->filt[fi]];
		sat_format(c->sat, sat, sizeof(sat));
		{
			char nm[40];

			snprintf(nm, sizeof(nm), "%s%s", c->scrambled ? "$" : "", c->name);
			snprintf(buf, sizeof(buf), " %-33.33s %-5s %-6s %8.3f %c  %-18.18s",
				 nm, psi_type_name(c->type), sat, c->freq_khz / 1000.0,
				 c->pol_h ? 'H' : 'V', c->provider);
		}

		if (sel)
			attron(has_colors() ? COLOR_PAIR(CP_SEL) : A_REVERSE);
		else if (t->filt[fi] == t->cur_idx && has_colors())
			attron(COLOR_PAIR(CP_PLAYING) | A_BOLD);
		else if (c->scrambled && has_colors())
			attron(COLOR_PAIR(CP_SCR));
		mvprintw(y, 0, "%-*.*s", cols, cols, buf);
		if (sel)
			attroff(has_colors() ? COLOR_PAIR(CP_SEL) : A_REVERSE);
		else if (t->filt[fi] == t->cur_idx && has_colors())
			attroff(COLOR_PAIR(CP_PLAYING) | A_BOLD);
		else if (c->scrambled && has_colors())
			attroff(COLOR_PAIR(CP_SCR));
	}

	/* status line */
	move(rows - 2, 0);
	if (t->msg[0]) {
		if (has_colors())
			attron(COLOR_PAIR(t->msg_err ? CP_SCR : CP_LOCK));
		mvprintw(rows - 2, 0, "%-*.*s", cols, cols, t->msg);
		if (has_colors())
			attroff(COLOR_PAIR(t->msg_err ? CP_SCR : CP_LOCK));
	} else if (t->cur_valid) {
		char st[256];

		format_status(t->a, st, sizeof(st), &t->last_bytes, &t->last_t);
		snprintf(buf, sizeof(buf), " %s  %.3f %c  |  %s", t->cur.name,
			 t->cur.freq_khz / 1000.0, t->cur.pol_h ? 'H' : 'V', st);
		mvprintw(rows - 2, 0, "%-*.*s", cols, cols, buf);
	} else {
		clrtoeol();
	}

	/* help line */
	if (has_colors())
		attron(COLOR_PAIR(CP_HEADER));
	else
		attron(A_REVERSE);
	mvprintw(rows - 1, 0, "%-*s", cols,
		 " up/down  Enter play  type search  ^F sat  ^O sort  ^N scan  ^A all  Esc quit ");
	if (has_colors())
		attroff(COLOR_PAIR(CP_HEADER));
	else
		attroff(A_REVERSE);

	refresh();
}

static void tui_set_msg(struct tui *t, bool err, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(t->msg, sizeof(t->msg), fmt, ap);
	va_end(ap);
	t->msg_err = err;
}

/* ---- scan menu (inside the TUI) ------------------------------------------ */

#define SCANLOG_MAX 400
static char scanlog[SCANLOG_MAX][180];
static int scanlog_n;

static void tui_scan_draw(const char *title, bool done)
{
	int rows = LINES, cols = COLS, h = rows - 3, i, first;

	erase();
	if (has_colors())
		attron(COLOR_PAIR(CP_HEADER));
	else
		attron(A_REVERSE);
	mvprintw(0, 0, "%-*.*s", cols, cols, title);
	if (has_colors())
		attroff(COLOR_PAIR(CP_HEADER));
	else
		attroff(A_REVERSE);

	first = scanlog_n > h ? scanlog_n - h : 0;
	for (i = 0; first + i < scanlog_n && i < h; i++)
		mvprintw(2 + i, 0, "%-*.*s", cols, cols, scanlog[first + i]);

	if (has_colors())
		attron(COLOR_PAIR(done ? CP_LOCK : CP_HEADER));
	else
		attron(A_REVERSE);
	mvprintw(rows - 1, 0, "%-*s", cols,
		 done ? " scan complete  -  press any key " : " scanning...  Esc/q = stop ");
	if (has_colors())
		attroff(COLOR_PAIR(done ? CP_LOCK : CP_HEADER));
	else
		attroff(A_REVERSE);
	refresh();
}

/* Scan progress callback: append the line, poll for an abort, redraw. */
static void tui_scan_progress(const char *line)
{
	int ch = getch();	/* nodelay is on during the scan */

	if (ch == 27 || ch == 'q')
		g_scan_stop = 1;
	if (scanlog_n < SCANLOG_MAX) {
		strlcpy(scanlog[scanlog_n++], line, sizeof(scanlog[0]));
	} else {
		memmove(scanlog[0], scanlog[1], (SCANLOG_MAX - 1) * sizeof(scanlog[0]));
		strlcpy(scanlog[SCANLOG_MAX - 1], line, sizeof(scanlog[0]));
	}
	tui_scan_draw(" Scan ", false);
}

/* One-line text input with an editable default. Returns false on Esc. */
static bool tui_input_line(const char *prompt, const char *def, char *out, size_t len)
{
	size_t n;

	strlcpy(out, def ? def : "", len);
	n = strlen(out);
	curs_set(1);
	for (;;) {
		int rows = LINES, cols = COLS, px = (int)strlen(prompt) + 1, ch;

		if (has_colors())
			attron(COLOR_PAIR(CP_LOCK));
		mvprintw(rows - 2, 0, "%-*.*s", cols, cols, prompt);
		if (has_colors())
			attroff(COLOR_PAIR(CP_LOCK));
		mvprintw(rows - 2, px, "%s", out);
		clrtoeol();
		refresh();

		ch = getch();
		if (ch == ERR) {
			if (g_stop) {
				curs_set(0);
				return false;
			}
			continue;
		}
		if (ch == 27) {
			curs_set(0);
			return false;
		}
		if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
			curs_set(0);
			return true;
		}
		if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
			if (n > 0)
				out[--n] = 0;
			continue;
		}
		if (ch >= 32 && ch < 127 && n + 1 < len) {
			out[n++] = ch;
			out[n] = 0;
		}
	}
}

/* Prompts for a transponder (freq/pol/sr), seeding from a channel if given. */
static bool tui_input_tp(struct tui *t, const struct channel *seed)
{
	struct options *o = &t->a->opt;
	char buf[64], def[64];

	if (seed)
		snprintf(def, sizeof(def), "%.3f", seed->freq_khz / 1000.0);
	else
		def[0] = 0;
	if (!tui_input_line(" Frequency (MHz): ", def, buf, sizeof(buf)))
		return false;
	o->req.freq_khz = (u32)(strtod(buf, NULL) * 1000.0 + 0.5);

	snprintf(def, sizeof(def), "%c", seed && !seed->pol_h ? 'V' : 'H');
	if (!tui_input_line(" Polarization (H/V): ", def, buf, sizeof(buf)))
		return false;
	o->req.pol_h = (buf[0] == 'H' || buf[0] == 'h');

	snprintf(def, sizeof(def), "%u", seed ? seed->symbol_rate / 1000 : 27500);
	if (!tui_input_line(" Symbol rate (ksym/s): ", def, buf, sizeof(buf)))
		return false;
	o->req.symbol_rate = (u32)atoi(buf) * 1000;
	o->req.delsys = DELSYS_NONE;
	return o->req.freq_khz && o->req.symbol_rate;
}

/* Prompts for a scan target: satellite position and how to reach it. */
static bool tui_input_target(int *sat, struct rotor_target *rt)
{
	char buf[64];

	if (!tui_input_line(" Satellite (e.g. 13E, 19.2E, 30W): ", "", buf, sizeof(buf)))
		return false;
	if (sat_parse(buf, sat))
		return false;
	if (!tui_input_line(" Rotor (DiSEqC N, usals:LAT,LON, or -): ", "-", buf, sizeof(buf)))
		return false;
	if (rotor_target_parse(buf, rt))
		memset(rt, 0, sizeof(*rt));
	return true;
}

/* DiSEqC 1.2 / USALS target -> rotor config, so the scan points the dish. */
static void cfg_rotor_from_target(struct rotor_config *rc, const struct rotor_target *t)
{
	double speed = rc->speed;

	memset(rc, 0, sizeof(*rc));
	rc->speed = speed;
	if (t->usals) {
		rc->mode = ROTOR_USALS;
		rc->lat = t->lat;
		rc->lon = t->lon;
	} else if (t->pos > 0) {
		rc->mode = ROTOR_DISEQC12;
		rc->fixed_index = t->pos;
	}
}

static void tui_reload(struct tui *t)
{
	channel_list_free(t->list);
	channels_load(t->a->opt.channels, t->list);
	channel_list_sort(t->list);
	if (t->list->n > t->filt_cap) {
		int *f = realloc(t->filt, t->list->n * sizeof(int));

		if (f) {
			t->filt = f;
			t->filt_cap = t->list->n;
		}
	}
	t->cur_valid = false;
	t->cur_idx = -1;
	tui_filter(t);
}

/* Runs one scan method with on-screen progress, then reloads the channel list. */
static void tui_run_scan(struct tui *t, int method, int sat, const struct rotor_target *rt)
{
	struct app *a = t->a;
	int ret;

	a->opt.req.sat = sat;
	a->opt.req.rotor = *rt;
	cfg_rotor_from_target(&a->cfg.rotor, rt);

	scanlog_n = 0;
	g_scan_stop = 0;
	g_scan_progress = tui_scan_progress;
	nodelay(stdscr, TRUE);
	tui_scan_draw(" Scan ", false);

	if (method == 2) {
		a->opt.have_pol = false;	/* both polarizations */
		a->opt.no_services = false;
		ret = mode_blindscan(a);
	} else {
		a->opt.scan_single = (method == 0);
		ret = mode_scan(a);
	}

	nodelay(stdscr, FALSE);
	timeout(500);
	g_scan_progress = NULL;

	scan_report("");
	scan_report(ret ? " Scan finished: nothing saved." : " Scan complete.");
	tui_scan_draw(" Scan ", true);
	timeout(-1);
	getch();
	timeout(500);

	tui_reload(t);
}

/* Satellite filter: pick one of the satellites present in the list, or All. */
static void tui_sat_menu(struct tui *t)
{
	int sats[32], cnt[32], ns = 0;
	int i, j, choice = 0, total = t->list->n;

	for (i = 0; i < t->list->n; i++) {
		int sv = t->list->ch[i].sat;

		for (j = 0; j < ns; j++)
			if (sats[j] == sv)
				break;
		if (j == ns && ns < 32) {
			sats[ns] = sv;
			cnt[ns] = 0;
			ns++;
		}
		if (j < ns)
			cnt[j]++;
	}
	/* keep the current filter highlighted */
	if (t->sat_filter_on)
		for (j = 0; j < ns; j++)
			if (sats[j] == t->sat_filter)
				choice = j + 1;

	for (;;) {
		int rows = LINES, cols = COLS, ch;

		erase();
		if (has_colors())
			attron(COLOR_PAIR(CP_HEADER));
		else
			attron(A_REVERSE);
		mvprintw(0, 0, "%-*s", cols, " Filter by satellite ");
		if (has_colors())
			attroff(COLOR_PAIR(CP_HEADER));
		else
			attroff(A_REVERSE);

		if (choice == 0)
			attron(has_colors() ? COLOR_PAIR(CP_SEL) : A_REVERSE);
		mvprintw(2, 2, " All satellites (%d) ", total);
		if (choice == 0)
			attroff(has_colors() ? COLOR_PAIR(CP_SEL) : A_REVERSE);

		for (i = 0; i < ns && i < rows - 5; i++) {
			char sn[16];

			sat_format(sats[i], sn, sizeof(sn));
			if (choice == i + 1)
				attron(has_colors() ? COLOR_PAIR(CP_SEL) : A_REVERSE);
			mvprintw(3 + i, 2, " %-8s (%d) ", sn, cnt[i]);
			if (choice == i + 1)
				attroff(has_colors() ? COLOR_PAIR(CP_SEL) : A_REVERSE);
		}
		mvprintw(rows - 1, 0, " up/down  Enter select  Esc cancel");
		refresh();

		ch = getch();
		if (ch == ERR) {
			if (g_stop)
				return;
			continue;
		}
		if (ch == 27)
			return;
		if (ch == KEY_UP && choice > 0) {
			choice--;
			continue;
		}
		if (ch == KEY_DOWN && choice < ns) {
			choice++;
			continue;
		}
		if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
			if (choice == 0) {
				t->sat_filter_on = false;
			} else {
				t->sat_filter_on = true;
				t->sat_filter = sats[choice - 1];
			}
			tui_filter(t);
			return;
		}
	}
}

static void tui_scan_menu(struct tui *t)
{
	static const char *methods[] = {
		"Single transponder",
		"Whole satellite from known TPs (NIT)",
		"Blind scan (whole satellite)",
		"Change target satellite...",
	};
	const struct channel *sel = t->nfilt ? &t->list->ch[t->filt[t->sel]] : NULL;
	struct rotor_target rt;
	char satname[16], rtname[48];
	int sat, choice = 0;

	/* default target: the selected channel's satellite */
	if (sel) {
		sat = sel->sat;
		rt = sel->rotor;
	} else {
		sat = SAT_UNKNOWN;
		memset(&rt, 0, sizeof(rt));
	}

	for (;;) {
		int rows = LINES, cols = COLS, i, ch;
		const struct channel *seed;

		sat_format(sat, satname, sizeof(satname));
		rotor_target_format(&rt, rtname, sizeof(rtname));

		erase();
		if (has_colors())
			attron(COLOR_PAIR(CP_HEADER));
		else
			attron(A_REVERSE);
		mvprintw(0, 0, "%-*s", cols, " Scan ");
		if (has_colors())
			attroff(COLOR_PAIR(CP_HEADER));
		else
			attroff(A_REVERSE);

		mvprintw(2, 2, "Target: %s   rotor: %s",
			 sat == SAT_UNKNOWN ? "(not set)" : satname, rtname);
		for (i = 0; i < 4; i++) {
			if (i == choice)
				attron(has_colors() ? COLOR_PAIR(CP_SEL) : A_REVERSE);
			mvprintw(4 + i, 2, " %d. %-50s", i + 1, methods[i]);
			if (i == choice)
				attroff(has_colors() ? COLOR_PAIR(CP_SEL) : A_REVERSE);
		}
		mvprintw(rows - 1, 0, " up/down  Enter select  Esc cancel");
		refresh();

		ch = getch();
		if (ch == ERR) {
			if (g_stop)
				return;
			continue;
		}
		if (ch == 27)
			return;
		if (ch == KEY_UP && choice > 0) {
			choice--;
			continue;
		}
		if (ch == KEY_DOWN && choice < 3) {
			choice++;
			continue;
		}
		if (ch != '\n' && ch != '\r' && ch != KEY_ENTER)
			continue;

		if (choice == 3) {			/* change target */
			tui_input_target(&sat, &rt);
			continue;
		}
		if (sat == SAT_UNKNOWN && !tui_input_target(&sat, &rt))
			continue;		/* a target is required */

		/* seed the TP prompt from the selected channel only if same sat */
		seed = (sel && sel->sat == sat) ? sel : NULL;
		if (choice == 2 || tui_input_tp(t, seed))
			tui_run_scan(t, choice, sat, &rt);
		return;
	}
}

static void tui_play(struct tui *t, int list_idx)
{
	struct app *a = t->a;
	const struct channel *c = &t->list->ch[list_idx];
	char url[128];
	uint64_t deadline;
	bool ready = false, missing = false;

	if (!t->cur_valid || !same_tp(c, &t->cur)) {
		struct tune_req req = {
			.freq_khz = c->freq_khz, .pol_h = c->pol_h,
			.symbol_rate = c->symbol_rate, .delsys = c->delsys,
			.stream_id = NO_STREAM_ID_FILTER, .sat = c->sat, .rotor = c->rotor,
		};

		tui_set_msg(t, false, " Tuning %s  %.3f %c %u ...", c->name,
			    c->freq_khz / 1000.0, c->pol_h ? 'H' : 'V', c->symbol_rate / 1000);
		tui_draw(t);
		t->cur_valid = false;
		t->cur_idx = -1;
		if (retune(a, &req, 5000)) {
			req.delsys = DELSYS_NONE;
			if (retune(a, &req, 5000)) {
				tui_set_msg(t, true, " No signal on %s (feed off air?)", c->name);
				return;
			}
		}
		t->cur = *c;
		t->cur_valid = true;
	}

	tui_set_msg(t, false, " Waiting for %s ...", c->name);
	tui_draw(t);
	deadline = now_ms() + 5000;
	while (!g_stop && !ready && !missing && now_ms() < deadline) {
		struct psi_service *s;

		pthread_mutex_lock(&a->psi.lock);
		s = psi_find_service(&a->psi, c->sid);
		ready = s && s->in_pat && s->have_pmt;
		missing = a->psi.have_pat && (!s || !s->in_pat);
		pthread_mutex_unlock(&a->psi.lock);
		if (!ready && !missing)
			msleep(100);
	}
	if (missing) {
		tui_set_msg(t, true, " '%s' is not on this transponder (rescan?)", c->name);
		return;
	}
	if (ready && !service_on_air(a, c->sid)) {
		tui_set_msg(t, true, " '%s' is not broadcasting right now", c->name);
		return;
	}

	t->cur_idx = list_idx;
	snprintf(url, sizeof(url), "http://127.0.0.1:%d/%u", a->opt.http_port, c->sid);
	launch_player(a, url);
	if (!t->msg_err)
		tui_set_msg(t, false, " Playing %s  ->  %s", c->name, url);
}

static int mode_interactive(struct app *a)
{
	struct channel_list list = { 0 };
	struct tui t;
	int ch;

	/* an empty list is fine: the TUI opens straight into the scan menu */
	channels_load(a->opt.channels, &list);
	channel_list_sort(&list);

	if (tsout_add_http(a->out, a->opt.http_port) || tsout_start(a->out)) {
		channel_list_free(&list);
		return 1;
	}

	memset(&t, 0, sizeof(t));
	t.a = a;
	t.list = &list;
	t.cur_idx = -1;
	t.last_t = now_ms();
	t.filt = malloc((list.n + 1) * sizeof(int));
	t.filt_cap = list.n;
	if (!t.filt) {
		channel_list_free(&list);
		return 1;
	}

	initscr();
	cbreak();
	noecho();
	keypad(stdscr, TRUE);
	curs_set(0);
	set_escdelay(25);
	timeout(500);		/* refresh the signal line twice a second */
	if (has_colors()) {
		start_color();
		use_default_colors();
		init_pair(CP_HEADER, COLOR_WHITE, COLOR_BLUE);
		init_pair(CP_SEL, COLOR_BLACK, COLOR_CYAN);
		init_pair(CP_SCR, COLOR_YELLOW, -1);
		init_pair(CP_LOCK, COLOR_GREEN, -1);
		init_pair(CP_DIM, COLOR_WHITE, -1);
		init_pair(CP_PLAYING, COLOR_GREEN, -1);
	}

	tui_filter(&t);
	if (list.n == 0)	/* no channels yet: go straight to the scan menu */
		tui_scan_menu(&t);
	while (!g_stop) {
		tui_draw(&t);
		ch = getch();
		if (ch == ERR)
			continue;	/* timeout: redraw with fresh signal */
		t.msg[0] = 0;		/* clear transient message on any key */

		switch (ch) {
		case KEY_UP:
			if (t.sel > 0)
				t.sel--;
			break;
		case KEY_DOWN:
			if (t.sel < t.nfilt - 1)
				t.sel++;
			break;
		case KEY_PPAGE:
			t.sel -= LINES - 4;
			if (t.sel < 0)
				t.sel = 0;
			break;
		case KEY_NPAGE:
			t.sel += LINES - 4;
			if (t.sel >= t.nfilt)
				t.sel = t.nfilt ? t.nfilt - 1 : 0;
			break;
		case KEY_HOME:
			t.sel = 0;
			break;
		case KEY_END:
			t.sel = t.nfilt ? t.nfilt - 1 : 0;
			break;
		case '\n':
		case '\r':
		case KEY_ENTER:
			if (t.nfilt)
				tui_play(&t, t.filt[t.sel]);
			break;
		case 1:		/* Ctrl-A: toggle scrambled/data visibility */
			t.show_all = !t.show_all;
			tui_filter(&t);
			break;
		case 6:		/* Ctrl-F: filter by satellite */
			tui_sat_menu(&t);
			break;
		case 15:	/* Ctrl-O: cycle sort name/freq/sat */
			t.sort_mode = (t.sort_mode + 1) % 3;
			tui_filter(&t);
			break;
		case 14:	/* Ctrl-N / F2: scan menu */
		case KEY_F(2):
			tui_scan_menu(&t);
			break;
		case KEY_BACKSPACE:
		case 127:
		case 8:
			if (t.search[0]) {
				t.search[strlen(t.search) - 1] = 0;
				tui_filter(&t);
			}
			break;
		case 27:	/* Esc: clear the search, or quit if already empty */
			if (t.search[0]) {
				t.search[0] = 0;
				tui_filter(&t);
			} else {
				g_stop = 1;
			}
			break;
		default:
			if (ch >= 32 && ch < 127) {
				size_t l = strlen(t.search);

				if (l + 1 < sizeof(t.search)) {
					t.search[l] = ch;
					t.search[l + 1] = 0;
					tui_filter(&t);
				}
			}
			break;
		}
	}

	endwin();
	free(t.filt);
	channel_list_free(&list);
	return 0;
}

/* ---- main ---------------------------------------------------------------- */

/* --set-position SAT=N: edits the channel list, no hardware involved. */
static int set_position(const struct options *opt)
{
	struct channel_list list = { 0 };
	const char *eq = strchr(opt->set_position, '=');
	char satstr[32], name[16];
	struct rotor_target t;
	int sat, n;

	if (!eq || (size_t)(eq - opt->set_position) >= sizeof(satstr)) {
		LOG("invalid --set-position, expected SAT=N (e.g. 13E=1)\n");
		return 1;
	}
	memcpy(satstr, opt->set_position, eq - opt->set_position);
	satstr[eq - opt->set_position] = 0;
	if (sat_parse(satstr, &sat) || rotor_target_parse(eq + 1, &t)) {
		LOG("invalid --set-position, expected SAT=N (1-255, 0 removes it) or "
		    "SAT=usals:LAT,LON\n");
		return 1;
	}
	if (channels_load(opt->channels, &list)) {
		LOG("no channel list '%s'\n", opt->channels);
		return 1;
	}
	n = channel_list_set_rotor(&list, sat, &t);
	sat_format(sat, name, sizeof(name));
	if (n == 0) {
		LOG("no channels of %s in %s\n", name, opt->channels);
		channel_list_free(&list);
		return 1;
	}
	channel_list_sort(&list);
	if (channels_save(opt->channels, &list)) {
		channel_list_free(&list);
		return 1;
	}
	rotor_target_format(&t, satstr, sizeof(satstr));
	LOG("%s: rotor %s set for %d channels in %s\n", name, satstr, n, opt->channels);
	channel_list_free(&list);
	return 0;
}

/* Prepends the options found in ./tbs5580.rc to the command line. */
static void load_rc(int *argc, char ***argv)
{
	static char *args[256];
	static char buf[4096];
	char line[512];
	size_t used = 0;
	int n = 0, i;
	FILE *f;

	f = fopen("tbs5580.rc", "r");
	if (!f)
		return;
	args[n++] = (*argv)[0];
	while (fgets(line, sizeof(line), f)) {
		char *tok, *save;

		if (line[strspn(line, " \t")] == '#')
			continue;
		for (tok = strtok_r(line, " \t\r\n", &save); tok && n < 200;
		     tok = strtok_r(NULL, " \t\r\n", &save)) {
			size_t len = strlen(tok) + 1;

			if (used + len > sizeof(buf))
				break;
			memcpy(buf + used, tok, len);
			args[n++] = buf + used;
			used += len;
		}
	}
	fclose(f);
	for (i = 1; i < *argc && n < 255; i++)
		args[n++] = (*argv)[i];
	args[n] = NULL;
	*argc = n;
	*argv = args;
}

int main(int argc, char **argv)
{
	static const struct option long_opts[] = {
		{ "freq", required_argument, NULL, 'f' },
		{ "pol", required_argument, NULL, 'p' },
		{ "sr", required_argument, NULL, 's' },
		{ "delsys", required_argument, NULL, 'd' },
		{ "lnb", required_argument, NULL, 'l' },
		{ "diseqc", required_argument, NULL, 'D' },
		{ "isi", required_argument, NULL, 1 },
		{ "pls-gold", required_argument, NULL, 2 },
		{ "pls-root", required_argument, NULL, 3 },
		{ "inversion", required_argument, NULL, 4 },
		{ "output", required_argument, NULL, 'o' },
		{ "udp", required_argument, NULL, 'u' },
		{ "http", required_argument, NULL, 'H' },
		{ "sid", required_argument, NULL, 'S' },
		{ "time", required_argument, NULL, 't' },
		{ "info", no_argument, NULL, 5 },
		{ "fw-dir", required_argument, NULL, 6 },
		{ "no-fw", no_argument, NULL, 7 },
		{ "lock-timeout", required_argument, NULL, 8 },
		{ "usb-buffers", required_argument, NULL, 9 },
		{ "scan", no_argument, NULL, 10 },
		{ "scan-single", no_argument, NULL, 11 },
		{ "player", required_argument, NULL, 12 },
		{ "no-player", no_argument, NULL, 13 },
		{ "interactive", no_argument, NULL, 'i' },
		{ "channels", required_argument, NULL, 'c' },
		{ "blindscan", no_argument, NULL, 14 },
		{ "sat", required_argument, NULL, 15 },
		{ "usals", required_argument, NULL, 16 },
		{ "rotor", required_argument, NULL, 17 },
		{ "positions", required_argument, NULL, 18 },
		{ "rotor-speed", required_argument, NULL, 19 },
		{ "motor", required_argument, NULL, 20 },
		{ "sr-range", required_argument, NULL, 21 },
		{ "no-services", no_argument, NULL, 22 },
		{ "set-position", required_argument, NULL, 23 },
		{ "verbose", no_argument, NULL, 'v' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};
	static struct app app;
	struct options *opt = &app.opt;
	struct fe_config *cfg = &app.cfg;
	int isi = -1, pls_code = 0, pls_mode = 0;
	int c, ret = 1;

	load_rc(&argc, &argv);

	opt->mode = MODE_TUNE;
	opt->req.sat = SAT_UNKNOWN;
	opt->sr_min = 1000000;
	opt->sr_max = 45000000;
	cfg->rotor.speed = 1.5;
	opt->req.stream_id = NO_STREAM_ID_FILTER;
	opt->sid = -1;
	opt->lock_timeout = 10;
	opt->channels = "channels.conf";
	opt->n_xfers = 16;
	opt->xfer_size = 32768;
	cfg->inversion = INVERSION_AUTO;
	lnb_parse("universal", &cfg->lnb);

	while ((c = getopt_long(argc, argv, "f:p:s:d:l:D:o:u:H:S:t:c:ivh", long_opts, NULL)) != -1) {
		switch (c) {
		case 'f':
			opt->req.freq_khz = (u32)(strtod(optarg, NULL) * 1000.0 + 0.5);
			break;
		case 'p':
			if (strcasecmp(optarg, "h") == 0 || strcasecmp(optarg, "l") == 0)
				opt->req.pol_h = true;
			else if (strcasecmp(optarg, "v") == 0 || strcasecmp(optarg, "r") == 0)
				opt->req.pol_h = false;
			else {
				LOG("invalid polarization '%s'\n", optarg);
				return 1;
			}
			opt->have_pol = true;
			break;
		case 's':
			opt->req.symbol_rate = atoi(optarg);
			if (opt->req.symbol_rate < 1000000)
				opt->req.symbol_rate *= 1000;
			break;
		case 'd':
			if (strcasecmp(optarg, "s") == 0 || strcasecmp(optarg, "dvbs") == 0)
				opt->req.delsys = DELSYS_DVBS;
			else if (strcasecmp(optarg, "s2") == 0 || strcasecmp(optarg, "dvbs2") == 0)
				opt->req.delsys = DELSYS_DVBS2;
			else if (strcasecmp(optarg, "auto") == 0)
				opt->req.delsys = DELSYS_NONE;
			else {
				LOG("invalid delivery system '%s'\n", optarg);
				return 1;
			}
			break;
		case 'l':
			if (lnb_parse(optarg, &cfg->lnb)) {
				LOG("invalid LNB '%s'\n", optarg);
				return 1;
			}
			break;
		case 'D':
			cfg->diseqc = atoi(optarg);
			if (cfg->diseqc < 1 || cfg->diseqc > 4) {
				LOG("DiSEqC port must be 1-4\n");
				return 1;
			}
			break;
		case 1:
			isi = atoi(optarg);
			break;
		case 2:
			pls_code = atoi(optarg);
			pls_mode = 1;
			break;
		case 3:
			pls_code = atoi(optarg);
			pls_mode = 0;
			break;
		case 4:
			if (strcasecmp(optarg, "on") == 0)
				cfg->inversion = INVERSION_ON;
			else if (strcasecmp(optarg, "off") == 0)
				cfg->inversion = INVERSION_OFF;
			else
				cfg->inversion = INVERSION_AUTO;
			break;
		case 'o':
			opt->output = optarg;
			break;
		case 'u':
			opt->udp = optarg;
			if (strncmp(opt->udp, "udp://", 6) == 0)
				opt->udp += 6;
			break;
		case 'H':
			opt->http_port = atoi(optarg);
			break;
		case 'S':
			opt->sid = atoi(optarg);
			break;
		case 't':
			opt->duration = atoi(optarg);
			break;
		case 5:
			opt->mode = MODE_INFO;
			break;
		case 6:
			cfg->fw_dir = optarg;
			break;
		case 7:
			cfg->no_fw = true;
			break;
		case 8:
			opt->lock_timeout = atoi(optarg);
			break;
		case 9:
			if (sscanf(optarg, "%d,%d", &opt->n_xfers, &opt->xfer_size) != 2 ||
			    opt->n_xfers < 1 || opt->xfer_size < 512 || opt->xfer_size % 512) {
				LOG("invalid --usb-buffers, expected N,SIZE (SIZE multiple of 512)\n");
				return 1;
			}
			break;
		case 10:
			opt->mode = MODE_SCAN;
			break;
		case 11:
			opt->scan_single = true;
			break;
		case 12:
			opt->player = optarg;
			break;
		case 13:
			opt->no_player = true;
			break;
		case 'i':
			opt->mode = MODE_INTERACTIVE;
			break;
		case 'c':
			opt->channels = optarg;
			break;
		case 14:
			opt->mode = MODE_BLINDSCAN;
			break;
		case 15:
			if (sat_parse(optarg, &opt->req.sat)) {
				LOG("invalid satellite position '%s' (e.g. 13E, 19.2E, 30W)\n", optarg);
				return 1;
			}
			break;
		case 16:
			if (rotor_parse_site(optarg, &cfg->rotor)) {
				LOG("invalid site '%s', expected LAT,LON (e.g. 45.46N,9.19E)\n", optarg);
				return 1;
			}
			break;
		case 17:
			cfg->rotor.fixed_index = atoi(optarg);
			if (cfg->rotor.fixed_index < 1 || cfg->rotor.fixed_index > 255) {
				LOG("rotor position must be 1-255\n");
				return 1;
			}
			if (cfg->rotor.mode == ROTOR_NONE)
				cfg->rotor.mode = ROTOR_DISEQC12;
			break;
		case 18:
			if (rotor_parse_positions(optarg, &cfg->rotor)) {
				LOG("invalid positions '%s', expected e.g. 13E=1,19.2E=2\n", optarg);
				return 1;
			}
			break;
		case 19:
			cfg->rotor.speed = strtod(optarg, NULL);
			break;
		case 20: {
			char *dup = strdup(optarg), *tok, *save;

			for (tok = strtok_r(dup, ",", &save); tok && opt->n_motor < MAX_MOTOR_CMDS;
			     tok = strtok_r(NULL, ",", &save))
				opt->motor[opt->n_motor++] = tok;
			break;
		}
		case 21: {
			unsigned lo, hi;

			if (sscanf(optarg, "%u-%u", &lo, &hi) != 2 || lo < 100 || hi > 60000 || lo > hi) {
				LOG("invalid --sr-range, expected MIN-MAX in ksym/s\n");
				return 1;
			}
			opt->sr_min = lo * 1000;
			opt->sr_max = hi * 1000;
			break;
		}
		case 22:
			opt->no_services = true;
			break;
		case 23:
			opt->set_position = optarg;
			opt->mode = MODE_SET_POSITION;
			break;
		case 'v':
			g_verbose++;
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (isi >= 0)
		opt->req.stream_id = (isi & 0xff) | ((u32)(pls_code & 0x3ffff) << 8) |
				     ((u32)pls_mode << 26);

	/* --motor alone just moves the dish */
	if (opt->mode == MODE_TUNE && opt->n_motor && !opt->req.freq_khz)
		opt->mode = MODE_MOTOR;
	/* --rotor N without --sat: move to N explicitly */
	if (cfg->rotor.fixed_index && opt->req.sat == SAT_UNKNOWN && opt->n_motor < MAX_MOTOR_CMDS) {
		static char gotocmd[16];

		snprintf(gotocmd, sizeof(gotocmd), "goto:%d", cfg->rotor.fixed_index);
		memmove(&opt->motor[1], &opt->motor[0], opt->n_motor * sizeof(opt->motor[0]));
		opt->motor[0] = gotocmd;
		opt->n_motor++;
		if (opt->mode == MODE_TUNE && !opt->req.freq_khz)
			opt->mode = MODE_MOTOR;
	}

	for (c = 0; c < opt->n_motor; c++) {
		u8 msg[6];

		if (rotor_manual_msg(&cfg->rotor, opt->motor[c], msg) < 0) {
			LOG("invalid motor command '%s'%s\n", opt->motor[c],
			    strncasecmp(opt->motor[c], "gotox", 5) == 0 ? " (gotox needs --usals)" : "");
			return 1;
		}
	}

	if ((opt->mode == MODE_TUNE || opt->mode == MODE_SCAN) &&
	    (!opt->req.freq_khz || !opt->req.symbol_rate || !opt->have_pol)) {
		if (opt->mode == MODE_SCAN)
			LOG("--scan needs a starting transponder: -f, -p and -s\n");
		usage(argv[0]);
		return 1;
	}
	if (opt->mode == MODE_INTERACTIVE && !opt->http_port)
		opt->http_port = 8001;
	cfg->info = opt->mode == MODE_INFO;

	if (opt->mode == MODE_SET_POSITION)
		return set_position(opt);

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGPIPE, SIG_IGN);

	psi_init(&app.psi);
	app.out = tsout_create(32 * 1024 * 1024, &app.psi);
	if (!app.out)
		return 1;

	/* the TUI owns the screen: send stray logs to a file instead of stderr */
	if (opt->mode == MODE_INTERACTIVE) {
		fflush(stderr);
		if (!freopen("tbs5580.log", "w", stderr))
			; /* fall back to stderr */
	}

	if (frontend_open(&app.fe, cfg))
		goto out;

	for (c = 0; c < opt->n_motor && !g_stop; c++) {
		if (frontend_motor(&app.fe, cfg, opt->motor[c]))
			goto out;
		msleep(200);
	}

	switch (opt->mode) {
	case MODE_MOTOR:
	case MODE_SET_POSITION:	/* handled before opening the hardware */
		ret = 0;
		break;
	case MODE_BLINDSCAN:
		ret = mode_blindscan(&app);
		break;
	case MODE_INFO:
		LOG("hardware OK\n");
		ret = 0;
		break;
	case MODE_TUNE:
		ret = mode_tune(&app);
		break;
	case MODE_SCAN:
		ret = mode_scan(&app);
		break;
	case MODE_INTERACTIVE:
		ret = mode_interactive(&app);
		break;
	}

out:
	tbs5580_stop_stream(&app.fe.usb);
	tsout_destroy(app.out);
	frontend_close(&app.fe);
	return ret;
}
