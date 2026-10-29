/*
 * JSON over HTTP. See http-client.h.
 */

#include "http-client.h"

#include <util/bmem.h>
#include <util/dstr.h>

#include <libavformat/avio.h>
#include <libavutil/avstring.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>

#include <stdio.h>
#include <string.h>

#define MAX_RESPONSE_BYTES (4 * 1024 * 1024)

static enum http_status status_from_error(int err, const char *url)
{
	switch (err) {
	case AVERROR_UNKNOWN: /* Schannel (Windows) refusing the handshake, for any reason: tcp and http never return it */
#ifdef __APPLE__
	case AVERROR(EIO): /* Secure Transport (macOS) refusing the handshake or the certificate */
#endif
		return url && av_strncasecmp(url, "https://", 8) == 0 ? HTTP_TLS_FAILED : HTTP_UNREACHABLE;
	case AVERROR_HTTP_BAD_REQUEST:
		return HTTP_BAD_REQUEST;
	case AVERROR_HTTP_UNAUTHORIZED:
		return HTTP_UNAUTHORIZED;
	case AVERROR_HTTP_FORBIDDEN:
		return HTTP_FORBIDDEN;
	case AVERROR_HTTP_NOT_FOUND:
		return HTTP_NOT_FOUND;
	case AVERROR_HTTP_OTHER_4XX:
		return HTTP_OTHER_4XX;
	case AVERROR_HTTP_SERVER_ERROR:
		return HTTP_SERVER_ERROR;
	default:
		return HTTP_UNREACHABLE;
	}
}

/* https: check the certificate against the system's trusted roots and the URL's host (FFmpeg 7 and 8 default to not
 * checking at all; only the tls protocol reads it, so it is harmless for http://). No redirects unless asked. */
static void set_safety_options(AVDictionary **options, const struct http_call *call)
{
	av_dict_set(options, "tls_verify", "1", 0);
	if (!call->follow_redirects)
		av_dict_set(options, "max_redirects", "0", 0);
}

/* FFmpeg's post_data option is binary, which it takes as hex */
static char *hex_encode(const char *text)
{
	static const char digits[] = "0123456789abcdef";
	const size_t len = strlen(text);
	char *hex = bmalloc(len * 2 + 1);
	for (size_t i = 0; i < len; i++) {
		hex[i * 2] = digits[(unsigned char)text[i] >> 4];
		hex[i * 2 + 1] = digits[(unsigned char)text[i] & 0xf];
	}
	hex[len * 2] = 0;
	return hex;
}

enum http_status http_call_json(const struct http_call *call, obs_data_t **json)
{
	if (json)
		*json = NULL;

	AVDictionary *options = NULL;
	av_dict_set(&options, "method", call->method, 0);
	av_dict_set(&options, "seekable", "0", 0);
	av_dict_set(&options, "rw_timeout", "4000000", 0); /* microseconds */
	set_safety_options(&options, call);
	struct dstr headers = {0};
	dstr_copy(&headers, "Accept: application/json\r\n");
	if (call->bearer && *call->bearer)
		dstr_catf(&headers, "Authorization: Bearer %s\r\n", call->bearer);
	av_dict_set(&options, "headers", headers.array, 0);
	if (call->body) {
		char *hex = hex_encode(call->body);
		av_dict_set(&options, "post_data", hex, 0);
		av_dict_set(&options, "content_type", "application/json", 0);
		memset(hex, 0, strlen(hex)); /* the body may hold a password (FFmpeg's own copies are beyond reach) */
		bfree(hex);
	}

	AVIOInterruptCB interrupt = {call->interrupt, call->opaque};
	AVIOContext *io = NULL;
	const int err = avio_open2(&io, call->url, AVIO_FLAG_READ, &interrupt, &options);
	av_dict_free(&options);
	memset(headers.array, 0, headers.len); /* the token never lingers in memory longer than needed */
	dstr_free(&headers);
	if (err < 0)
		return status_from_error(err, call->url);

	struct dstr reply = {0};
	enum http_status status = HTTP_OK;
	unsigned char chunk[16384];
	for (;;) {
		const int got = avio_read(io, chunk, sizeof(chunk));
		if (got == AVERROR_EOF || got == 0)
			break;
		if (got < 0) {
			status = HTTP_UNREACHABLE;
			break;
		}
		if (reply.len + (size_t)got > (size_t)MAX_RESPONSE_BYTES) {
			status = HTTP_BAD_RESPONSE;
			break;
		}
		dstr_ncat(&reply, (const char *)chunk, (size_t)got);
	}
	avio_closep(&io);

	if (status == HTTP_OK && json && reply.array && reply.len) {
		*json = obs_data_create_from_json(reply.array);
		if (!*json)
			status = HTTP_BAD_RESPONSE;
	}
	dstr_free(&reply);
	return status;
}

enum http_status http_events_open(const struct http_call *call, const char *last_event_id, int idle_timeout_s,
				  struct AVIOContext **io)
{
	*io = NULL;
	AVDictionary *options = NULL;
	av_dict_set(&options, "method", "GET", 0);
	av_dict_set(&options, "seekable", "0", 0);
	char timeout[32];
	snprintf(timeout, sizeof(timeout), "%lld", (long long)idle_timeout_s * 1000000);
	av_dict_set(&options, "rw_timeout", timeout, 0);
	set_safety_options(&options, call);
	struct dstr headers = {0};
	dstr_copy(&headers, "Accept: text/event-stream\r\nCache-Control: no-cache\r\n");
	if (call->bearer && *call->bearer)
		dstr_catf(&headers, "Authorization: Bearer %s\r\n", call->bearer);
	if (last_event_id && *last_event_id)
		dstr_catf(&headers, "Last-Event-ID: %s\r\n", last_event_id);
	av_dict_set(&options, "headers", headers.array, 0);

	AVIOInterruptCB interrupt = {call->interrupt, call->opaque};
	const int err = avio_open2(io, call->url, AVIO_FLAG_READ, &interrupt, &options);
	av_dict_free(&options);
	memset(headers.array, 0, headers.len);
	dstr_free(&headers);
	return err < 0 ? status_from_error(err, call->url) : HTTP_OK;
}

const char *http_status_text(enum http_status status)
{
	switch (status) {
	case HTTP_OK:
		return "OK";
	case HTTP_UNREACHABLE:
		return "unreachable";
	case HTTP_BAD_REQUEST:
		return "bad request";
	case HTTP_UNAUTHORIZED:
		return "not signed in (the token was refused)";
	case HTTP_FORBIDDEN:
		return "not allowed";
	case HTTP_NOT_FOUND:
		return "not found";
	case HTTP_OTHER_4XX:
		return "refused";
	case HTTP_SERVER_ERROR:
		return "server error";
	case HTTP_TLS_FAILED:
		return "couldn't open a secure connection (is it https, with a certificate this PC trusts for this address?)";
	default:
		return "unexpected reply";
	}
}
