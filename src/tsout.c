/*
 * Transport stream output, see tsout.h.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, version 2.
 */
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <pthread.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include "tsout.h"

#define TS_PACKET 188
#define UDP_PACKETS 7
#define WORK_SIZE (256 * 1024)
#define MAX_HTTP_CLIENTS 8

/* Keeps only the PIDs of one service and replaces the PAT. */
struct ts_filter {
	int sid;		/* -1 = whole TS */
	bool ready;
	u32 gen;
	u16 tsid;
	u16 pmt_pid;
	u8 pat_version;
	u8 pat_cc;
	u8 pidmap[8192 / 8];
};

struct http_client {
	int fd;
	struct ts_filter f;
};

struct tsout {
	struct psi *psi;

	/* ring buffer */
	u8 *ring;
	size_t size, head, tail, used;
	bool reset_pending;
	pthread_mutex_t lock;
	pthread_cond_t cond;

	/* sinks */
	FILE *file;
	bool file_is_stdout;
	int udp_fd;
	struct sockaddr_storage udp_addr;
	socklen_t udp_addrlen;
	struct ts_filter sink_filter;
	int http_fd;
	int http_port;
	struct http_client http_clients[MAX_HTTP_CLIENTS];
	int n_http_clients;
	pthread_mutex_t clients_lock;

	u8 *scratch;		/* filtered output, writer thread only */


	pthread_t writer, http_thread;
	bool writer_running, http_running;
	volatile bool stop;

	struct tsout_stats st;
	u32 pid_packets[8192];	/* packets seen per PID, writer thread only */
};

static void filter_init(struct ts_filter *f, int sid)
{
	memset(f, 0, sizeof(*f));
	f->sid = sid;
}

struct tsout *tsout_create(size_t ring_size, struct psi *psi)
{
	struct tsout *o = calloc(1, sizeof(*o));

	if (!o)
		return NULL;
	o->ring = malloc(ring_size);
	o->scratch = malloc(WORK_SIZE);
	if (!o->ring || !o->scratch) {
		free(o->ring);
		free(o->scratch);
		free(o);
		return NULL;
	}
	o->psi = psi;
	o->size = ring_size;
	o->udp_fd = -1;
	o->http_fd = -1;
	filter_init(&o->sink_filter, -1);
	pthread_mutex_init(&o->lock, NULL);
	pthread_cond_init(&o->cond, NULL);
	pthread_mutex_init(&o->clients_lock, NULL);
	return o;
}

int tsout_add_file(struct tsout *o, const char *path)
{
	if (strcmp(path, "-") == 0) {
		o->file = stdout;
		o->file_is_stdout = true;
	} else {
		o->file = fopen(path, "wb");
		if (!o->file) {
			LOG("cannot open '%s' for writing\n", path);
			return -errno;
		}
	}
	return 0;
}

int tsout_add_udp(struct tsout *o, const char *hostport)
{
	char host[256];
	const char *colon = strrchr(hostport, ':');
	struct addrinfo hints = { .ai_socktype = SOCK_DGRAM }, *res;
	int ret;

	if (!colon || colon == hostport || (size_t)(colon - hostport) >= sizeof(host)) {
		LOG("invalid UDP destination '%s', expected host:port\n", hostport);
		return -EINVAL;
	}
	memcpy(host, hostport, colon - hostport);
	host[colon - hostport] = 0;

	ret = getaddrinfo(host, colon + 1, &hints, &res);
	if (ret) {
		LOG("cannot resolve '%s': %s\n", host, gai_strerror(ret));
		return -EINVAL;
	}
	o->udp_fd = socket(res->ai_family, SOCK_DGRAM, 0);
	if (o->udp_fd < 0) {
		freeaddrinfo(res);
		return -errno;
	}
	memcpy(&o->udp_addr, res->ai_addr, res->ai_addrlen);
	o->udp_addrlen = res->ai_addrlen;
	freeaddrinfo(res);
	return 0;
}

int tsout_add_http(struct tsout *o, int port)
{
	struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(port),
				  .sin_addr.s_addr = htonl(INADDR_ANY) };
	int one = 1;

	o->http_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (o->http_fd < 0)
		return -errno;
	setsockopt(o->http_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (bind(o->http_fd, (struct sockaddr *)&sa, sizeof(sa)) ||
	    listen(o->http_fd, 4)) {
		LOG("cannot listen on HTTP port %d: %s\n", port, strerror(errno));
		close(o->http_fd);
		o->http_fd = -1;
		return -EADDRINUSE;
	}
	o->http_port = port;
	return 0;
}

void tsout_set_sid(struct tsout *o, int sid)
{
	filter_init(&o->sink_filter, sid);
}

/* ---- service filter ------------------------------------------------------ */

static void map_set(struct ts_filter *f, u16 pid)
{
	f->pidmap[pid >> 3] |= 1 << (pid & 7);
}

static bool map_has(const struct ts_filter *f, u16 pid)
{
	return f->pidmap[pid >> 3] & (1 << (pid & 7));
}

/* Rebuild the PID map when the service's PMT changed. */
static void filter_refresh(struct tsout *o, struct ts_filter *f)
{
	struct psi_service *s;
	int i;

	if (f->sid < 0 || !o->psi)
		return;
	pthread_mutex_lock(&o->psi->lock);
	s = psi_find_service(o->psi, f->sid);
	if (s && s->in_pat && s->have_pmt && (!f->ready || f->gen != s->generation)) {
		if (f->ready && f->pmt_pid != s->pmt_pid)
			f->pat_version++;
		memset(f->pidmap, 0, sizeof(f->pidmap));
		f->tsid = o->psi->tsid;
		f->pmt_pid = s->pmt_pid;
		map_set(f, s->pmt_pid);
		if (s->pcr_pid != 0x1fff)
			map_set(f, s->pcr_pid);
		for (i = 0; i < s->n_es; i++)
			map_set(f, s->es[i].pid);
		map_set(f, 0x11);	/* SDT: service name */
		map_set(f, 0x12);	/* EIT: programme guide */
		map_set(f, 0x14);	/* TDT/TOT: time */
		f->gen = s->generation;
		f->ready = true;
	}
	pthread_mutex_unlock(&o->psi->lock);
}

/* Returns the filtered data (in o->scratch) or buf itself for the whole TS. */
static const u8 *filter_apply(struct tsout *o, struct ts_filter *f, const u8 *buf,
			      size_t len, size_t *out_len)
{
	size_t i, n = 0;

	if (f->sid < 0) {
		*out_len = len;
		return buf;
	}
	filter_refresh(o, f);
	if (f->ready) {
		for (i = 0; i + TS_PACKET <= len; i += TS_PACKET) {
			const u8 *pkt = buf + i;
			u16 pid = (pkt[1] & 0x1f) << 8 | pkt[2];

			if (pid == 0) {
				if (pkt[1] & 0x40) {	/* one PAT per original PAT */
					psi_make_pat(o->scratch + n, f->tsid, f->sid, f->pmt_pid,
						     f->pat_version, f->pat_cc++);
					n += TS_PACKET;
				}
			} else if (map_has(f, pid)) {
				memcpy(o->scratch + n, pkt, TS_PACKET);
				n += TS_PACKET;
			}
		}
	}
	*out_len = n;
	return o->scratch;
}

/* ---- HTTP ---------------------------------------------------------------- */

static void http_reply(int fd, const char *status, const char *type, const char *body)
{
	char hdr[256];

	snprintf(hdr, sizeof(hdr),
		 "HTTP/1.0 %s\r\nContent-Type: %s\r\nConnection: close\r\n\r\n", status, type);
	send(fd, hdr, strlen(hdr), 0);
	if (body)
		send(fd, body, strlen(body), 0);
}

static void http_playlist(struct tsout *o, int fd, const char *host)
{
	size_t cap = 65536, n = 0;
	char *body = malloc(cap);
	int i;

	if (!body)
		return;
	n += snprintf(body + n, cap - n, "#EXTM3U\n");
	if (o->psi) {
		pthread_mutex_lock(&o->psi->lock);
		for (i = 0; i < o->psi->n_services && n < cap - 512; i++) {
			struct psi_service *s = &o->psi->services[i];

			if (!s->in_pat)
				continue;
			n += snprintf(body + n, cap - n, "#EXTINF:-1,%s%s\nhttp://%s/%u\n",
				      s->name[0] ? s->name : "Service", s->scrambled ? " ($)" : "",
				      host, s->sid);
		}
		pthread_mutex_unlock(&o->psi->lock);
	}
	http_reply(fd, "200 OK", "audio/x-mpegurl", body);
	free(body);
}

static void http_handle(struct tsout *o, int fd)
{
	static const char ts_hdr[] =
		"HTTP/1.0 200 OK\r\n"
		"Content-Type: video/mp2t\r\n"
		"Cache-Control: no-cache\r\n"
		"Connection: close\r\n\r\n";
	struct timeval tv = { 1, 0 }, sndtv = { 2, 0 };
	char req[4096], path[256], host[128];
	const char *h;
	ssize_t r;
	int one = 1, sid = -1;

	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &sndtv, sizeof(sndtv));

	r = recv(fd, req, sizeof(req) - 1, 0);
	if (r <= 0)
		goto close;
	req[r] = 0;
	if (sscanf(req, "%*s %255s", path) != 1)
		strcpy(path, "/");

	snprintf(host, sizeof(host), "127.0.0.1:%d", o->http_port);
	h = strcasestr(req, "\nHost:");
	if (h) {
		h += 6;
		while (*h == ' ')
			h++;
		sscanf(h, "%127[^\r\n]", host);
	}

	if (strcmp(path, "/playlist.m3u") == 0) {
		http_playlist(o, fd, host);
		goto close;
	}
	if (path[0] == '/' && isdigit((unsigned char)path[1])) {
		sid = atoi(path + 1);
	} else if (strcmp(path, "/") != 0 && strcmp(path, "/ts") != 0) {
		http_reply(fd, "404 Not Found", "text/plain", "not found\n");
		goto close;
	}

	if (send(fd, ts_hdr, sizeof(ts_hdr) - 1, 0) < 0)
		goto close;

	pthread_mutex_lock(&o->clients_lock);
	if (o->n_http_clients < MAX_HTTP_CLIENTS) {
		struct http_client *c = &o->http_clients[o->n_http_clients++];

		c->fd = fd;
		filter_init(&c->f, sid);
		LOG("HTTP client connected: %s (%d)\n", path, o->n_http_clients);
		fd = -1;
	}
	pthread_mutex_unlock(&o->clients_lock);
close:
	if (fd >= 0)
		close(fd);
}

static void *tsout_http_thread(void *arg)
{
	struct tsout *o = arg;

	while (!o->stop) {
		struct timeval tv = { 0, 200000 };
		fd_set rfds;
		int fd;

		FD_ZERO(&rfds);
		FD_SET(o->http_fd, &rfds);
		if (select(o->http_fd + 1, &rfds, NULL, NULL, &tv) <= 0)
			continue;
		fd = accept(o->http_fd, NULL, NULL);
		if (fd >= 0)
			http_handle(o, fd);
	}
	return NULL;
}

static void http_drop_clients(struct tsout *o)
{
	int i;

	pthread_mutex_lock(&o->clients_lock);
	for (i = 0; i < o->n_http_clients; i++)
		close(o->http_clients[i].fd);
	o->n_http_clients = 0;
	pthread_mutex_unlock(&o->clients_lock);
}

/* ---- writer -------------------------------------------------------------- */

static void tsout_emit(struct tsout *o, const u8 *buf, size_t len)
{
	const u8 *data = buf;
	size_t n = 0, off;
	int i;

	if (o->file || o->udp_fd >= 0)
		data = filter_apply(o, &o->sink_filter, buf, len, &n);

	if (o->file && !o->st.file_error && n) {
		if (fwrite(data, 1, n, o->file) != n ||
		    (o->file_is_stdout && fflush(o->file)))
			o->st.file_error = true;
	}

	if (o->udp_fd >= 0) {
		for (off = 0; off < n; off += TS_PACKET * UDP_PACKETS) {
			size_t chunk = n - off;

			if (chunk > TS_PACKET * UDP_PACKETS)
				chunk = TS_PACKET * UDP_PACKETS;
			sendto(o->udp_fd, data + off, chunk, 0,
			       (struct sockaddr *)&o->udp_addr, o->udp_addrlen);
		}
	}

	if (o->http_fd >= 0) {
		pthread_mutex_lock(&o->clients_lock);
		for (i = 0; i < o->n_http_clients; i++) {
			struct http_client *c = &o->http_clients[i];
			ssize_t sent = 0, r = 1;

			data = filter_apply(o, &c->f, buf, len, &n);
			while ((size_t)sent < n) {
				r = send(c->fd, data + sent, n - sent, 0);
				if (r <= 0)
					break;
				sent += r;
			}
			if (r <= 0) {
				close(c->fd);
				o->http_clients[i] = o->http_clients[--o->n_http_clients];
				i--;
				LOG("HTTP client disconnected (%d)\n", o->n_http_clients);
			}
		}
		pthread_mutex_unlock(&o->clients_lock);
	}
}

static bool is_sync(const u8 *buf, size_t i, size_t len)
{
	return buf[i] == 0x47 && (i + TS_PACKET == len || buf[i + TS_PACKET] == 0x47);
}

/* Re-align the raw USB byte stream on 188-byte TS packets. */
static size_t tsout_align(struct tsout *o, u8 *buf, size_t len)
{
	size_t i = 0, start = 0;

	while (i + TS_PACKET <= len) {
		if (is_sync(buf, i, len)) {
			u16 pid = (buf[i + 1] & 0x1f) << 8 | buf[i + 2];

			if (buf[i + 1] & 0x80) {
				o->st.tei_packets++;
			} else if (o->psi) {
				psi_packet(o->psi, buf + i);
			}
			o->pid_packets[pid]++;
			o->st.packets++;
			i += TS_PACKET;
			continue;
		}
		/* lost sync: flush what we have and search for the next sync byte */
		if (i > start)
			tsout_emit(o, buf + start, i - start);
		o->st.sync_losses++;
		do {
			i++;
		} while (i + TS_PACKET <= len && !is_sync(buf, i, len));
		start = i;
	}
	if (i > start)
		tsout_emit(o, buf + start, i - start);
	/* keep the incomplete tail */
	memmove(buf, buf + i, len - i);
	return len - i;
}

static void *tsout_writer(void *arg)
{
	struct tsout *o = arg;
	u8 *work = malloc(WORK_SIZE);
	size_t have = 0;

	if (!work)
		return NULL;

	while (!o->stop) {
		size_t n;

		pthread_mutex_lock(&o->lock);
		if (o->reset_pending) {
			have = 0;
			if (o->psi)
				psi_reset(o->psi);
			filter_init(&o->sink_filter, o->sink_filter.sid);
			http_drop_clients(o);
			o->reset_pending = false;
			pthread_cond_broadcast(&o->cond);
		}
		while (o->used == 0 && !o->stop && !o->reset_pending) {
			struct timespec ts;
			struct timeval now;

			gettimeofday(&now, NULL);
			ts.tv_sec = now.tv_sec;
			ts.tv_nsec = now.tv_usec * 1000 + 100000000;
			if (ts.tv_nsec >= 1000000000) {
				ts.tv_sec++;
				ts.tv_nsec -= 1000000000;
			}
			pthread_cond_timedwait(&o->cond, &o->lock, &ts);
		}
		if (o->reset_pending) {
			pthread_mutex_unlock(&o->lock);
			continue;
		}
		n = WORK_SIZE - have;
		if (n > o->used)
			n = o->used;
		if (n > o->size - o->tail)
			n = o->size - o->tail;
		memcpy(work + have, o->ring + o->tail, n);
		o->tail = (o->tail + n) % o->size;
		o->used -= n;
		pthread_mutex_unlock(&o->lock);

		have = tsout_align(o, work, have + n);
	}
	free(work);
	return NULL;
}

int tsout_start(struct tsout *o)
{
	if (o->writer_running)
		return 0;	/* already streaming */
	if (pthread_create(&o->writer, NULL, tsout_writer, o))
		return -ENOMEM;
	o->writer_running = true;
	if (o->http_fd >= 0) {
		if (pthread_create(&o->http_thread, NULL, tsout_http_thread, o))
			return -ENOMEM;
		o->http_running = true;
	}
	return 0;
}

void tsout_push(struct tsout *o, const u8 *buf, int len)
{
	size_t first;

	pthread_mutex_lock(&o->lock);
	o->st.bytes_in += len;
	if ((size_t)len > o->size - o->used) {
		o->st.bytes_dropped += len;
		pthread_mutex_unlock(&o->lock);
		return;
	}
	first = o->size - o->head;
	if (first > (size_t)len)
		first = len;
	memcpy(o->ring + o->head, buf, first);
	memcpy(o->ring, buf + first, len - first);
	o->head = (o->head + len) % o->size;
	o->used += len;
	pthread_cond_broadcast(&o->cond);
	pthread_mutex_unlock(&o->lock);
}

void tsout_reset(struct tsout *o)
{
	pthread_mutex_lock(&o->lock);
	o->head = o->tail = o->used = 0;
	o->reset_pending = true;
	pthread_cond_broadcast(&o->cond);
	/* wait for the writer thread, so no stale PSI survives the retune */
	while (o->reset_pending && o->writer_running)
		pthread_cond_wait(&o->cond, &o->lock);
	pthread_mutex_unlock(&o->lock);
}

u32 tsout_pid_packets(struct tsout *o, u16 pid)
{
	return o->pid_packets[pid & 0x1fff];
}


void tsout_get_stats(struct tsout *o, struct tsout_stats *st)
{
	pthread_mutex_lock(&o->lock);
	*st = o->st;
	pthread_mutex_unlock(&o->lock);
	pthread_mutex_lock(&o->clients_lock);
	st->http_clients = o->n_http_clients;
	pthread_mutex_unlock(&o->clients_lock);
}

void tsout_destroy(struct tsout *o)
{
	if (!o)
		return;
	o->stop = true;
	pthread_mutex_lock(&o->lock);
	pthread_cond_broadcast(&o->cond);
	pthread_mutex_unlock(&o->lock);
	if (o->writer_running)
		pthread_join(o->writer, NULL);
	if (o->http_running)
		pthread_join(o->http_thread, NULL);
	if (o->file && !o->file_is_stdout)
		fclose(o->file);
	else if (o->file)
		fflush(o->file);
	if (o->udp_fd >= 0)
		close(o->udp_fd);
	http_drop_clients(o);
	if (o->http_fd >= 0)
		close(o->http_fd);
	free(o->ring);
	free(o->scratch);
	free(o);
}
