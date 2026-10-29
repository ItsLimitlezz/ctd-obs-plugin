/*
 * A small JSON-over-HTTP client for the CTD Relay API. HTTP goes through FFmpeg's http protocol (https too, with
 * OBS's FFmpeg) and JSON through libobs, both already linked. Calls block: run them off OBS's UI and graphics threads.
 *
 * https:// servers must have a certificate the system trusts for the name or address in the URL (FFmpeg before 9 checks
 * nothing unless told to). Redirects aren't followed unless a call asks: FFmpeg would send the token and the body
 * (which may hold a password) to wherever the server points.
 */

#pragma once

#include <obs-data.h>

#include <stdbool.h>

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

const char *http_status_text(enum http_status status);
