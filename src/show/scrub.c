/* Secret scrubber (ticket T10, design section 8); see scrub.h for the rules. */
#include "scrub.h"
#include "bundle.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define POINTER_MAX 1024

/* ---- names ---- */

/* name split into lowercase words at separators and camelCase humps ("streamKey" -> stream, key) */
static size_t split_words(const char *name, char words[][32], size_t max)
{
	size_t n = 0, len = 0;
	char prev = 0;
	for (const char *p = name;; p++) {
		char c = *p;
		bool alnum = c && isalnum((unsigned char)c);
		bool hump = alnum && isupper((unsigned char)c) && prev && islower((unsigned char)prev);
		if ((!alnum || hump) && len) {
			if (n < max)
				words[n++][len] = '\0';
			len = 0;
		}
		if (!c)
			break;
		if (alnum && n < max && len < 31)
			words[n][len++] = (char)tolower((unsigned char)c);
		prev = c;
	}
	return n;
}

static bool in_list(const char *word, const char *const *list)
{
	for (; *list; list++)
		if (strcmp(word, *list) == 0)
			return true;
	return false;
}

bool show_scrub_is_secret_name(const char *name)
{
	static const char *const anywhere[] = {"password",   "passwd",      "passphrase", "secret",     "token",
					       "credential", "credentials", "cookie",     "bearer",     "apikey",
					       "streamkey",  "accesskey",   "secretkey",  "privatekey", NULL};
	static const char *const exact[] = {"key", "pass", "pwd", "auth", "authorization", "sig", "signature", NULL};
	static const char *const key_owners[] = {"stream",       "api",        "auth",   "access",  "private",
						 "secret",       "client",     "app",    "license", "session",
						 "signing",      "encryption", "ingest", "overlay", "relay",
						 "subscription", NULL};
	char words[16][32];
	if (!name || !*name)
		return false;
	size_t n = split_words(name, words, 16);
	if (n == 0)
		return false;
	if (n == 1 && in_list(words[0], exact))
		return true;
	/* "x_auth_token", "streamKey", "apikey" */
	char joined[16 * 32] = "";
	size_t used = 0;
	for (size_t i = 0; i < n; i++) {
		if (in_list(words[i], anywhere))
			return true;
		if (i > 0 && strcmp(words[i], "key") == 0 && in_list(words[i - 1], key_owners))
			return true;
		size_t len = strlen(words[i]); /* < 32, and at most 16 words: always fits */
		memcpy(joined + used, words[i], len + 1);
		used += len;
	}
	for (const char *const *w = anywhere; *w; w++)
		if (strstr(joined, *w))
			return true;
	return false;
}

/* ---- values ---- */

static bool all_of(const char *s, size_t n, int (*pred)(int))
{
	for (size_t i = 0; i < n; i++)
		if (!pred((unsigned char)s[i]))
			return false;
	return true;
}

static int is_key_char(int c)
{
	return isalnum(c) || c == '_' || c == '-';
}

static int is_lower_alnum(int c)
{
	return isdigit(c) || (c >= 'a' && c <= 'z');
}

static int is_b64url(int c)
{
	return isalnum(c) || c == '_' || c == '-' || c == '=';
}

bool show_scrub_is_secret_value(const char *v)
{
	if (!v)
		return false;
	size_t n = strlen(v);
	if (n < 16 || n > 4096)
		return false;
	/* Twitch: live_<digits>_<alnum> */
	if (strncmp(v, "live_", 5) == 0) {
		const char *p = v + 5;
		size_t d = strspn(p, "0123456789");
		if (d >= 4 && p[d] == '_' && strlen(p + d + 1) >= 16 &&
		    all_of(p + d + 1, strlen(p + d + 1), is_key_char))
			return true;
	}
	/* payment/API style keys */
	if ((strncmp(v, "sk_live_", 8) == 0 || strncmp(v, "sk_test_", 8) == 0 || strncmp(v, "rk_live_", 8) == 0) &&
	    all_of(v, n, is_key_char))
		return true;
	/* Facebook Live: FB-<digits>-<digits>-<key> */
	if (strncmp(v, "FB-", 3) == 0 && isdigit((unsigned char)v[3]) && all_of(v, n, is_key_char))
		return true;
	/* YouTube: xxxx-xxxx-xxxx-xxxx(-xxxx) */
	if ((n == 19 || n == 24) && all_of(v, n, is_key_char)) {
		bool shape = true;
		for (size_t i = 0; i < n && shape; i++)
			shape = (i % 5 == 4) ? v[i] == '-' : is_lower_alnum((unsigned char)v[i]) != 0;
		if (shape)
			return true;
	}
	/* JWT: eyJ....eyJ....sig */
	if (strncmp(v, "eyJ", 3) == 0 && n >= 30) {
		int dots = 0;
		bool ok = true;
		for (size_t i = 0; i < n && ok; i++) {
			if (v[i] == '.')
				dots++;
			else
				ok = is_b64url((unsigned char)v[i]) != 0;
		}
		if (ok && dots == 2)
			return true;
	}
	/* "Bearer <token>" / "Basic <creds>" */
	size_t scheme = strncmp(v, "Bearer ", 7) == 0 || strncmp(v, "bearer ", 7) == 0 ? 7
			: strncmp(v, "Basic ", 6) == 0                                 ? 6
										       : 0;
	if (scheme && !strchr(v + scheme, ' ') && strlen(v + scheme) >= 12)
		return true;
	return false;
}

/* ---- URLs ---- */

struct sbuf {
	char *data;
	size_t len, cap;
	bool oom;
};

static void sb_put(struct sbuf *b, const char *s, size_t n)
{
	if (b->oom)
		return;
	if (b->len + n + 1 > b->cap) {
		size_t cap = b->cap ? b->cap * 2 : 128;
		while (cap < b->len + n + 1)
			cap *= 2;
		char *d = realloc(b->data, cap);
		if (!d) {
			b->oom = true;
			return;
		}
		b->data = d;
		b->cap = cap;
	}
	memcpy(b->data + b->len, s, n);
	b->len += n;
	b->data[b->len] = '\0';
}

static void sb_puts(struct sbuf *b, const char *s)
{
	sb_put(b, s, strlen(s));
}

static bool secret_param(const char *name, size_t n)
{
	static const char *const names[] = {"key",           "token",         "access_token",  "auth",   "secret",
					    "password",      "sig",           "signature",     "apikey", "api_key",
					    "streamkey",     "stream_key",    "passwd",        "pwd",    "id_token",
					    "refresh_token", "client_secret", "code_verifier", NULL};
	char lower[64];
	if (n == 0 || n >= sizeof(lower))
		return false;
	for (size_t i = 0; i < n; i++)
		lower[i] = (char)tolower((unsigned char)name[i]);
	lower[n] = '\0';
	return in_list(lower, names) || show_scrub_is_secret_name(lower);
}

/* name=value&name=value; secret values become the placeholder */
static bool scrub_params(struct sbuf *b, const char *s, size_t n)
{
	bool changed = false;
	size_t i = 0;
	while (i <= n) {
		size_t end = i;
		while (end < n && s[end] != '&' && s[end] != ';')
			end++;
		const char *eq = memchr(s + i, '=', end - i);
		if (eq && secret_param(s + i, (size_t)(eq - (s + i))) && (size_t)(eq - s) + 1 < end &&
		    strncmp(eq + 1, SHOW_SECRET_PLACEHOLDER, strlen(SHOW_SECRET_PLACEHOLDER)) != 0) {
			sb_put(b, s + i, (size_t)(eq - (s + i)) + 1);
			sb_puts(b, SHOW_SECRET_PLACEHOLDER);
			changed = true;
		} else {
			sb_put(b, s + i, end - i);
		}
		if (end < n)
			sb_put(b, s + end, 1);
		i = end + 1;
	}
	return changed;
}

static bool url_scheme_len(const char *url, size_t *len)
{
	size_t i = 0;
	if (!isalpha((unsigned char)url[0]))
		return false;
	while (url[i] && (isalnum((unsigned char)url[i]) || url[i] == '+' || url[i] == '-' || url[i] == '.'))
		i++;
	if (strncmp(url + i, "://", 3) != 0 || i > 16)
		return false;
	*len = i + 3;
	return true;
}

bool show_scrub_url(const char *url, char **out)
{
	size_t scheme;
	*out = NULL;
	if (!url || !url_scheme_len(url, &scheme))
		return false;
	struct sbuf b = {0};
	bool changed = false;
	const char *authority = url + scheme;
	size_t alen = strcspn(authority, "/?#");
	const char *at = NULL;
	for (size_t i = 0; i < alen; i++)
		if (authority[i] == '@')
			at = authority + i;
	sb_put(&b, url, scheme);
	if (at) {
		const size_t ph = strlen(SHOW_SECRET_PLACEHOLDER);
		sb_puts(&b, SHOW_SECRET_PLACEHOLDER);
		sb_put(&b, at, (size_t)(authority + alen - at));
		changed = !((size_t)(at - authority) == ph && strncmp(authority, SHOW_SECRET_PLACEHOLDER, ph) == 0);
	} else {
		sb_put(&b, authority, alen);
	}
	const char *p = authority + alen;
	/* path: a segment that is a credential on its own (rtmp://host/app/<stream key>) */
	size_t plen = strcspn(p, "?#");
	for (size_t i = 0; i < plen;) {
		size_t seg = i + 1;
		while (seg < plen && p[seg] != '/')
			seg++;
		size_t start = p[i] == '/' ? i + 1 : i;
		char word[4097];
		size_t wl = seg - start;
		if (wl > 0 && wl < sizeof(word)) {
			memcpy(word, p + start, wl);
			word[wl] = '\0';
		} else {
			word[0] = '\0';
		}
		sb_put(&b, p + i, start - i);
		if (word[0] && show_scrub_is_secret_value(word)) {
			sb_puts(&b, SHOW_SECRET_PLACEHOLDER);
			changed = true;
		} else {
			sb_put(&b, p + start, wl);
		}
		i = seg;
	}
	p += plen;
	if (*p == '?') {
		sb_put(&b, p, 1);
		p++;
		size_t qlen = strcspn(p, "#");
		changed = scrub_params(&b, p, qlen) || changed;
		p += qlen;
	}
	if (*p == '#') {
		/* OAuth puts tokens in the fragment (#access_token=...) */
		sb_put(&b, p, 1);
		p++;
		changed = scrub_params(&b, p, strlen(p)) || changed;
	}
	if (b.oom || !changed) {
		free(b.data);
		return false;
	}
	*out = b.data;
	return true;
}

/* ---- documents ---- */

struct walk {
	cj *manifest;     /* NULL for settings documents */
	cj *local;        /* local secrets object */
	const char *file; /* settings documents: the key prefix; NULL for a collection */
	struct show_scrub_report *report;
	bool ok;
};

static bool is_owner(const cj *obj)
{
	return cj_is_object(obj) && cj_is_string(cj_get(obj, "uuid")) &&
	       (cj_get(obj, "id") || cj_get(obj, "versioned_id"));
}

static bool is_overlay_source(const cj *source)
{
	const char *id = cj_get_str(source, "id");
	if (!id)
		id = cj_get_str(source, "versioned_id");
	if (!id || strncmp(id, "browser_source", 14) != 0)
		return false;
	const char *slot = cj_get_str(cj_get(source, "private_settings"), "ctd_overlay_slot");
	return slot && *slot;
}

static void local_key(char *out, size_t size, const struct walk *w, const char *owner, const char *pointer)
{
	snprintf(out, size, "%s|%s", w->file ? w->file : (owner ? owner : ""), pointer);
}

static void record(struct walk *w, const char *owner, const char *pointer, const char *original, bool overlay)
{
	char key[POINTER_MAX + 128];
	local_key(key, sizeof(key), w, owner, pointer);
	if (!cj_set_str(w->local, key, original))
		w->ok = false;
	if (overlay || !w->manifest)
		return;
	cj *list = cj_get_or_add_array(w->manifest, "secrets");
	cj *entry = cj_new_object();
	if (owner)
		cj_set_str(entry, "source", owner);
	else
		cj_set(entry, "source", cj_new_null());
	cj_set_str(entry, "key", pointer);
	if (!cj_append(list, entry))
		w->ok = false;
	if (w->report)
		w->report->secrets++;
}

static bool skipped_member(const char *name)
{
	/* identities, and hotkey bindings (whose "key" members are OBS_KEY_* names, not secrets) */
	if (strcmp(name, "uuid") == 0 || strcmp(name, "id") == 0 || strcmp(name, "versioned_id") == 0 ||
	    strcmp(name, "name") == 0 || strcmp(name, "prev_ver") == 0)
		return true;
	char lower[64];
	size_t n = strlen(name);
	if (n >= sizeof(lower))
		return false;
	for (size_t i = 0; i <= n; i++)
		lower[i] = (char)tolower((unsigned char)name[i]);
	return strstr(lower, "hotkey") != NULL;
}

static void scrub_string(struct walk *w, cj *value, const char *member, const char *owner, const char *pointer,
			 bool overlay_url)
{
	const char *s = cj_str(value);
	if (!s || !*s || strcmp(s, SHOW_SECRET_PLACEHOLDER) == 0 || strcmp(s, SHOW_OVERLAY_PLACEHOLDER) == 0 ||
	    strncmp(s, SHOW_ASSET_PREFIX, strlen(SHOW_ASSET_PREFIX)) == 0)
		return;
	if (overlay_url) {
		record(w, owner, pointer, s, true);
		cj_set_string_value(value, SHOW_OVERLAY_PLACEHOLDER);
		return;
	}
	if (member && show_scrub_is_secret_name(member) && strncmp(s, "OBS_KEY_", 8) != 0) {
		record(w, owner, pointer, s, false);
		cj_set_string_value(value, SHOW_SECRET_PLACEHOLDER);
		return;
	}
	char *scrubbed = NULL;
	if (show_scrub_url(s, &scrubbed)) {
		record(w, owner, pointer, s, false);
		cj_set_string_value(value, scrubbed);
		free(scrubbed);
		return;
	}
	if (show_scrub_is_secret_value(s)) {
		record(w, owner, pointer, s, false);
		cj_set_string_value(value, SHOW_SECRET_PLACEHOLDER);
	}
}

static void walk(struct walk *w, cj *node, const char *member, const char *owner, char *pointer, int depth,
		 bool overlay_source)
{
	if (depth > CJ_MAX_DEPTH)
		return;
	if (cj_is_string(node)) {
		bool overlay_url = overlay_source && strcmp(pointer, "/settings/url") == 0;
		scrub_string(w, node, member, owner, pointer, overlay_url);
		return;
	}
	if (!cj_is_object(node) && !cj_is_array(node))
		return;
	char saved[POINTER_MAX];
	snprintf(saved, sizeof(saved), "%s", pointer);
	if (is_owner(node) && !w->file) {
		owner = cj_get_str(node, "uuid");
		pointer[0] = '\0';
		overlay_source = is_overlay_source(node);
		if (overlay_source && w->manifest) {
			const cj *priv = cj_get(node, "private_settings");
			const char *url = cj_get_str(cj_get(node, "settings"), "url");
			if (url && *url && strcmp(url, SHOW_OVERLAY_PLACEHOLDER) != 0) {
				cj *entry = cj_new_object();
				cj_set_str(entry, "source", owner);
				cj_set_str(entry, "slot", cj_get_str(priv, "ctd_overlay_slot"));
				const char *canvas = cj_get_str(priv, "ctd_overlay_canvas");
				cj_set_str(entry, "canvas", canvas && *canvas ? canvas : "landscape");
				if (!cj_append(cj_get_or_add_array(w->manifest, "overlays"), entry))
					w->ok = false;
				if (w->report)
					w->report->overlays++;
			}
		}
	}
	size_t base = strlen(pointer);
	for (size_t i = 0; i < cj_size(node); i++) {
		char index[24];
		const char *name = cj_key_at(node, i);
		if (name && skipped_member(name))
			continue;
		if (!name)
			snprintf(index, sizeof(index), "%zu", i);
		if (!cj_pointer_append(pointer, POINTER_MAX, name ? name : index)) {
			w->ok = false; /* too deep to address: refuse rather than leak */
			continue;
		}
		walk(w, cj_at(node, i), name ? name : member, owner, pointer, depth + 1, overlay_source);
		pointer[base] = '\0';
	}
	snprintf(pointer, POINTER_MAX, "%s", saved);
}

bool show_scrub_collection(cj *collection, cj *manifest, cj *local_secrets, struct show_scrub_report *report)
{
	if (!cj_is_object(collection) || !cj_is_object(local_secrets))
		return false;
	struct walk w = {manifest, local_secrets, NULL, report, true};
	char pointer[POINTER_MAX] = "";
	walk(&w, collection, NULL, NULL, pointer, 0, false);
	return w.ok;
}

bool show_scrub_settings(cj *settings, const char *file, cj *local_secrets, struct show_scrub_report *report)
{
	if (!settings || !file || !cj_is_object(local_secrets))
		return false;
	struct walk w = {NULL, local_secrets, file, report, true};
	char pointer[POINTER_MAX] = "";
	walk(&w, settings, NULL, NULL, pointer, 0, false);
	return w.ok;
}

bool show_scrub_service(cj *service, cj **local_service, struct show_scrub_report *report)
{
	static const char *const keep[] = {"service", "server", "protocol", "bwtest", "use_auth", NULL};
	if (local_service)
		*local_service = NULL;
	if (!cj_is_object(service))
		return false;
	if (local_service && !(*local_service = cj_clone(service)))
		return false;
	/* top level: only "type" and "settings" */
	for (size_t i = 0; i < cj_size(service);) {
		const char *k = cj_key_at(service, i);
		if (strcmp(k, "type") != 0 && strcmp(k, "settings") != 0) {
			cj_remove(service, k);
			continue;
		}
		i++;
	}
	cj *settings = cj_get(service, "settings");
	for (size_t i = 0; cj_is_object(settings) && i < cj_size(settings);) {
		const char *k = cj_key_at(settings, i);
		if (!in_list(k, keep)) {
			const char *v = cj_str(cj_at(settings, i));
			if (report && v && *v && show_scrub_is_secret_name(k))
				report->stream_keys++;
			cj_remove(settings, k);
			continue;
		}
		char *scrubbed = NULL;
		if (cj_is_string(cj_at(settings, i)) && show_scrub_url(cj_str(cj_at(settings, i)), &scrubbed)) {
			cj_set_string_value(cj_at(settings, i), scrubbed);
			free(scrubbed);
			if (report)
				report->stream_keys++;
		}
		i++;
	}
	return true;
}

char *show_scrub_ini(const char *ini)
{
	struct sbuf b = {0};
	sb_put(&b, "", 0);
	for (const char *line = ini ? ini : ""; *line;) {
		size_t n = strcspn(line, "\n");
		size_t end = n < 1 ? n : n;
		const char *eq = memchr(line, '=', n);
		bool drop = false;
		if (eq && line[0] != '[' && line[0] != ';' && line[0] != '#') {
			char key[128];
			size_t kl = (size_t)(eq - line);
			while (kl && (line[kl - 1] == ' ' || line[kl - 1] == '\t'))
				kl--;
			if (kl < sizeof(key)) {
				memcpy(key, line, kl);
				key[kl] = '\0';
				drop = show_scrub_is_secret_name(key);
			}
		}
		if (!drop)
			sb_put(&b, line, end + (line[n] == '\n' ? 1 : 0));
		line += n + (line[n] == '\n' ? 1 : 0);
	}
	if (b.oom) {
		free(b.data);
		return NULL;
	}
	return b.data;
}

/* ---- restore ---- */

cj *show_find_by_uuid(cj *root, const char *uuid)
{
	if (!root || !uuid)
		return NULL;
	if (cj_is_object(root)) {
		const char *u = cj_get_str(root, "uuid");
		if (u && strcmp(u, uuid) == 0)
			return root;
	}
	for (size_t i = 0; i < cj_size(root); i++) {
		cj *found = show_find_by_uuid(cj_at(root, i), uuid);
		if (found)
			return found;
	}
	return NULL;
}

struct restore {
	const cj *local;
	const char *file;
	cj *missing;
	int restored;
};

static void restore_walk(struct restore *r, cj *node, const char *owner, char *pointer, int depth)
{
	if (depth > CJ_MAX_DEPTH)
		return;
	const char *s = cj_str(node);
	if (s) {
		bool overlay = strcmp(s, SHOW_OVERLAY_PLACEHOLDER) == 0;
		if (!overlay && !strstr(s, SHOW_SECRET_PLACEHOLDER))
			return;
		char key[POINTER_MAX + 128];
		snprintf(key, sizeof(key), "%s|%s", r->file ? r->file : (owner ? owner : ""), pointer);
		const char *original = cj_get_str(r->local, key);
		if (original) {
			cj_set_string_value(node, original);
			r->restored++;
		} else if (r->missing) {
			cj *entry = cj_new_object();
			if (owner)
				cj_set_str(entry, "source", owner);
			else
				cj_set(entry, "source", cj_new_null());
			cj_set_str(entry, "key", pointer);
			if (overlay)
				cj_set_bool(entry, "overlay", true);
			cj_append(r->missing, entry);
		}
		return;
	}
	if (!cj_is_object(node) && !cj_is_array(node))
		return;
	char saved[POINTER_MAX];
	snprintf(saved, sizeof(saved), "%s", pointer);
	if (is_owner(node) && !r->file) {
		owner = cj_get_str(node, "uuid");
		pointer[0] = '\0';
	}
	size_t base = strlen(pointer);
	for (size_t i = 0; i < cj_size(node); i++) {
		char index[24];
		const char *name = cj_key_at(node, i);
		if (!name)
			snprintf(index, sizeof(index), "%zu", i);
		if (!cj_pointer_append(pointer, POINTER_MAX, name ? name : index))
			continue;
		restore_walk(r, cj_at(node, i), owner, pointer, depth + 1);
		pointer[base] = '\0';
	}
	snprintf(pointer, POINTER_MAX, "%s", saved);
}

int show_secrets_restore(cj *collection, const cj *local_secrets, cj *missing)
{
	struct restore r = {local_secrets, NULL, missing, 0};
	char pointer[POINTER_MAX] = "";
	restore_walk(&r, collection, NULL, pointer, 0);
	return r.restored;
}

int show_secrets_restore_settings(cj *settings, const char *file, const cj *local_secrets)
{
	struct restore r = {local_secrets, file, NULL, 0};
	char pointer[POINTER_MAX] = "";
	restore_walk(&r, settings, NULL, pointer, 0);
	return r.restored;
}
