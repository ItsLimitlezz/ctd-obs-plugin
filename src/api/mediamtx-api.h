/*
 * MediaMTX control API client (v3): list the server's paths, and add or delete them.
 *
 * HTTP goes through FFmpeg's http protocol and JSON through libobs, both already linked, so this adds no
 * dependencies. Calls block: run them off OBS's UI and graphics threads.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mtx_api_server {
	const char *host;
	int port;
	const char *user; /* may be NULL or empty */
	const char *pass;
	int (*interrupt)(void *); /* may be NULL; aborts a request as soon as it returns non-zero */
	void *opaque;
	const char *token; /* a bearer token (MediaMTX JWT auth) instead of user/pass; may be NULL */
};

struct mtx_path {
	char *name;
	bool live;               /* a publisher is sending to it right now */
	char *publisher;         /* its type ("srtConn", "rtmpConn", "rtspSession", "webRTCSession", ...), or "" */
	char *publisher_id;      /* the connection's id, for kicking it and finding its link stats; or "" */
	char *tracks;            /* "H264, MPEG-4 Audio", or "" */
	uint64_t bytes_received; /* since the publisher started: the bitrate is its change over time */
	int readers;
};

/* One SRT connection (GET /v3/srtconns/list): a publisher's link quality, or one of the readers (this plugin's own). */
struct mtx_srt_conn {
	char *id;
	char *path;
	char *remote; /* "ip:port" */
	bool publishing;
	double rtt_ms;
	uint64_t packets_received;
	uint64_t packets_lost;    /* packetsReceivedLoss */
	uint64_t packets_retrans; /* packetsReceivedRetrans */
	uint64_t packets_dropped; /* packetsReceivedDrop: too late to play */
	double receive_mbps;
	double link_capacity_mbps;
};

struct mtx_srt_conn_list {
	struct mtx_srt_conn *items;
	size_t count;
};

struct mtx_path_list {
	struct mtx_path *items; /* live paths first, then by name */
	size_t count;
};

enum mtx_api_status {
	MTX_API_UNKNOWN, /* not asked yet */
	MTX_API_OK,
	MTX_API_UNREACHABLE,
	MTX_API_UNAUTHORIZED,
	MTX_API_NOT_FOUND, /* no API at that address, or no such path */
	MTX_API_REJECTED,  /* invalid name, path already exists, or a setting the server doesn't know */
	MTX_API_BAD_RESPONSE,
};

/* Lists every path on the server. On MTX_API_OK, out holds the list; free it with mtx_path_list_free. */
enum mtx_api_status mtx_api_list_paths(const struct mtx_api_server *server, struct mtx_path_list *out);

/*
 * Adds a path to the server's running configuration. always_available makes MediaMTX serve an offline picture while
 * nothing is publishing. MediaMTX does not write API changes to mediamtx.yml: the path lasts until it restarts.
 */
enum mtx_api_status mtx_api_add_path(const struct mtx_api_server *server, const char *name, bool always_available);

/* Removes a path from the server's running configuration (until it restarts, for paths in mediamtx.yml). */
enum mtx_api_status mtx_api_delete_path(const struct mtx_api_server *server, const char *name);

/* Every SRT connection (all pages). MTX_API_NOT_FOUND when the server has no SRT, or a version without the list. */
enum mtx_api_status mtx_api_list_srtconns(const struct mtx_api_server *server, struct mtx_srt_conn_list *out);
void mtx_srt_conn_list_free(struct mtx_srt_conn_list *list);

/* H8: disconnects a publisher (its type and id from struct mtx_path), so a stuck camera reconnects.
 * MTX_API_REJECTED for a type that can't be kicked. */
enum mtx_api_status mtx_api_kick(const struct mtx_api_server *server, const char *publisher_type, const char *id);

/* The server's version ("v1.21.1") from GET /v3/info, into out. MTX_API_NOT_FOUND on versions without it. */
enum mtx_api_status mtx_api_version(const struct mtx_api_server *server, char *out, size_t size);

/* Letters, digits and _ - . ~ /, with no leading, trailing or doubled slash. */
bool mtx_path_name_valid(const char *name);

void mtx_path_list_free(struct mtx_path_list *list);

#ifdef __cplusplus
}
#endif
