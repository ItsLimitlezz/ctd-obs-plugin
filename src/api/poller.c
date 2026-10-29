/*
 * The MediaMTX poller. See poller.h.
 */

#include "poller.h"
#include "../obs/servers.h"

#include <obs-module.h>
#include <util/bmem.h>
#include <util/darray.h>
#include <util/platform.h>
#include <util/threading.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define POLL_EVERY_MS 2000
#define VERSION_EVERY_NS (60ull * 1000000000ull)
#define SRT_RETRY_NS (30ull * 1000000000ull) /* a server without the SRT list is asked again this often */

/* what the last poll saw, to turn counters into rates */
struct counter {
	char *key; /* a path's name, or an SRT connection's id */
	uint64_t a;
	uint64_t b;
};

struct mtx_poller {
	char id[48];
	long refs; /* guarded by the registry's mutex */
	pthread_t thread;
	bool thread_running;
	os_event_t *wake;
	volatile bool stopping;

	pthread_mutex_t mutex; /* guards snapshot */
	struct mtx_poll_snapshot snapshot;

	/* the thread's own */
	DARRAY(struct counter) bytes; /* per path: bytes received */
	DARRAY(struct counter) srt;   /* per SRT connection: packets received, packets lost */
	uint64_t last_poll_ns;
	uint64_t version_ns;
	uint64_t srt_retry_ns;
	bool srt_missing;
	char version[32];
	enum mtx_api_status logged; /* the last status logged, so the log shows changes only */
	bool logged_once;
	size_t logged_total;
	size_t logged_live;
};

static struct {
	pthread_mutex_t mutex;
	bool ready;
	DARRAY(struct mtx_poller *) list;
} registry;

static void registry_init(void)
{
	if (!registry.ready) {
		pthread_mutex_init(&registry.mutex, NULL);
		da_init(registry.list);
		registry.ready = true;
	}
}

/* ---------------------------------------------------------------- helpers -- */

static void free_stream(struct mtx_stream *s)
{
	bfree(s->name);
	bfree(s->publisher);
	bfree(s->publisher_id);
	bfree(s->tracks);
	bfree(s->remote);
}

void mtx_poll_snapshot_free(struct mtx_poll_snapshot *snapshot)
{
	for (size_t i = 0; i < snapshot->count; i++)
		free_stream(&snapshot->items[i]);
	bfree(snapshot->items);
	snapshot->items = NULL;
	snapshot->count = 0;
}

static void copy_snapshot(struct mtx_poll_snapshot *to, const struct mtx_poll_snapshot *from)
{
	*to = *from;
	to->items = from->count ? bzalloc(from->count * sizeof(*to->items)) : NULL;
	for (size_t i = 0; i < from->count; i++) {
		const struct mtx_stream *f = &from->items[i];
		struct mtx_stream *t = &to->items[i];
		*t = *f;
		t->name = bstrdup(f->name);
		t->publisher = bstrdup(f->publisher);
		t->publisher_id = bstrdup(f->publisher_id);
		t->tracks = bstrdup(f->tracks);
		t->remote = bstrdup(f->remote ? f->remote : "");
	}
}

static struct counter *find_counter(struct counter *list, size_t count, const char *key)
{
	for (size_t i = 0; i < count; i++) {
		if (strcmp(list[i].key, key) == 0)
			return &list[i];
	}
	return NULL;
}

static void free_counters(struct counter *list, size_t count)
{
	for (size_t i = 0; i < count; i++)
		bfree(list[i].key);
}

static int poll_interrupt(void *opaque)
{
	struct mtx_poller *p = opaque;
	return os_atomic_load_bool(&p->stopping);
}

static const char *status_words(enum mtx_api_status status)
{
	switch (status) {
	case MTX_API_OK:
		return "connected";
	case MTX_API_UNAUTHORIZED:
		return "API login failed";
	case MTX_API_NOT_FOUND:
		return "no MediaMTX v3 API there";
	case MTX_API_BAD_RESPONSE:
	case MTX_API_REJECTED:
		return "the API sent an unexpected reply";
	default:
		return "API unreachable";
	}
}

/* ------------------------------------------------------------------- poll -- */

/* One poll: paths, then (when the server has them) SRT connections; the result replaces the snapshot. */
static void poll_once(struct mtx_poller *p, const struct ctd_server *server)
{
	char host[256];
	int port = 0;
	struct mtx_poll_snapshot next = {0};
	if (!ctd_server_api_address(server, host, sizeof(host), &port)) {
		next.status = MTX_API_NOT_FOUND; /* the profile has no API */
	} else {
		const struct mtx_api_server api = {host,           port, server->api_user, server->api_pass,
						   poll_interrupt, p,    server->api_token};
		const uint64_t start = os_gettime_ns();
		struct mtx_path_list paths = {0};
		next.status = mtx_api_list_paths(&api, &paths);
		const uint64_t now = os_gettime_ns();
		next.latency_ms = (int)((now - start) / 1000000);

		if (next.status == MTX_API_OK) {
			/* H6: the version once on connecting and then now and again; older servers have no /v3/info */
			if (!p->version_ns || now - p->version_ns > VERSION_EVERY_NS) {
				if (mtx_api_version(&api, p->version, sizeof(p->version)) != MTX_API_OK)
					p->version[0] = '\0';
				p->version_ns = now;
			}
			struct mtx_srt_conn_list conns = {0};
			if (!p->srt_missing || now > p->srt_retry_ns) {
				const enum mtx_api_status srt = mtx_api_list_srtconns(&api, &conns);
				p->srt_missing = srt == MTX_API_NOT_FOUND;
				if (p->srt_missing)
					p->srt_retry_ns = now + SRT_RETRY_NS;
			}
			const double seconds = p->last_poll_ns ? (double)(now - p->last_poll_ns) / 1e9 : 0.0;

			DARRAY(struct counter) bytes;
			DARRAY(struct counter) srt;
			da_init(bytes);
			da_init(srt);
			next.items = paths.count ? bzalloc(paths.count * sizeof(*next.items)) : NULL;
			next.count = paths.count;
			for (size_t i = 0; i < paths.count; i++) {
				struct mtx_path *path = &paths.items[i];
				struct mtx_stream *s = &next.items[i];
				s->name = bstrdup(path->name);
				s->live = path->live;
				s->publisher = bstrdup(path->publisher);
				s->publisher_id = bstrdup(path->publisher_id);
				s->tracks = bstrdup(path->tracks);
				s->readers = path->readers;
				s->remote = bstrdup("");

				/* the bitrate from the bytes counter; a smaller count means a new publisher */
				const struct counter *before = find_counter(p->bytes.array, p->bytes.num, path->name);
				if (before && seconds > 0 && path->bytes_received >= before->a)
					s->bitrate_kbps =
						(double)(path->bytes_received - before->a) * 8.0 / 1000.0 / seconds;
				struct counter c = {bstrdup(path->name), path->bytes_received, 0};
				da_push_back(bytes, &c);

				/* the SRT publisher's link */
				for (size_t k = 0; k < conns.count && *path->publisher_id; k++) {
					const struct mtx_srt_conn *conn = &conns.items[k];
					if (!conn->publishing || strcmp(conn->id, path->publisher_id) != 0)
						continue;
					s->has_link = true;
					bfree(s->remote);
					s->remote = bstrdup(conn->remote);
					s->rtt_ms = conn->rtt_ms;
					s->retransmits = conn->packets_retrans;
					s->dropped = conn->packets_dropped;
					s->receive_mbps = conn->receive_mbps;
					s->link_capacity_mbps = conn->link_capacity_mbps;
					const struct counter *was = find_counter(p->srt.array, p->srt.num, conn->id);
					if (was && conn->packets_received >= was->a && conn->packets_lost >= was->b) {
						const uint64_t got = conn->packets_received - was->a;
						const uint64_t lost = conn->packets_lost - was->b;
						s->loss_percent =
							got + lost ? 100.0 * (double)lost / (double)(got + lost) : 0.0;
					}
					struct counter sc = {bstrdup(conn->id), conn->packets_received,
							     conn->packets_lost};
					da_push_back(srt, &sc);
				}
			}
			free_counters(p->bytes.array, p->bytes.num);
			da_free(p->bytes);
			p->bytes.da = bytes.da;
			free_counters(p->srt.array, p->srt.num);
			da_free(p->srt);
			p->srt.da = srt.da;
			p->last_poll_ns = now;
			mtx_srt_conn_list_free(&conns);
		} else {
			p->version_ns = 0; /* ask again once it's back */
		}
		mtx_path_list_free(&paths);
	}
	snprintf(next.version, sizeof(next.version), "%s", next.status == MTX_API_OK ? p->version : "");
	next.updated_ns = os_gettime_ns();

	/* H7: the status in the log when it changes (or the number of streams does), never once per poll */
	size_t live = 0;
	for (size_t i = 0; i < next.count; i++)
		live += next.items[i].live;
	if (!p->logged_once || next.status != p->logged ||
	    (next.status == MTX_API_OK && (next.count != p->logged_total || live != p->logged_live))) {
		p->logged_total = next.count;
		p->logged_live = live;
		if (next.status == MTX_API_OK)
			blog(LOG_INFO, "[ctd-live] MediaMTX '%s' (%s): connected%s%s, %zu streams, %zu live",
			     server->name, server->host, *next.version ? ", " : "", next.version, next.count, live);
		else if (port)
			blog(next.status == MTX_API_UNAUTHORIZED ? LOG_WARNING : LOG_INFO,
			     "[ctd-live] MediaMTX '%s' (%s): %s", server->name, server->host,
			     status_words(next.status));
		p->logged = next.status;
		p->logged_once = true;
	}

	pthread_mutex_lock(&p->mutex);
	next.seq = p->snapshot.seq + 1;
	struct mtx_poll_snapshot old = p->snapshot;
	p->snapshot = next;
	pthread_mutex_unlock(&p->mutex);
	mtx_poll_snapshot_free(&old);
}

static void *poll_thread(void *opaque)
{
	struct mtx_poller *p = opaque;
	os_set_thread_name("ctd-mediamtx-poll");
	while (!os_atomic_load_bool(&p->stopping)) {
		struct ctd_server server;
		if (servers_get(p->id, &server)) {
			poll_once(p, &server);
			ctd_server_free(&server);
		}
		if (os_event_timedwait(p->wake, POLL_EVERY_MS) == 0 && os_atomic_load_bool(&p->stopping))
			break;
	}
	return NULL;
}

/* -------------------------------------------------------------------- API -- */

static void stop_poller(struct mtx_poller *p)
{
	os_atomic_set_bool(&p->stopping, true);
	os_event_signal(p->wake);
	if (p->thread_running)
		pthread_join(p->thread, NULL);
	os_event_destroy(p->wake);
	pthread_mutex_destroy(&p->mutex);
	mtx_poll_snapshot_free(&p->snapshot);
	free_counters(p->bytes.array, p->bytes.num);
	da_free(p->bytes);
	free_counters(p->srt.array, p->srt.num);
	da_free(p->srt);
	bfree(p);
}

struct mtx_poller *mtx_poller_acquire(const char *server_id)
{
	if (!server_id || !*server_id)
		return NULL;
	registry_init();
	pthread_mutex_lock(&registry.mutex);
	for (size_t i = 0; i < registry.list.num; i++) {
		struct mtx_poller *p = registry.list.array[i];
		if (strcmp(p->id, server_id) == 0) {
			p->refs++;
			pthread_mutex_unlock(&registry.mutex);
			return p;
		}
	}
	struct mtx_poller *p = bzalloc(sizeof(*p));
	snprintf(p->id, sizeof(p->id), "%s", server_id);
	p->refs = 1;
	pthread_mutex_init(&p->mutex, NULL);
	os_event_init(&p->wake, OS_EVENT_TYPE_AUTO);
	p->thread_running = pthread_create(&p->thread, NULL, poll_thread, p) == 0;
	da_push_back(registry.list, &p);
	pthread_mutex_unlock(&registry.mutex);
	return p;
}

void mtx_poller_release(struct mtx_poller *poller)
{
	/* after mtx_pollers_shutdown every poller is gone already (a dock destroyed late) */
	if (!poller || !registry.ready)
		return;
	pthread_mutex_lock(&registry.mutex);
	const bool last = --poller->refs == 0;
	if (last)
		da_erase_item(registry.list, &poller);
	pthread_mutex_unlock(&registry.mutex);
	if (last)
		stop_poller(poller); /* outside the registry's lock: joining may take a moment */
}

void mtx_poller_snapshot(struct mtx_poller *poller, struct mtx_poll_snapshot *out)
{
	memset(out, 0, sizeof(*out));
	if (!poller || !registry.ready)
		return;
	pthread_mutex_lock(&poller->mutex);
	copy_snapshot(out, &poller->snapshot);
	pthread_mutex_unlock(&poller->mutex);
}

void mtx_poller_refresh(struct mtx_poller *poller)
{
	if (poller)
		os_event_signal(poller->wake);
}

void mtx_pollers_shutdown(void)
{
	if (!registry.ready)
		return;
	pthread_mutex_lock(&registry.mutex);
	struct mtx_poller **left = registry.list.array;
	const size_t count = registry.list.num;
	da_init(registry.list);
	pthread_mutex_unlock(&registry.mutex);
	for (size_t i = 0; i < count; i++)
		stop_poller(left[i]);
	bfree(left);
	pthread_mutex_destroy(&registry.mutex);
	registry.ready = false;
}
