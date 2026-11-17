/* Show-profile manifest, schema 1; see manifest.h. */
#include "manifest.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

const char *const show_asset_kinds[] = {"lut", "image", "video", "audio", "font", "html", "other", NULL};

static bool is_hex(char c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool show_is_sha256(const char *s)
{
	if (!s || strlen(s) != 64)
		return false;
	for (int i = 0; i < 64; i++)
		if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
			return false;
	return true;
}

bool show_is_uuid(const char *s)
{
	static const int groups[] = {8, 4, 4, 4, 12};
	if (!s || strlen(s) != 36)
		return false;
	const char *p = s;
	for (int g = 0; g < 5; g++) {
		for (int i = 0; i < groups[g]; i++, p++)
			if (!is_hex(*p))
				return false;
		if (g < 4 && *p++ != '-')
			return false;
	}
	return true;
}

/* Valid UTF-8 (no overlongs, no surrogates), no C0/DEL control characters. */
static bool utf8_clean(const char *s)
{
	const unsigned char *p = (const unsigned char *)s;
	while (*p) {
		unsigned c = *p;
		int extra;
		unsigned cp, min;
		if (c < 0x80) {
			if (c < 0x20 || c == 0x7F)
				return false;
			p++;
			continue;
		} else if ((c & 0xE0) == 0xC0) {
			extra = 1;
			cp = c & 0x1F;
			min = 0x80;
		} else if ((c & 0xF0) == 0xE0) {
			extra = 2;
			cp = c & 0x0F;
			min = 0x800;
		} else if ((c & 0xF8) == 0xF0) {
			extra = 3;
			cp = c & 0x07;
			min = 0x10000;
		} else {
			return false;
		}
		p++;
		for (int i = 0; i < extra; i++, p++) {
			if ((*p & 0xC0) != 0x80)
				return false;
			cp = (cp << 6) | (*p & 0x3F);
		}
		if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
			return false;
		if (cp >= 0x80 && cp < 0xA0) /* C1 controls */
			return false;
	}
	return true;
}

bool show_name_is_valid(const char *name)
{
	if (!name)
		return false;
	size_t n = strlen(name);
	if (n == 0 || n > SHOW_NAME_MAX || name[0] == ' ' || name[n - 1] == ' ')
		return false;
	if (strpbrk(name, "/\\:*?\"<>|"))
		return false;
	return utf8_clean(name);
}

bool show_path_is_safe(const char *path)
{
	if (!path || !*path || strlen(path) > 255)
		return false;
	if (path[0] == '/' || path[0] == '\\' || strchr(path, '\\') || strchr(path, ':'))
		return false;
	for (const unsigned char *p = (const unsigned char *)path; *p; p++)
		if (*p < 0x20 || *p == 0x7F)
			return false;
	const char *seg = path;
	for (;;) {
		const char *end = strchr(seg, '/');
		size_t len = end ? (size_t)(end - seg) : strlen(seg);
		if (len == 0)
			return false;
		if ((len == 1 && seg[0] == '.') || (len == 2 && seg[0] == '.' && seg[1] == '.'))
			return false;
		if (!end)
			break;
		seg = end + 1;
	}
	return utf8_clean(path);
}

bool show_asset_placeholder_parse(const char *value, char out[65])
{
	static const char prefix[] = "${ctd-asset:";
	const size_t plen = sizeof(prefix) - 1;
	if (!value || strncmp(value, prefix, plen) != 0 || strlen(value) != plen + 65 || value[plen + 64] != '}')
		return false;
	memcpy(out, value + plen, 64);
	out[64] = '\0';
	return show_is_sha256(out);
}

void show_asset_placeholder(const char *sha256, char *out, size_t size)
{
	snprintf(out, size, "${ctd-asset:%s}", sha256);
}

cj *show_manifest_new(const char *id, const char *name)
{
	cj *m = cj_new_object();
	if (!m)
		return NULL;
	cj_set_int(m, "schema", SHOW_MANIFEST_SCHEMA);
	cj_set_str(m, "showProfileId", id);
	cj_set(m, "relayProfileId", cj_new_null());
	cj_set_str(m, "name", name);
	cj_set_int(m, "revision", 0);
	cj_set_int(m, "parentRevision", 0);
	cj_set_str(m, "savedAt", "");
	cj_set(m, "savedBy", cj_new_object());
	cj_set(m, "versions", cj_new_object());
	cj_set(m, "files", cj_new_object());
	cj_set(m, "assets", cj_new_array());
	cj_set(m, "devices", cj_new_array());
	cj_set(m, "overlays", cj_new_array());
	cj_set(m, "secrets", cj_new_array());
	return m;
}

/* ---- validation ---- */

struct vctx {
	char *err;
	size_t size;
	bool ok;
};

static bool bad(struct vctx *v, const char *fmt, ...)
{
	if (v->ok && v->err && v->size) {
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(v->err, v->size, fmt, ap);
		va_end(ap);
	}
	v->ok = false;
	return false;
}

static bool is_whole(const cj *n)
{
	return cj_is_number(n) && cj_int(n, -1) == (int64_t)cj_num(n, -0.5);
}

static bool check_str(struct vctx *v, const cj *obj, const char *key, const char *where, bool required)
{
	const cj *s = cj_get(obj, key);
	if (!s) {
		if (required)
			return bad(v, "%s: missing \"%s\"", where, key);
		return true;
	}
	if (!cj_is_string(s) && !(cj_kind(s) == CJ_NULL && !required))
		return bad(v, "%s: \"%s\" must be a string", where, key);
	return true;
}

static bool check_file_entry(struct vctx *v, const cj *value, const char *where)
{
	if (cj_is_string(value)) {
		if (!show_path_is_safe(cj_str(value)))
			return bad(v, "%s: unsafe path \"%s\"", where, cj_str(value));
		return true;
	}
	if (cj_is_array(value)) {
		for (size_t i = 0; i < cj_size(value); i++)
			if (!check_file_entry(v, cj_at(value, i), where))
				return false;
		return true;
	}
	if (cj_is_object(value)) {
		const cj *file = cj_get(value, "file");
		if (file)
			return check_file_entry(v, file, where);
		for (size_t i = 0; i < cj_size(value); i++)
			if (!check_file_entry(v, cj_at(value, i), where))
				return false;
		return true;
	}
	return bad(v, "%s: expected a path", where);
}

static bool known_kind(const char *kind)
{
	for (int i = 0; kind && show_asset_kinds[i]; i++)
		if (strcmp(kind, show_asset_kinds[i]) == 0)
			return true;
	return false;
}

static bool check_ref(struct vctx *v, const cj *ref, const char *where)
{
	if (!cj_is_object(ref))
		return bad(v, "%s: a ref must be an object", where);
	const cj *source = cj_get(ref, "source");
	if (source && cj_kind(source) != CJ_NULL && !cj_is_string(source))
		return bad(v, "%s: ref source must be a string or null", where);
	const char *key = cj_get_str(ref, "key");
	if (!key || key[0] != '/')
		return bad(v, "%s: ref key must be a JSON Pointer", where);
	return true;
}

bool show_manifest_validate(const cj *m, char *err, size_t err_size)
{
	struct vctx v = {err, err_size, true};
	char where[96];
	if (err && err_size)
		err[0] = '\0';
	if (!cj_is_object(m))
		return bad(&v, "manifest: not an object");

	const cj *schema = cj_get(m, "schema");
	if (!is_whole(schema))
		return bad(&v, "manifest: missing schema");
	if (cj_int(schema, 0) != SHOW_MANIFEST_SCHEMA)
		return bad(&v, "manifest: schema %lld is not supported by this CTD OBS (update CTD OBS)",
			   (long long)cj_int(schema, 0));

	if (!show_is_uuid(cj_get_str(m, "showProfileId")))
		return bad(&v, "manifest: showProfileId must be a uuid");
	if (!show_name_is_valid(cj_get_str(m, "name")))
		return bad(&v, "manifest: bad name");
	if (!check_str(&v, m, "relayProfileId", "manifest", false))
		return false;

	const cj *rev = cj_get(m, "revision"), *parent = cj_get(m, "parentRevision");
	if (!is_whole(rev) || cj_int(rev, -1) < 0)
		return bad(&v, "manifest: revision must be a whole number >= 0");
	if (parent && cj_kind(parent) != CJ_NULL) {
		if (!is_whole(parent) || cj_int(parent, -1) < 0)
			return bad(&v, "manifest: parentRevision must be a whole number >= 0");
		if (cj_int(rev, 0) > 0 && cj_int(parent, 0) >= cj_int(rev, 0))
			return bad(&v, "manifest: parentRevision must be below revision");
	}
	if (!check_str(&v, m, "savedAt", "manifest", false))
		return false;

	const cj *files = cj_get(m, "files");
	if (!cj_is_object(files))
		return bad(&v, "manifest: files must be an object");
	if (!cj_is_string(cj_get(files, "collection")))
		return bad(&v, "manifest: files.collection missing");
	for (size_t i = 0; i < cj_size(files); i++) {
		snprintf(where, sizeof(where), "files.%.40s", cj_key_at(files, i));
		if (!check_file_entry(&v, cj_at(files, i), where))
			return false;
	}

	const cj *assets = cj_get(m, "assets");
	if (assets && !cj_is_array(assets))
		return bad(&v, "manifest: assets must be an array");
	for (size_t i = 0; i < cj_size(assets); i++) {
		const cj *a = cj_at(assets, i);
		snprintf(where, sizeof(where), "assets[%zu]", i);
		if (!cj_is_object(a))
			return bad(&v, "%s: not an object", where);
		const char *sha = cj_get_str(a, "sha256");
		if (!show_is_sha256(sha))
			return bad(&v, "%s: bad sha256", where);
		for (size_t j = 0; j < i; j++)
			if (strcmp(sha, cj_get_str(cj_at(assets, j), "sha256")) == 0)
				return bad(&v, "%s: duplicate sha256", where);
		const cj *size = cj_get(a, "size");
		if (!is_whole(size) || cj_int(size, -1) < 0)
			return bad(&v, "%s: bad size", where);
		if (!known_kind(cj_get_str(a, "kind")))
			return bad(&v, "%s: unknown kind", where);
		if (!check_str(&v, a, "name", where, true))
			return false;
		const char *name = cj_get_str(a, "name");
		if (strchr(name, '/') || strchr(name, '\\') || !utf8_clean(name))
			return bad(&v, "%s: name must be a plain file name", where);
		const cj *refs = cj_get(a, "refs");
		if (refs && !cj_is_array(refs))
			return bad(&v, "%s: refs must be an array", where);
		for (size_t j = 0; j < cj_size(refs); j++)
			if (!check_ref(&v, cj_at(refs, j), where))
				return false;
	}

	const cj *devices = cj_get(m, "devices");
	if (devices && !cj_is_array(devices))
		return bad(&v, "manifest: devices must be an array");
	for (size_t i = 0; i < cj_size(devices); i++) {
		const cj *d = cj_at(devices, i);
		snprintf(where, sizeof(where), "devices[%zu]", i);
		if (!cj_is_object(d))
			return bad(&v, "%s: not an object", where);
		if (!check_str(&v, d, "slot", where, true) || !check_str(&v, d, "sourceType", where, true) ||
		    !check_str(&v, d, "class", where, false) || !check_str(&v, d, "label", where, false))
			return false;
		const cj *identity = cj_get(d, "identity");
		if (identity && !cj_is_object(identity))
			return bad(&v, "%s: identity must be an object", where);
	}

	const cj *overlays = cj_get(m, "overlays");
	if (overlays && !cj_is_array(overlays))
		return bad(&v, "manifest: overlays must be an array");
	for (size_t i = 0; i < cj_size(overlays); i++) {
		const cj *o = cj_at(overlays, i);
		snprintf(where, sizeof(where), "overlays[%zu]", i);
		if (!cj_is_object(o))
			return bad(&v, "%s: not an object", where);
		if (!check_str(&v, o, "source", where, true) || !check_str(&v, o, "slot", where, true) ||
		    !check_str(&v, o, "canvas", where, true))
			return false;
	}

	const cj *secrets = cj_get(m, "secrets");
	if (secrets && !cj_is_array(secrets))
		return bad(&v, "manifest: secrets must be an array");
	for (size_t i = 0; i < cj_size(secrets); i++) {
		const cj *s = cj_at(secrets, i);
		snprintf(where, sizeof(where), "secrets[%zu]", i);
		if (!check_ref(&v, s, where))
			return false;
		if (cj_get(s, "value"))
			return bad(&v, "%s: a secret's value never goes in a manifest", where);
	}
	return v.ok;
}
