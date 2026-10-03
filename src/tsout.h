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
int tsout_start(struct tsout *o);
void tsout_push(struct tsout *o, const u8 *buf, int len);
/* After a retune: drop buffered data, reset the PSI parser, disconnect HTTP clients. */
void tsout_reset(struct tsout *o);
/* Packets received so far on pid (a counter, compare two readings). */
u32 tsout_pid_packets(struct tsout *o, u16 pid);
void tsout_get_stats(struct tsout *o, struct tsout_stats *st);
void tsout_destroy(struct tsout *o);

#endif
