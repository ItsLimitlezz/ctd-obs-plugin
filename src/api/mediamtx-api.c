/*
 * MediaMTX control API client. See mediamtx-api.h.
 */

#include "mediamtx-api.h"

#include <obs-data.h>
#include <util/bmem.h>
#include <util/darray.h>
#include <util/dstr.h>

#include <libavformat/avio.h>
#include <libavutil/base64.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_RESPONSE_BYTES (8 * 1024 * 1024)
#define MAX_PAGES 50

/* the offline picture MediaMTX serves for an always-available path: H.264 video plus AAC audio, like a camera */
#define ALWAYS_AVAILABLE_BODY                                                         \
	"{\"alwaysAvailable\":true,\"alwaysAvailableTracks\":[{\"codec\":\"H264\"},"   \
	"{\"codec\":\"MPEG4Audio\",\"sampleRate\":48000,\"channelCount\":2}]}"

static enum mtx_api_status status_from_error(int err)
{
	switch (err) {
	case AVERROR_HTTP_UNAUTHORIZED:
	case AVERROR_HTTP_FORBIDDEN:
		return MTX_API_UNAUTHORIZED;
	case AVERROR_HTTP_NOT_FOUND:
		return MTX_API_NOT_FOUND;
	case AVERROR_HTTP_BAD_REQUEST:
		return MTX_API_REJECTED;
	case AVERROR_HTTP_OTHER_4XX:
	case AVERROR_HTTP_SERVER_ERROR:
		return MTX_API_BAD_RESPONSE;
	default:
		return MTX_API_UNREACHABLE;
	}
}

/* "Authorization: Bearer ..." for a token, "Authorization: Basic ..." for user:pass, or NULL for neither.
 * Credentials never reach a URL or the log. */
static char *basic_auth_header(const char *user, const char *pass, const char *token)
{
	if (token && *token) {
		struct dstr bearer = {0};
		dstr_printf(&bearer, "Authorization: Bearer %s\r\n", token);
		return bearer.array;
	}
	if (!user || !*user)
		return NULL;

	struct dstr credentials = {0};
	dstr_printf(&credentials, "%s:%s", user, pass ? pass : "");
	size_t encoded_size = AV_BASE64_SIZE(credentials.len);
	char *encoded = bzalloc(encoded_size);
	av_base64_encode(encoded, (int)encoded_size, (const uint8_t *)credentials.array, (int)credentials.len);

	struct dstr header = {0};
	dstr_printf(&header, "Authorization: Basic %s\r\n", encoded);

	memset(credentials.array, 0, credentials.len);
	memset(encoded, 0, encoded_size);
	dstr_free(&credentials);
	bfree(encoded);
	return header.array;
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

/*
 * One request to the API. body (may be NULL) is sent as JSON. On MTX_API_OK and when response is not NULL, *response is
 * the reply as a bmalloc'd, NUL-terminated string.
 */
static enum mtx_api_status http_request(const struct mtx_api_server *server, const char *method, const char *path,
					const char *body, char **response)
{
	struct dstr url = {0};
	/* an IPv6 literal needs brackets in a URL */
	dstr_printf(&url, strchr(server->host, ':') ? "http://[%s]:%d%s" : "http://%s:%d%s", server->host, server->port,
		    path);

	AVDictionary *options = NULL;
	av_dict_set(&options, "method", method, 0);
	av_dict_set(&options, "seekable", "0", 0);
	av_dict_set(&options, "rw_timeout", "3000000", 0); /* microseconds */
	char *headers = basic_auth_header(server->user, server->pass, server->token);
	if (headers)
		av_dict_set(&options, "headers", headers, 0);
	if (body) {
		char *hex = hex_encode(body);
		av_dict_set(&options, "post_data", hex, 0);
		av_dict_set(&options, "content_type", "application/json", 0);
		bfree(hex);
	}

	AVIOInterruptCB interrupt = {server->interrupt, server->opaque};
	AVIOContext *io = NULL;
	int err = avio_open2(&io, url.array, AVIO_FLAG_READ, &interrupt, &options);
	av_dict_free(&options);
	dstr_free(&url);
	if (headers) {
		memset(headers, 0, strlen(headers));
		bfree(headers);
	}
	if (err < 0)
		return status_from_error(err);
	if (!response) {
		avio_closep(&io);
		return MTX_API_OK;
	}

	struct dstr reply = {0};
	enum mtx_api_status status = MTX_API_OK;
	unsigned char chunk[16384];
	for (;;) {
		int got = avio_read(io, chunk, sizeof(chunk));
		if (got == AVERROR_EOF || got == 0)
			break;
		if (got < 0) {
			status = MTX_API_UNREACHABLE;
			break;
		}
		if (reply.len + (size_t)got > (size_t)MAX_RESPONSE_BYTES) {
			status = MTX_API_BAD_RESPONSE;
			break;
		}
		dstr_ncat(&reply, (const char *)chunk, (size_t)got);
	}
	avio_closep(&io);

	if (status == MTX_API_OK && !reply.array)
		status = MTX_API_BAD_RESPONSE;
	if (status != MTX_API_OK) {
		dstr_free(&reply);
		return status;
	}
	*response = reply.array;
	return MTX_API_OK;
}

/*
 * Every item's "tracks" in a /v3/paths/list page, in order. MediaMTX lists them as an array of strings
 * (["H264","MPEG-4 Audio"]), which libobs's JSON reader drops, so they are read from the text itself: each path item
 * has exactly one "tracks" key and nothing nested in it has one. Returns the count found; *out holds bmalloc'd
 * strings ("H264, MPEG-4 Audio").
 */
static size_t extract_tracks(const char *json, char ***out)
{
	DARRAY(char *) found;
	da_init(found);
	for (const char *at = json; (at = strstr(at, "\"tracks\"")) != NULL;) {
		at += 8;
		while (*at == ' ' || *at == '\t' || *at == '\n' || *at == '\r' || *at == ':')
			at++;
		if (*at != '[')
			continue;
		struct dstr joined = {0};
		dstr_copy(&joined, "");
		for (at++; *at && *at != ']';) {
			if (*at != '"') {
				at++;
				continue;
			}
			const char *end = ++at;
			while (*end && *end != '"')
				end += *end == '\\' && end[1] ? 2 : 1;
			if (joined.len)
				dstr_cat(&joined, ", ");
			dstr_ncat(&joined, at, (size_t)(end - at));
			at = *end ? end + 1 : end;
		}
		char *value = joined.array;
		da_push_back(found, &value);
	}
	*out = found.array;
	return found.num;
}

static int compare_paths(const void *a, const void *b)
{
	const struct mtx_path *pa = a, *pb = b;
	if (pa->live != pb->live)
		return pa->live ? -1 : 1;
	return strcmp(pa->name, pb->name);
}

enum mtx_api_status mtx_api_list_paths(const struct mtx_api_server *server, struct mtx_path_list *out)
{
	DARRAY(struct mtx_path) paths;
	da_init(paths);

	enum mtx_api_status status = MTX_API_OK;
	int page_count = 1;
	for (int page = 0; page < page_count && page < MAX_PAGES; page++) {
		struct dstr request = {0};
		dstr_printf(&request, "/v3/paths/list?page=%d", page);
		char *body = NULL;
		status = http_request(server, "GET", request.array, NULL, &body);
		dstr_free(&request);
		if (status != MTX_API_OK)
			break;

		obs_data_t *response = obs_data_create_from_json(body);
		char **tracks = NULL;
		const size_t track_count = extract_tracks(body, &tracks);
		bfree(body);
		obs_data_array_t *items = response ? obs_data_get_array(response, "items") : NULL;
		if (!items) {
			for (size_t i = 0; i < track_count; i++)
				bfree(tracks[i]);
			bfree(tracks);
			obs_data_release(response);
			status = MTX_API_BAD_RESPONSE;
			break;
		}
		page_count = (int)obs_data_get_int(response, "pageCount");

		const size_t count = obs_data_array_count(items);
		for (size_t i = 0; i < count; i++) {
			obs_data_t *item = obs_data_array_item(items, i);
			const char *name = obs_data_get_string(item, "name");
			if (*name) {
				/* "ready" in every v3 version; "online" since 1.13. "source" is null without a publisher. */
				obs_data_t *source = obs_data_get_obj(item, "source");
				obs_data_array_t *readers = obs_data_get_array(item, "readers");
				struct mtx_path path = {
					.name = bstrdup(name),
					.live = obs_data_get_bool(item, "ready") || obs_data_get_bool(item, "online"),
					.publisher = bstrdup(source ? obs_data_get_string(source, "type") : ""),
					.publisher_id = bstrdup(source ? obs_data_get_string(source, "id") : ""),
					/* the tracks scan only lines up when every item had its key */
					.tracks = bstrdup(track_count == count ? tracks[i] : ""),
					.bytes_received = (uint64_t)obs_data_get_int(item, "bytesReceived"),
					.readers = readers ? (int)obs_data_array_count(readers) : 0,
				};
				obs_data_array_release(readers);
				obs_data_release(source);
				da_push_back(paths, &path);
			}
			obs_data_release(item);
		}
		for (size_t i = 0; i < track_count; i++)
			bfree(tracks[i]);
		bfree(tracks);
		obs_data_array_release(items);
		obs_data_release(response);
	}

	if (status != MTX_API_OK) {
		struct mtx_path_list partial = {paths.array, paths.num};
		mtx_path_list_free(&partial);
		return status;
	}

	if (paths.num > 1)
		qsort(paths.array, paths.num, sizeof(*paths.array), compare_paths);
	out->items = paths.array;
	out->count = paths.num;
	return MTX_API_OK;
}

enum mtx_api_status mtx_api_add_path(const struct mtx_api_server *server, const char *name, bool always_available)
{
	if (!mtx_path_name_valid(name))
		return MTX_API_REJECTED;
	struct dstr request = {0};
	dstr_printf(&request, "/v3/config/paths/add/%s", name);
	enum mtx_api_status status =
		http_request(server, "POST", request.array, always_available ? ALWAYS_AVAILABLE_BODY : "{}", NULL);
	dstr_free(&request);
	return status;
}

enum mtx_api_status mtx_api_delete_path(const struct mtx_api_server *server, const char *name)
{
	if (!mtx_path_name_valid(name))
		return MTX_API_REJECTED;
	struct dstr request = {0};
	dstr_printf(&request, "/v3/config/paths/delete/%s", name);
	enum mtx_api_status status = http_request(server, "DELETE", request.array, NULL, NULL);
	dstr_free(&request);
	return status;
}

bool mtx_path_name_valid(const char *name)
{
	const size_t len = name ? strlen(name) : 0;
	if (!len || name[0] == '/' || name[len - 1] == '/' || strstr(name, "//"))
		return false;
	for (size_t i = 0; i < len; i++) {
		const unsigned char c = (unsigned char)name[i];
		if (!isalnum(c) && !strchr("_-.~/", c))
			return false;
	}
	return true;
}

void mtx_path_list_free(struct mtx_path_list *list)
{
	for (size_t i = 0; i < list->count; i++) {
		bfree(list->items[i].name);
		bfree(list->items[i].publisher);
		bfree(list->items[i].publisher_id);
		bfree(list->items[i].tracks);
	}
	bfree(list->items);
	list->items = NULL;
	list->count = 0;
}

enum mtx_api_status mtx_api_list_srtconns(const struct mtx_api_server *server, struct mtx_srt_conn_list *out)
{
	DARRAY(struct mtx_srt_conn) conns;
	da_init(conns);
	enum mtx_api_status status = MTX_API_OK;
	int page_count = 1;
	for (int page = 0; page < page_count && page < MAX_PAGES; page++) {
		struct dstr request = {0};
		dstr_printf(&request, "/v3/srtconns/list?page=%d", page);
		char *body = NULL;
		status = http_request(server, "GET", request.array, NULL, &body);
		dstr_free(&request);
		if (status != MTX_API_OK)
			break;
		obs_data_t *response = obs_data_create_from_json(body);
		bfree(body);
		obs_data_array_t *items = response ? obs_data_get_array(response, "items") : NULL;
		if (!items) {
			obs_data_release(response);
			status = MTX_API_BAD_RESPONSE;
			break;
		}
		page_count = (int)obs_data_get_int(response, "pageCount");
		for (size_t i = 0; i < obs_data_array_count(items); i++) {
			obs_data_t *item = obs_data_array_item(items, i);
			struct mtx_srt_conn conn = {
				.id = bstrdup(obs_data_get_string(item, "id")),
				.path = bstrdup(obs_data_get_string(item, "path")),
				.remote = bstrdup(obs_data_get_string(item, "remoteAddr")),
				.publishing = strcmp(obs_data_get_string(item, "state"), "publish") == 0,
				.rtt_ms = obs_data_get_double(item, "msRTT"),
				.packets_received = (uint64_t)obs_data_get_int(item, "packetsReceived"),
				.packets_lost = (uint64_t)obs_data_get_int(item, "packetsReceivedLoss"),
				.packets_retrans = (uint64_t)obs_data_get_int(item, "packetsReceivedRetrans"),
				.packets_dropped = (uint64_t)obs_data_get_int(item, "packetsReceivedDrop"),
				.receive_mbps = obs_data_get_double(item, "mbpsReceiveRate"),
				.link_capacity_mbps = obs_data_get_double(item, "mbpsLinkCapacity"),
			};
			da_push_back(conns, &conn);
			obs_data_release(item);
		}
		obs_data_array_release(items);
		obs_data_release(response);
	}
	out->items = conns.array;
	out->count = conns.num;
	if (status != MTX_API_OK)
		mtx_srt_conn_list_free(out);
	return status;
}

void mtx_srt_conn_list_free(struct mtx_srt_conn_list *list)
{
	for (size_t i = 0; i < list->count; i++) {
		bfree(list->items[i].id);
		bfree(list->items[i].path);
		bfree(list->items[i].remote);
	}
	bfree(list->items);
	list->items = NULL;
	list->count = 0;
}

enum mtx_api_status mtx_api_kick(const struct mtx_api_server *server, const char *publisher_type, const char *id)
{
	/* the list each connection type lives in: POST /v3/<list>/kick/<id> */
	static const struct {
		const char *type;
		const char *list;
	} lists[] = {{"srtConn", "srtconns"},           {"rtmpConn", "rtmpconns"},
		     {"rtmpsConn", "rtmpsconns"},       {"rtspSession", "rtspsessions"},
		     {"rtspsSession", "rtspssessions"}, {"webRTCSession", "webrtcsessions"}};
	const char *list = NULL;
	for (size_t i = 0; i < sizeof(lists) / sizeof(*lists) && publisher_type; i++) {
		if (strcmp(lists[i].type, publisher_type) == 0)
			list = lists[i].list;
	}
	if (!list || !id || !*id || strspn(id, "0123456789abcdefABCDEF-") != strlen(id))
		return MTX_API_REJECTED;
	struct dstr request = {0};
	dstr_printf(&request, "/v3/%s/kick/%s", list, id);
	const enum mtx_api_status status = http_request(server, "POST", request.array, "{}", NULL);
	dstr_free(&request);
	return status;
}

enum mtx_api_status mtx_api_version(const struct mtx_api_server *server, char *out, size_t size)
{
	if (size)
		out[0] = '\0';
	char *body = NULL;
	const enum mtx_api_status status = http_request(server, "GET", "/v3/info", NULL, &body);
	if (status != MTX_API_OK)
		return status;
	obs_data_t *info = obs_data_create_from_json(body);
	bfree(body);
	if (!info)
		return MTX_API_BAD_RESPONSE;
	snprintf(out, size, "%s", obs_data_get_string(info, "version"));
	obs_data_release(info);
	return MTX_API_OK;
}
