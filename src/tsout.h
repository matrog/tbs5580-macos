/*
 * Transport stream output: ring buffer fed from the USB callback, a writer
 * thread that re-aligns to 188-byte packets, feeds the PSI parser and fans
 * out to file/stdout, UDP and a minimal HTTP server. Outputs can be
 * restricted to a single service (program), with a rewritten PAT.
 *
 * HTTP URLs:  /             whole transport stream
 *             /<sid>        single service, e.g. /3401
 *             /playlist.m3u playlist of the services on the transponder
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, version 2.
 */
#ifndef TSOUT_H
#define TSOUT_H

#include <stddef.h>
#include "common.h"
#include "psi.h"

struct tsout;

struct tsout_stats {
	uint64_t bytes_in;
	uint64_t bytes_dropped;
	uint64_t packets;
	uint64_t sync_losses;
	uint64_t tei_packets;	/* transport error indicator set */
	int http_clients;
	bool file_error;	/* write to file/stdout failed (e.g. player closed the pipe) */
};

struct tsout *tsout_create(size_t ring_size, struct psi *psi);
int tsout_add_file(struct tsout *o, const char *path);	/* "-" = stdout */
int tsout_add_udp(struct tsout *o, const char *hostport);	/* "host:port" */
int tsout_add_http(struct tsout *o, int port);
/* Restrict the file/UDP outputs to one service, -1 = whole TS. */
void tsout_set_sid(struct tsout *o, int sid);

/*
 * Channel list for the "all channels" mobile playlist (/all.m3u) and
 * tune-on-request (/tune/<index>). The list is owned by the caller and must
 * stay valid; call again after it is reloaded. NULL disables the feature.
 */
struct channel_list;
void tsout_set_channels(struct tsout *o, const struct channel_list *list);

/*
 * Remote tune hand-off to the main thread (the only one allowed to touch the
 * tuner). The main loop calls tsout_take_tune_request() regularly: it returns
 * a channel index to tune to, or -1. After tuning it calls tsout_tune_done().
 */
int tsout_take_tune_request(struct tsout *o);
void tsout_tune_done(struct tsout *o, bool ok);
int tsout_start(struct tsout *o);
void tsout_push(struct tsout *o, const u8 *buf, int len);
/* After a retune: drop buffered data, reset the PSI parser, disconnect HTTP clients. */
void tsout_reset(struct tsout *o);
/* Packets received so far on pid (a counter, compare two readings). */
u32 tsout_pid_packets(struct tsout *o, u16 pid);
void tsout_get_stats(struct tsout *o, struct tsout_stats *st);
void tsout_destroy(struct tsout *o);

#endif
