#include "devices-identity.h"

#include <stddef.h>
#include <string.h>

typedef enum { PARSE_NONE, PARSE_DSHOW, PARSE_AVCAPTURE } parse_kind;

typedef struct {
	const char *type;
	const char *cls;
	const char *id_key;
	const char *id_key_alt; /* fallback key, may be NULL */
	const char *name_key;   /* may be NULL */
	parse_kind parse;
} row_t;

/* Design section 5. Keys are the OBS settings keys of each source type; check them on the PC/Mac before relying on
 * a rarely used one (DeckLink, display names). */
static const row_t rows[] = {
	/* video */
	{"dshow_input", "video", "video_device_id", NULL, NULL, PARSE_DSHOW},
	{"av_capture_input", "video", "device", NULL, "device_name", PARSE_AVCAPTURE},
	{"av_capture_input_v2", "video", "device", NULL, "device_name", PARSE_AVCAPTURE},
	{"macos-avcapture", "video", "device", NULL, "device_name", PARSE_AVCAPTURE},
	{"decklink-input", "video", "device_hash", NULL, "device_name", PARSE_NONE},
	{"v4l2_input", "video", "device_id", NULL, NULL, PARSE_NONE},
	/* audio (the global DesktopAudioDevice1 / AuxAudioDevice1... are sources of these same types) */
	{"wasapi_input_capture", "audio", "device_id", NULL, "device_name", PARSE_NONE},
	{"wasapi_output_capture", "audio", "device_id", NULL, "device_name", PARSE_NONE},
	{"coreaudio_input_capture", "audio", "device_id", NULL, "device_name", PARSE_NONE},
	{"coreaudio_output_capture", "audio", "device_id", NULL, "device_name", PARSE_NONE},
	/* display */
	{"monitor_capture", "display", "monitor_id", "monitor", "monitor_name", PARSE_NONE},
	{"screen_capture", "display", "display_uuid", "display", "display_name", PARSE_NONE},
	/* window / game: the window string (exe + class), kept as is */
	{"window_capture", "window", "window", NULL, NULL, PARSE_NONE},
	{"game_capture", "window", "window", NULL, NULL, PARSE_NONE},
};

#define ROW_COUNT (sizeof(rows) / sizeof(rows[0]))

static const row_t *find_row(const char *type)
{
	if (!type)
		return NULL;
	for (size_t i = 0; i < ROW_COUNT; i++)
		if (strcmp(rows[i].type, type) == 0)
			return &rows[i];
	return NULL;
}

bool devices_identity_keys(const char *source_type, const char **id_key, const char **name_key)
{
	const row_t *row = find_row(source_type);
	if (id_key)
		*id_key = row ? row->id_key : NULL;
	if (name_key)
		*name_key = row ? row->name_key : NULL;
	return row != NULL;
}

static void copy_str(char *dst, size_t cap, const char *src, size_t len)
{
	if (len >= cap)
		len = cap - 1;
	memcpy(dst, src, len);
	dst[len] = '\0';
}

static void set_str(char *dst, size_t cap, const char *src)
{
	copy_str(dst, cap, src ? src : "", src ? strlen(src) : 0);
}

static char lower(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool is_hex(char c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static bool all_hex(const char *s, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (!is_hex(s[i]))
			return false;
	return true;
}

static void set_vid_pid(devices_identity_t *out, const char *vid, const char *pid)
{
	for (int i = 0; i < 4; i++) {
		out->vid_pid[i] = lower(vid[i]);
		out->vid_pid[5 + i] = lower(pid[i]);
	}
	out->vid_pid[4] = ':';
	out->vid_pid[9] = '\0';
}

/* Case-insensitive search for an ASCII needle. */
static const char *find_ci(const char *hay, const char *needle)
{
	size_t n = strlen(needle);
	for (; *hay; hay++) {
		size_t i = 0;
		while (i < n && hay[i] && lower(hay[i]) == needle[i])
			i++;
		if (i == n)
			return hay;
	}
	return NULL;
}

bool devices_identity_parse_dshow(const char *device_id, devices_identity_t *out)
{
	if (!device_id || !out)
		return false;

	/* Name: everything before the first ':' (the name has its own ':' and '#' escaped). */
	const char *colon = strchr(device_id, ':');
	if (colon) {
		char name[sizeof(out->name)];
		size_t w = 0;
		for (const char *p = device_id; p < colon && w + 1 < sizeof(name);) {
			if (p[0] == '#' && p + 2 < colon + 1 && p[1] == '3' && p[2] == 'A') {
				name[w++] = ':';
				p += 3;
			} else if (p[0] == '#' && p + 2 < colon + 1 && p[1] == '2' && p[2] == '2') {
				name[w++] = '#';
				p += 3;
			} else {
				name[w++] = *p++;
			}
		}
		name[w] = '\0';
		set_str(out->name, sizeof(out->name), name);
	}

	const char *path = colon ? colon + 1 : device_id;
	const char *v = find_ci(path, "vid_");
	if (!v)
		return false;
	const char *p = find_ci(v, "&pid_");
	if (!p || !all_hex(v + 4, 4) || !all_hex(p + 5, 4))
		return false;
	set_vid_pid(out, v + 4, p + 5);

	/* Instance: the segment after the "vid_..&pid_.." segment, up to the next '#'. */
	const char *seg_end = strchr(p, '#');
	if (seg_end) {
		const char *inst = seg_end + 1;
		const char *inst_end = strchr(inst, '#');
		size_t len = inst_end ? (size_t)(inst_end - inst) : strlen(inst);
		if (len >= sizeof(out->serial))
			len = sizeof(out->serial) - 1;
		for (size_t i = 0; i < len; i++)
			out->serial[i] = lower(inst[i]);
		out->serial[len] = '\0';
	}
	return true;
}

bool devices_identity_parse_avcapture(const char *unique_id, devices_identity_t *out)
{
	if (!unique_id || !out)
		return false;
	if (unique_id[0] != '0' || (unique_id[1] != 'x' && unique_id[1] != 'X'))
		return false;
	const char *hex = unique_id + 2;
	size_t n = strlen(hex);
	/* location id (at least 1 digit) + vid + pid, and nothing but hex digits */
	if (n < 9 || n > 16 || !all_hex(hex, n))
		return false;
	set_vid_pid(out, hex + n - 8, hex + n - 4);
	return true;
}

const char *devices_identity_class(const char *source_type)
{
	const row_t *r = find_row(source_type);
	return r ? r->cls : NULL;
}

static const char *get(devices_identity_get_fn fn, void *ctx, const char *key)
{
	const char *v = (fn && key) ? fn(ctx, key) : NULL;
	return v ? v : "";
}

bool devices_identity(const char *source_type, devices_identity_get_fn get_fn, void *ctx, devices_identity_t *out)
{
	if (!out)
		return false;
	memset(out, 0, sizeof(*out));

	const row_t *r = find_row(source_type);
	if (!r)
		return false;

	set_str(out->cls, sizeof(out->cls), r->cls);

	const char *id = get(get_fn, ctx, r->id_key);
	if (!*id)
		id = get(get_fn, ctx, r->id_key_alt);
	set_str(out->id, sizeof(out->id), id);

	if (r->parse == PARSE_DSHOW)
		devices_identity_parse_dshow(id, out);
	else if (r->parse == PARSE_AVCAPTURE)
		devices_identity_parse_avcapture(id, out);

	/* An explicit name setting wins over a name parsed from the id. */
	const char *name = get(get_fn, ctx, r->name_key);
	if (*name)
		set_str(out->name, sizeof(out->name), name);

	return true;
}
