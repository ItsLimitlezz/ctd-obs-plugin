/* Asset scanner/rewriter for Show Profiles; see assets.h. */
#include "assets.h"

#include "manifest.h"
#include "sha256.h"
#include "show-fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- kind ---- */

static bool ext_in(const char *ext, const char *list)
{
	/* list: space-separated lowercase extensions */
	size_t n = strlen(ext);
	const char *p = list;
	while (*p) {
		const char *end = strchr(p, ' ');
		size_t len = end ? (size_t)(end - p) : strlen(p);
		if (len == n && strncmp(p, ext, n) == 0)
			return true;
		if (!end)
			break;
		p = end + 1;
	}
	return false;
}

static bool type_is(const char *type, const char *base)
{
	size_t n = strlen(base);
	return type && strncmp(type, base, n) == 0 && (type[n] == '\0' || type[n] == '_');
}

const char *show_asset_kind(const char *source_type, const char *path)
{
	char ext[16];
	size_t n = 0;
	const char *dot = NULL;
	if (type_is(source_type, "clut_filter"))
		return "lut";
	if (!path)
		return "other";
	for (const char *p = path; *p; p++) {
		if (*p == '.')
			dot = p;
		else if (*p == '/' || *p == '\\')
			dot = NULL;
	}
	if (!dot)
		return "other";
	for (dot++; *dot && n + 1 < sizeof(ext); dot++, n++)
		ext[n] = (*dot >= 'A' && *dot <= 'Z') ? (char)(*dot + 32) : *dot;
	ext[n] = '\0';
	if (*dot) /* longer than any extension we know */
		return "other";
	if (ext_in(ext, "cube"))
		return "lut";
	if (ext_in(ext, "png jpg jpeg gif bmp tga webp psd"))
		return "image";
	if (ext_in(ext, "mp4 mov mkv webm avi flv ts m4v mxf"))
		return "video";
	if (ext_in(ext, "mp3 wav flac ogg m4a aac opus"))
		return "audio";
	if (ext_in(ext, "ttf otf"))
		return "font";
	if (ext_in(ext, "html htm"))
		return "html";
	return "other";
}

/* ---- known path keys ---- */

struct known {
	const char *base;
	const char *guard;    /* bool setting that says the path is in use (NULL: always) */
	const char *keys[3];  /* string settings holding a path */
	const char *lists[2]; /* array settings whose elements are {"value": path} (or plain strings) */
};

static const struct known known_types[] = {
	{"image_source", NULL, {"file"}, {NULL}},
	{"ffmpeg_source", "is_local_file", {"local_file"}, {NULL}},
	{"vlc_source", NULL, {NULL}, {"playlist"}},
	{"slideshow", NULL, {NULL}, {"files"}},
	{"browser_source", "is_local_file", {"local_file"}, {NULL}},
	{"clut_filter", NULL, {"image_path"}, {NULL}},
	{"text_gdiplus", "read_from_file", {"file"}, {NULL}},
	{"text_ft2_source", "from_file", {"text_file", "file"}, {NULL}},
	{"obs_stinger_transition", NULL, {"path", "track_matte_path"}, {NULL}},
};

static const struct known *find_known(const char *type)
{
	for (size_t i = 0; type && i < sizeof(known_types) / sizeof(known_types[0]); i++)
		if (type_is(type, known_types[i].base))
			return &known_types[i];
	return NULL;
}

static bool all_digits(const char *s, size_t n)
{
	if (n == 0)
		return false;
	for (size_t i = 0; i < n; i++)
		if (s[i] < '0' || s[i] > '9')
			return false;
	return true;
}

/* Is the pointer (from the owning source/filter object) one of the type's known path keys? */
static bool pointer_is_known(const struct known *k, const char *ptr)
{
	static const char prefix[] = "/settings/";
	if (!k || strncmp(ptr, prefix, sizeof(prefix) - 1) != 0)
		return false;
	const char *rest = ptr + sizeof(prefix) - 1;
	for (int i = 0; i < 3 && k->keys[i]; i++)
		if (strcmp(rest, k->keys[i]) == 0)
			return true;
	for (int i = 0; i < 2 && k->lists[i]; i++) {
		size_t n = strlen(k->lists[i]);
		if (strncmp(rest, k->lists[i], n) != 0 || rest[n] != '/')
			continue;
		const char *idx = rest + n + 1;
		const char *slash = strchr(idx, '/');
		if (!slash)
			return all_digits(idx, strlen(idx));
		if (all_digits(idx, (size_t)(slash - idx)) && strcmp(slash, "/value") == 0)
			return true;
	}
	return false;
}

static bool is_url(const char *s)
{
	const char *c = strstr(s, "://");
	if (!c || c - s < 2)
		return false;
	for (const char *p = s; p < c; p++)
		if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '+' ||
		      *p == '-' || *p == '.'))
			return false;
	return true;
}

/* POSIX "/x", Windows "C:\x" or "C:/x", UNC "\\host\x". */
static bool is_abs_path(const char *s)
{
	if (s[0] == '/' || (s[0] == '\\' && s[1] == '\\'))
		return s[1] != '\0';
	return ((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z')) && s[1] == ':' &&
	       (s[2] == '\\' || s[2] == '/');
}

/* ---- scan ---- */

struct hashed {
	char *path;
	char sha[65];
	int64_t size;
};

struct scan {
	cj *assets;        /* manifest.assets */
	cj *asset_sources; /* may be NULL */
	cj *missing;       /* may be NULL */
	show_hash_fn hash;
	void *opaque;
	struct hashed *cache;
	size_t cache_count, cache_cap;
	char *err;
	size_t err_size;
	bool ok;
};

struct owner {
	const char *type;
	const char *source; /* uuid of the source (or NULL) */
	const char *filter; /* uuid of the filter (or NULL) */
	const struct known *known;
	bool quiet; /* known path key whose guard setting is false: a missing file is not worth reporting */
};

static bool default_hash(void *opaque, const char *path, char sha256[65], int64_t *size)
{
	(void)opaque;
	return sha256_file(path, sha256, size, NULL, NULL);
}

static void scan_fail(struct scan *s, const char *fmt, const char *arg)
{
	if (s->ok && s->err && s->err_size)
		snprintf(s->err, s->err_size, fmt, arg);
	s->ok = false;
}

static const char *file_name(const char *path)
{
	const char *name = path;
	for (const char *p = path; *p; p++)
		if (*p == '/' || *p == '\\')
			name = p + 1;
	return name;
}

static cj *id_or_null(const char *s)
{
	return s ? cj_new_string(s) : cj_new_null();
}

static cj *make_ref(const struct owner *o, const char *key)
{
	cj *ref = cj_new_object();
	if (!ref)
		return NULL;
	cj_set(ref, "source", id_or_null(o->source));
	cj_set(ref, "filter", id_or_null(o->filter));
	cj_set_str(ref, "key", key);
	return ref;
}

static char *dup_str(const char *s)
{
	size_t n = strlen(s) + 1;
	char *p = malloc(n);
	if (p)
		memcpy(p, s, n);
	return p;
}

/* Hashes path once per call (the caller's hash may cache across calls too). */
static const struct hashed *hash_path(struct scan *s, const char *path)
{
	for (size_t i = 0; i < s->cache_count; i++)
		if (strcmp(s->cache[i].path, path) == 0)
			return &s->cache[i];
	if (s->cache_count == s->cache_cap) {
		size_t cap = s->cache_cap ? s->cache_cap * 2 : 16;
		struct hashed *p = realloc(s->cache, cap * sizeof(*p));
		if (!p) {
			scan_fail(s, "out of memory%s", "");
			return NULL;
		}
		s->cache = p;
		s->cache_cap = cap;
	}
	struct hashed *h = &s->cache[s->cache_count];
	memset(h, 0, sizeof(*h));
	h->path = dup_str(path);
	if (!h->path) {
		scan_fail(s, "out of memory%s", "");
		return NULL;
	}
	if (!s->hash(s->opaque, path, h->sha, &h->size) || !show_is_sha256(h->sha)) {
		free(h->path);
		scan_fail(s, "can't read %s to hash it", path);
		return NULL;
	}
	s->cache_count++;
	return h;
}

static cj *find_asset(cj *assets, const char *sha)
{
	for (size_t i = 0; i < cj_size(assets); i++) {
		const char *have = cj_get_str(cj_at(assets, i), "sha256");
		if (have && strcmp(have, sha) == 0)
			return cj_at(assets, i);
	}
	return NULL;
}

/* An existing asset file at v (a string node): hash it, record it, swap in the placeholder. */
static void take_asset(struct scan *s, const struct owner *o, cj *v, const char *path, const char *ptr)
{
	const struct hashed *h = hash_path(s, path);
	if (!h)
		return;
	cj *entry = find_asset(s->assets, h->sha);
	if (!entry) {
		entry = cj_append(s->assets, cj_new_object());
		if (!entry) {
			scan_fail(s, "out of memory%s", "");
			return;
		}
		cj_set_str(entry, "sha256", h->sha);
		cj_set_int(entry, "size", h->size);
		cj_set_str(entry, "kind", show_asset_kind(o->type, path));
		cj_set_str(entry, "name", file_name(path));
		cj_set(entry, "refs", cj_new_array());
	}
	cj *refs = cj_get_or_add_array(entry, "refs");
	cj *ref = make_ref(o, ptr);
	if (!refs || !ref || !cj_append(refs, ref)) {
		scan_fail(s, "out of memory%s", "");
		return;
	}
	if (s->asset_sources && !cj_get(s->asset_sources, h->sha))
		cj_set_str(s->asset_sources, h->sha, path);
	char placeholder[96];
	show_asset_placeholder(h->sha, placeholder, sizeof(placeholder));
	if (!cj_set_string_value(v, placeholder))
		scan_fail(s, "out of memory%s", "");
}

static void note_missing(struct scan *s, const struct owner *o, const char *path, const char *ptr)
{
	if (!s->missing)
		return;
	cj *m = cj_append(s->missing, make_ref(o, ptr));
	if (!m || !cj_set_str(m, "path", path))
		scan_fail(s, "out of memory%s", "");
}

static void handle_string(struct scan *s, const struct owner *o, cj *v, const char *ptr)
{
	const char *str = cj_str(v);
	char placeholder[65];
	if (!str || !*str || show_asset_placeholder_parse(str, placeholder))
		return;
	bool known = pointer_is_known(o->known, ptr);
	if (known ? is_url(str) : !is_abs_path(str))
		return;
	if (!known && strlen(str) > 4096)
		return;
	if (sfs_exists(str) && !sfs_is_dir(str))
		take_asset(s, o, v, str, ptr);
	else if (known && !o->quiet)
		note_missing(s, o, str, ptr);
}

static void walk(struct scan *s, const struct owner *o, cj *v, char *ptr, size_t ptr_size)
{
	size_t len = strlen(ptr);
	if (!s->ok)
		return;
	if (cj_is_string(v)) {
		handle_string(s, o, v, ptr);
	} else if (cj_is_array(v)) {
		for (size_t i = 0; i < cj_size(v) && s->ok; i++) {
			char idx[24];
			snprintf(idx, sizeof(idx), "%zu", i);
			if (cj_pointer_append(ptr, ptr_size, idx))
				walk(s, o, cj_at(v, i), ptr, ptr_size);
			ptr[len] = '\0';
		}
	} else if (cj_is_object(v)) {
		for (size_t i = 0; i < cj_size(v) && s->ok; i++) {
			if (cj_pointer_append(ptr, ptr_size, cj_key_at(v, i)))
				walk(s, o, cj_at(v, i), ptr, ptr_size);
			ptr[len] = '\0';
		}
	}
}

/* One source or filter object: scan its settings. */
static void scan_object(struct scan *s, const char *parent_uuid, const cj *obj, bool is_filter)
{
	const char *type = cj_get_str(obj, "id");
	const char *uuid = cj_get_str(obj, "uuid");
	cj *settings = cj_get(obj, "settings");
	struct owner o;
	char ptr[1024] = "";
	if (!type)
		type = cj_get_str(obj, "versioned_id");
	if (!cj_is_object(settings))
		return;
	o.type = type;
	o.source = is_filter ? parent_uuid : uuid;
	o.filter = is_filter ? uuid : NULL;
	o.known = find_known(type);
	o.quiet = o.known && o.known->guard && cj_get(settings, o.known->guard) &&
		  !cj_get_bool(settings, o.known->guard, true);
	if (cj_pointer_append(ptr, sizeof(ptr), "settings"))
		walk(s, &o, settings, ptr, sizeof(ptr));
}

static void scan_source(struct scan *s, const cj *src)
{
	scan_object(s, NULL, src, false);
	const cj *filters = cj_get(src, "filters");
	for (size_t i = 0; cj_is_array(filters) && i < cj_size(filters) && s->ok; i++)
		if (cj_is_object(cj_at(filters, i)))
			scan_object(s, cj_get_str(src, "uuid"), cj_at(filters, i), true);
}

bool show_assets_rewrite(cj *collection, cj *manifest, cj *asset_sources, cj *missing, show_hash_fn hash, void *opaque,
			 char *err, size_t err_size)
{
	struct scan s = {0};
	static const char *const lists[] = {"sources", "groups", "transitions"};
	if (err && err_size)
		err[0] = '\0';
	if (!cj_is_object(collection) || !cj_is_object(manifest)) {
		if (err && err_size)
			snprintf(err, err_size, "collection or manifest is not an object");
		return false;
	}
	s.assets = cj_get_or_add_array(manifest, "assets");
	s.asset_sources = asset_sources;
	s.missing = missing;
	s.hash = hash ? hash : default_hash;
	s.opaque = opaque;
	s.err = err;
	s.err_size = err_size;
	s.ok = s.assets != NULL;
	if (!s.ok && err && err_size)
		snprintf(err, err_size, "out of memory");

	for (size_t i = 0; i < cj_size(collection) && s.ok; i++) {
		const char *key = cj_key_at(collection, i);
		cj *v = cj_at(collection, i);
		bool listed = false;
		for (size_t l = 0; l < sizeof(lists) / sizeof(lists[0]); l++)
			listed = listed || strcmp(key, lists[l]) == 0;
		if (listed && cj_is_array(v)) {
			for (size_t j = 0; j < cj_size(v) && s.ok; j++)
				if (cj_is_object(cj_at(v, j)))
					scan_source(&s, cj_at(v, j));
		} else if (cj_is_object(v) && cj_get_str(v, "id") && cj_is_object(cj_get(v, "settings"))) {
			scan_source(&s, v); /* a global audio device ("DesktopAudioDevice1", ...) */
		}
	}
	for (size_t i = 0; i < s.cache_count; i++)
		free(s.cache[i].path);
	free(s.cache);
	return s.ok;
}

/* ---- resolve ---- */

struct replacement {
	cj *node;
	char *path;
};

struct resolve {
	const cj *manifest;
	show_asset_path_fn path_for;
	void *opaque;
	struct replacement *list;
	size_t count, cap;
	char *err;
	size_t err_size;
	bool ok;
};

static void resolve_fail(struct resolve *r, const char *fmt, const char *arg)
{
	if (r->ok && r->err && r->err_size)
		snprintf(r->err, r->err_size, fmt, arg);
	r->ok = false;
}

static void resolve_walk(struct resolve *r, cj *v)
{
	if (!r->ok)
		return;
	if (cj_is_string(v)) {
		char sha[65];
		if (!show_asset_placeholder_parse(cj_str(v), sha))
			return;
		const cj *asset = NULL;
		const cj *assets = cj_get(r->manifest, "assets");
		for (size_t i = 0; i < cj_size(assets); i++) {
			const char *a = cj_get_str(cj_at(assets, i), "sha256");
			if (a && strcmp(a, sha) == 0)
				asset = cj_at(assets, i);
		}
		if (!asset) {
			resolve_fail(r, "asset %s is not in the manifest", sha);
			return;
		}
		char path[4096];
		path[0] = '\0';
		if (!r->path_for(r->opaque, asset, path, sizeof(path)) || !path[0]) {
			resolve_fail(r, "no local file for asset %s", sha);
			return;
		}
		if (r->count == r->cap) {
			size_t cap = r->cap ? r->cap * 2 : 16;
			struct replacement *p = realloc(r->list, cap * sizeof(*p));
			if (!p) {
				resolve_fail(r, "out of memory%s", "");
				return;
			}
			r->list = p;
			r->cap = cap;
		}
		r->list[r->count].node = v;
		r->list[r->count].path = dup_str(path);
		if (!r->list[r->count].path) {
			resolve_fail(r, "out of memory%s", "");
			return;
		}
		r->count++;
	} else if (cj_is_array(v) || cj_is_object(v)) {
		for (size_t i = 0; i < cj_size(v) && r->ok; i++)
			resolve_walk(r, cj_at(v, i));
	}
}

/* Two passes instead of a clone: pass one resolves every placeholder (any failure changes nothing), pass two swaps the
 * strings in place. */
bool show_assets_resolve(cj *collection, const cj *manifest, show_asset_path_fn path_for, void *opaque, char *err,
			 size_t err_size)
{
	struct resolve r = {0};
	if (err && err_size)
		err[0] = '\0';
	r.manifest = manifest;
	r.path_for = path_for;
	r.opaque = opaque;
	r.err = err;
	r.err_size = err_size;
	r.ok = collection && manifest && path_for;
	if (!r.ok && err && err_size)
		snprintf(err, err_size, "bad arguments");
	resolve_walk(&r, collection);
	for (size_t i = 0; r.ok && i < r.count; i++)
		if (!cj_set_string_value(r.list[i].node, r.list[i].path))
			resolve_fail(&r, "out of memory%s", "");
	for (size_t i = 0; i < r.count; i++)
		free(r.list[i].path);
	free(r.list);
	return r.ok;
}
