/*
 * The MediaMTX poller (SPEC 5.2, H5 to H7): one per server profile, shared by every source and the dock that use it,
 * on its own thread. Every 2 s it reads the server's paths and SRT connections, and keeps a snapshot: each stream's
 * publisher, tracks, bitrate and readers, the SRT publisher's link (RTT, loss, retransmits, receive rate, capacity),
 * and the server's status, version and API latency. The API being down, slow or refusing only changes the status; it
 * never touches playback (H7).
 */

#pragma once

#include "mediamtx-api.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mtx_stream {
	char *name;
	bool live;
	char *publisher; /* "srtConn", "rtmpConn", ... or "" */
	char *publisher_id;
	char *tracks; /* "H264, MPEG-4 Audio" */
	int readers;
	double bitrate_kbps; /* what the server received over the last poll */
	/* the SRT publisher's link, when has_link */
	bool has_link;
	char *remote;
	double rtt_ms;
	double loss_percent; /* packets lost over the last poll, of those sent */
	uint64_t retransmits;
	uint64_t dropped;
	double receive_mbps;
	double link_capacity_mbps;
};

struct mtx_poll_snapshot {
	enum mtx_api_status status; /* MTX_API_NOT_FOUND also when the profile has no API */
	char version[32];           /* "v1.21.1", or "" when the server doesn't say */
	int latency_ms;             /* the last paths request */
	uint64_t updated_ns;        /* os_gettime_ns of the last poll */
	struct mtx_stream *items;   /* live first, then by name */
	size_t count;
	uint64_t seq; /* goes up with every poll */
};

struct mtx_poller;

/* The poller for a server profile, started if it isn't running; release it when done. NULL for an empty id. */
struct mtx_poller *mtx_poller_acquire(const char *server_id);
void mtx_poller_release(struct mtx_poller *poller);

/* A copy of the latest poll; free it with mtx_poll_snapshot_free. */
void mtx_poller_snapshot(struct mtx_poller *poller, struct mtx_poll_snapshot *out);
void mtx_poll_snapshot_free(struct mtx_poll_snapshot *snapshot);
/* Polls again now instead of at the next tick. */
void mtx_poller_refresh(struct mtx_poller *poller);

/* At module unload: stops any poller still running. */
void mtx_pollers_shutdown(void);

#ifdef __cplusplus
}
#endif
