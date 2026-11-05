/*
 * A small JSON-over-HTTP client for the CTD Relay API. HTTP goes through FFmpeg's http protocol (https too, with
 * OBS's FFmpeg) and JSON through libobs, both already linked. Calls block: run them off OBS's UI and graphics threads.
 * File transfers (http_get_file, http_put_file) go through libcurl instead: see docs/show-profiles-s2.md.
 *
 * https:// servers must have a certificate the system trusts for the name or address in the URL (FFmpeg before 9 checks
 * nothing unless told to). Redirects aren't followed unless a call asks: FFmpeg would send the token and the body
 * (which may hold a password) to wherever the server points.
 */

#pragma once

#include <obs-data.h>

#include <stdbool.h>
#include <stdint.h>

enum http_status {
	HTTP_OK,           /* 2xx */
	HTTP_UNREACHABLE,  /* no answer, timeout, or aborted */
	HTTP_BAD_REQUEST,  /* 400 */
	HTTP_UNAUTHORIZED, /* 401: no token, or a revoked one */
	HTTP_FORBIDDEN,    /* 403: the token lacks a scope, or a pairing was denied */
	HTTP_NOT_FOUND,    /* 404 */
	HTTP_OTHER_4XX,    /* 409, 410 (pairing expired), ... (not 429: FFmpeg has its own code) */
	HTTP_SERVER_ERROR, /* 5xx */
	HTTP_BAD_RESPONSE, /* not JSON, or too big */
	HTTP_TLS_FAILED,   /* https refused (certificate, name, revocation, not https): Windows; else unreachable */
	HTTP_ABORTED,      /* file transfers only: the interrupt callback asked to stop */
	HTTP_LOCAL_FILE,   /* file transfers only: the local file couldn't be read, written or renamed */
};

struct http_call {
	const char *method;       /* "GET", "POST", "PUT", "DELETE" */
	const char *url;          /* full URL */
	const char *bearer;       /* token for "Authorization: Bearer", or NULL */
	const char *body;         /* JSON to send, or NULL */
	int (*interrupt)(void *); /* may be NULL; aborts the call as soon as it returns non-zero */
	void *opaque;
	bool follow_redirects; /* only for calls that carry no secret (webhooks) */
};

/* One call. On HTTP_OK and when json is not NULL, *json is the reply parsed (release it), or NULL for an empty reply. */
enum http_status http_call_json(const struct http_call *call, obs_data_t **json);

/*
 * Opens a long-lived GET for Server-Sent Events. last_event_id (may be NULL) resumes the stream; idle_timeout_s is how
 * long a silent stream may last before a read fails (the server pings every 15 s). Read the returned AVIOContext with
 * avio_read_partial and close it with avio_closep.
 */
struct AVIOContext;
enum http_status http_events_open(const struct http_call *call, const char *last_event_id, int idle_timeout_s,
				  struct AVIOContext **io);

/*
 * Streaming file transfers (Show Profiles, up to 2 GB and beyond: sizes are 64-bit). Nothing is held in memory but a
 * 16-64 KB buffer. They block; run them on a worker thread.
 *
 * - sha256_out (may be NULL) receives the lowercase hex SHA-256 of the bytes transferred, computed while streaming.
 * - interrupt (may be NULL) is polled at least once a second and on every buffer; non-zero aborts with HTTP_ABORTED.
 * - progress (may be NULL) gets (opaque, bytes done, bytes total); total is 0 while unknown. Called on the calling thread.
 * - If the server sends "X-Content-SHA256: <hex>" with a GET or with the reply to a PUT, it must match, or the call
 *   fails with HTTP_BAD_RESPONSE (and a GET leaves no file).
 * - Redirects are never followed (the token would go with them); http and https only; https checks the certificate.
 * - Needs curl_global_init() once per process before the first call (the plugin's obs_module_load).
 *
 * http_get_file writes to "<path>.<random>.part" next to path, flushes it to disk, then renames it over path. On any
 * failure or abort the .part is deleted and path is untouched. (A crash mid-transfer can leave a .part: callers that
 * own a directory should sweep "*.part" there on start.)
 *
 * http_put_file sends path with a Content-Length (no chunked encoding) and "Expect: 100-continue", so a server can
 * refuse (401, 403, 413) before the body is sent. HTTP_OK means the server answered 2xx after the whole body.
 */
typedef void (*http_progress_cb)(void *opaque, uint64_t done, uint64_t total);

enum http_status http_get_file(const char *url, const char *bearer, const char *path, char sha256_out[65],
			       int (*interrupt)(void *), http_progress_cb progress, void *opaque);
enum http_status http_put_file(const char *url, const char *bearer, const char *path, char sha256_out[65],
			       int (*interrupt)(void *), http_progress_cb progress, void *opaque);

const char *http_status_text(enum http_status status);
