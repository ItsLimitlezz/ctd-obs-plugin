/*
 * JSON over HTTP (FFmpeg), and file transfers (libcurl). See http-client.h.
 */

#include "http-client.h"

#include <util/bmem.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include <curl/curl.h>

#include <libavformat/avio.h>
#include <libavutil/avstring.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libavutil/sha.h>

#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

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

/* ---- File download over libcurl (see http-client.h; adapted from the S2 spike, branch sr/s2-binary-http) ---- */

#define SHA256_HEX_LEN 64

struct transfer {
	FILE *file;
	struct AVSHA *sha;
	uint64_t done;
	uint64_t total; /* the server's Content-Length; 0 until it says */
	bool io_failed;
	char server_sha256[SHA256_HEX_LEN + 1]; /* from X-Content-SHA256, or empty */
	int (*interrupt)(void *);
	http_progress_cb progress;
	void *opaque;
};

static void sha_hex(struct AVSHA *sha, char out[SHA256_HEX_LEN + 1])
{
	static const char digits[] = "0123456789abcdef";
	uint8_t digest[32];
	av_sha_final(sha, digest);
	for (int i = 0; i < 32; i++) {
		out[i * 2] = digits[digest[i] >> 4];
		out[i * 2 + 1] = digits[digest[i] & 0xf];
	}
	out[SHA256_HEX_LEN] = 0;
}

static size_t on_body_in(char *data, size_t size, size_t count, void *opaque)
{
	struct transfer *t = opaque;
	const size_t len = size * count;
	if (fwrite(data, 1, len, t->file) != len) {
		t->io_failed = true;
		return 0; /* CURLE_WRITE_ERROR */
	}
	av_sha_update(t->sha, (const uint8_t *)data, len);
	t->done += len;
	return len;
}

static size_t on_header(char *line, size_t size, size_t count, void *opaque)
{
	struct transfer *t = opaque;
	const size_t len = size * count;
	static const char name[] = "x-content-sha256:";
	if (len > sizeof(name) - 1 && av_strncasecmp(line, name, sizeof(name) - 1) == 0) {
		const char *v = line + sizeof(name) - 1;
		const char *end = line + len;
		while (v < end && (*v == ' ' || *v == '\t'))
			v++;
		size_t n = 0;
		while (v + n < end && n < SHA256_HEX_LEN && av_isxdigit(v[n]))
			n++;
		if (n == SHA256_HEX_LEN) {
			for (size_t i = 0; i < n; i++)
				t->server_sha256[i] = (char)av_tolower(v[i]);
			t->server_sha256[n] = 0;
		}
	}
	return len;
}

static int on_progress(void *opaque, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
	struct transfer *t = opaque;
	(void)ultotal;
	(void)ulnow;
	if (t->interrupt && t->interrupt(t->opaque))
		return 1; /* CURLE_ABORTED_BY_CALLBACK */
	if (dltotal > 0)
		t->total = (uint64_t)dltotal;
	if (t->progress)
		t->progress(t->opaque, (uint64_t)dlnow, t->total);
	return 0;
}

static enum http_status status_from_curl(CURLcode res, long code, const struct transfer *t)
{
	switch (res) {
	case CURLE_OK:
		break;
	case CURLE_ABORTED_BY_CALLBACK:
		return HTTP_ABORTED;
	case CURLE_WRITE_ERROR:
		return t->io_failed ? HTTP_LOCAL_FILE : HTTP_UNREACHABLE;
	case CURLE_PEER_FAILED_VERIFICATION:
	case CURLE_SSL_CONNECT_ERROR:
	case CURLE_SSL_CERTPROBLEM:
	case CURLE_SSL_CACERT_BADFILE:
	case CURLE_SSL_ISSUER_ERROR:
		return HTTP_TLS_FAILED;
	case CURLE_HTTP_RETURNED_ERROR:
		break; /* the code says which */
	case CURLE_UNSUPPORTED_PROTOCOL:
	case CURLE_URL_MALFORMAT:
		return HTTP_BAD_REQUEST;
	default:
		return HTTP_UNREACHABLE; /* connect, DNS, timeout, stall, short body (CURLE_PARTIAL_FILE) */
	}
	if (code >= 200 && code < 300)
		return HTTP_OK;
	if (code >= 300 && code < 400)
		return HTTP_BAD_RESPONSE; /* a redirect, which we don't follow */
	switch (code) {
	case 400:
		return HTTP_BAD_REQUEST;
	case 401:
		return HTTP_UNAUTHORIZED;
	case 403:
		return HTTP_FORBIDDEN;
	case 404:
		return HTTP_NOT_FOUND;
	}
	if (code >= 400 && code < 500)
		return HTTP_OTHER_4XX;
	if (code >= 500)
		return HTTP_SERVER_ERROR;
	return HTTP_BAD_RESPONSE;
}

/* One transfer. t->file is open; the caller owns it. */
static enum http_status run_transfer(const char *url, const char *bearer, struct transfer *t)
{
	CURL *curl = curl_easy_init();
	if (!curl)
		return HTTP_UNREACHABLE;

	struct curl_slist *headers = NULL;
	char auth[1024];
	auth[0] = 0;
	if (bearer && *bearer) {
		snprintf(auth, sizeof(auth), "Authorization: Bearer %s", bearer);
		headers = curl_slist_append(headers, auth);
	}
	headers = curl_slist_append(headers, "Accept: application/octet-stream, application/json");

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
#if LIBCURL_VERSION_NUM >= 0x075500
	curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
#else
	curl_easy_setopt(curl, CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
	curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L); /* a 4xx/5xx body never lands in the file */
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L); /* stalled: no byte for 30 s */
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
	curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 65536L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_body_in);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, t);
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, on_header);
	curl_easy_setopt(curl, CURLOPT_HEADERDATA, t);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, on_progress);
	curl_easy_setopt(curl, CURLOPT_XFERINFODATA, t);

	const CURLcode res = curl_easy_perform(curl);
	long code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
	curl_easy_cleanup(curl);
	curl_slist_free_all(headers);
	memset(auth, 0, sizeof(auth)); /* the token never lingers in memory longer than needed */

	enum http_status status = status_from_curl(res, code, t);
	if (status == HTTP_OK && t->progress)
		t->progress(t->opaque, t->done, t->done);
	return status;
}

static int flush_to_disk(FILE *f)
{
	if (fflush(f) != 0)
		return -1;
#ifdef _WIN32
	return _commit(_fileno(f));
#else
	return fsync(fileno(f));
#endif
}

enum http_status http_get_file(const char *url, const char *bearer, const char *path, char sha256_out[65],
			       int (*interrupt)(void *), http_progress_cb progress, void *opaque)
{
	static volatile long counter;
	const long n = os_atomic_inc_long(&counter);
	if (sha256_out)
		sha256_out[0] = 0;

	struct dstr part = {0};
	dstr_printf(&part, "%s.%llx%lx.part", path, (unsigned long long)os_gettime_ns(), n);
	FILE *file = os_fopen(part.array, "wb");
	if (!file) {
		dstr_free(&part);
		return HTTP_LOCAL_FILE;
	}

	struct transfer t = {.file = file, .interrupt = interrupt, .progress = progress, .opaque = opaque};
	t.sha = av_sha_alloc();
	av_sha_init(t.sha, 256);

	enum http_status status = run_transfer(url, bearer, &t);
	char hex[SHA256_HEX_LEN + 1];
	sha_hex(t.sha, hex);
	av_freep(&t.sha);

	if (status == HTTP_OK && t.server_sha256[0] && strcmp(t.server_sha256, hex) != 0)
		status = HTTP_BAD_RESPONSE;
	if (status == HTTP_OK && flush_to_disk(file) != 0)
		status = HTTP_LOCAL_FILE;
	if (fclose(file) != 0 && status == HTTP_OK)
		status = HTTP_LOCAL_FILE;
	if (status == HTTP_OK && os_rename(part.array, path) != 0)
		status = HTTP_LOCAL_FILE;
	if (status != HTTP_OK)
		os_unlink(part.array);
	else if (sha256_out)
		memcpy(sha256_out, hex, sizeof(hex));
	dstr_free(&part);
	return status;
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
	case HTTP_ABORTED:
		return "stopped";
	case HTTP_LOCAL_FILE:
		return "couldn't read or write the file on this PC";
	default:
		return "unexpected reply";
	}
}
