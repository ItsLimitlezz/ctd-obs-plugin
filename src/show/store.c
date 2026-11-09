/* Local show-profile store (ticket T7, design section 2); see store.h. */
#include "store.h"
#include "manifest.h"
#include "sha256.h"
#include "show-fs.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PATH_MAX_LEN 2048

static char store_root[PATH_MAX_LEN];

static bool set_err(char *err, size_t size, const char *fmt, ...)
{
	if (err && size) {
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(err, size, fmt, ap);
		va_end(ap);
	}
	return false;
}

static bool path_join3(char *out, size_t size, const char *a, const char *b, const char *c)
{
	char tmp[PATH_MAX_LEN];
	return sfs_join(tmp, sizeof(tmp), a, b) && sfs_join(out, size, tmp, c);
}

static bool profile_dir(const char *id, char *out, size_t size)
{
	if (!store_root[0] || !show_is_uuid(id))
		return false;
	return path_join3(out, size, store_root, "show-profiles", id);
}

static bool profile_file(const char *id, const char *name, char *out, size_t size)
{
	char dir[PATH_MAX_LEN];
	return profile_dir(id, dir, sizeof(dir)) && sfs_join(out, size, dir, name);
}

bool show_store_revision_dir(const char *id, int64_t seq, char *out, size_t size)
{
	char dir[PATH_MAX_LEN], name[32];
	if (seq <= 0)
		return false;
	snprintf(name, sizeof(name), "%lld", (long long)seq);
	return profile_dir(id, dir, sizeof(dir)) && path_join3(out, size, dir, "revisions", name);
}

/* ---- open / sweep ---- */

static bool ends_with(const char *s, const char *suffix)
{
	size_t n = strlen(s), m = strlen(suffix);
	return n >= m && strcmp(s + n - m, suffix) == 0;
}

struct sweep {
	const char *dir;
};

static bool sweep_entry(void *opaque, const char *name, bool is_dir)
{
	struct sweep *s = opaque;
	char path[PATH_MAX_LEN];
	if (!sfs_join(path, sizeof(path), s->dir, name))
		return true;
	if (is_dir) {
		if (strncmp(name, ".tmp-", 5) == 0) {
			sfs_remove_tree(path);
		} else {
			struct sweep child = {path};
			sfs_list(path, sweep_entry, &child);
		}
	} else if (ends_with(name, ".tmp") || ends_with(name, ".part")) {
		sfs_remove(path);
	}
	return true;
}

bool show_store_open(const char *root)
{
	char dir[PATH_MAX_LEN];
	if (!root || strlen(root) >= sizeof(store_root))
		return false;
	snprintf(store_root, sizeof(store_root), "%s", root);
	if (!sfs_join(dir, sizeof(dir), store_root, "show-profiles") || !sfs_mkdirs(dir))
		goto fail;
	struct sweep s1 = {dir};
	sfs_list(dir, sweep_entry, &s1);
	if (!sfs_join(dir, sizeof(dir), store_root, "assets") || !sfs_mkdirs(dir))
		goto fail;
	struct sweep s2 = {dir};
	sfs_list(dir, sweep_entry, &s2);
	return true;
fail:
	store_root[0] = '\0';
	return false;
}

void show_store_close(void)
{
	store_root[0] = '\0';
}

const char *show_store_root(void)
{
	return store_root;
}

/* ---- meta ---- */

cj *show_store_meta(const char *id)
{
	char path[PATH_MAX_LEN];
	if (!profile_file(id, "meta.json", path, sizeof(path)))
		return NULL;
	cj *meta = cj_parse_file(path, NULL, 0);
	if (meta && !cj_is_object(meta)) {
		cj_free(meta);
		return NULL;
	}
	return meta;
}

bool show_store_write_meta(const char *id, const cj *meta)
{
	char path[PATH_MAX_LEN];
	return profile_file(id, "meta.json", path, sizeof(path)) && cj_write_file(path, meta, true);
}

static int ascii_casecmp(const char *a, const char *b)
{
	for (;; a++, b++) {
		int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
		if (ca != cb || !ca)
			return ca - cb;
	}
}

struct list_ctx {
	cj *list;
	const char *dir;
};

static bool list_entry(void *opaque, const char *name, bool is_dir)
{
	struct list_ctx *l = opaque;
	if (!is_dir || !show_is_uuid(name))
		return true;
	cj *meta = show_store_meta(name);
	if (meta && cj_get_str(meta, "name"))
		cj_append(l->list, meta);
	else
		cj_free(meta);
	return true;
}

static int compare_meta(const void *a, const void *b)
{
	const cj *ma = *(cj *const *)a, *mb = *(cj *const *)b;
	int c = ascii_casecmp(cj_get_str(ma, "name"), cj_get_str(mb, "name"));
	return c ? c : strcmp(cj_get_str(ma, "id"), cj_get_str(mb, "id"));
}

cj *show_store_list(void)
{
	char dir[PATH_MAX_LEN];
	cj *list = cj_new_array();
	if (!list || !store_root[0] || !sfs_join(dir, sizeof(dir), store_root, "show-profiles"))
		return list;
	struct list_ctx l = {list, dir};
	sfs_list(dir, list_entry, &l);
	/* sort: pull the items out, qsort, put them back */
	size_t n = cj_size(list);
	if (n < 2)
		return list;
	cj **items = malloc(n * sizeof(*items));
	cj *sorted = cj_new_array();
	if (!items || !sorted) {
		free(items);
		cj_free(sorted);
		return list;
	}
	for (size_t i = 0; i < n; i++)
		items[i] = cj_clone(cj_at(list, i));
	qsort(items, n, sizeof(*items), compare_meta);
	for (size_t i = 0; i < n; i++)
		cj_append(sorted, items[i]);
	free(items);
	cj_free(list);
	return sorted;
}

bool show_store_find_by_name(const char *name, char id_out[37])
{
	cj *list = show_store_list();
	bool found = false;
	for (size_t i = 0; i < cj_size(list) && !found; i++) {
		const cj *meta = cj_at(list, i);
		if (ascii_casecmp(cj_get_str(meta, "name"), name) == 0) {
			snprintf(id_out, 37, "%s", cj_get_str(meta, "id"));
			found = true;
		}
	}
	cj_free(list);
	return found;
}

bool show_store_create(const char *id, const char *name, char *err, size_t err_size)
{
	char dir[PATH_MAX_LEN], other[37];
	if (!show_is_uuid(id))
		return set_err(err, err_size, "bad show profile id");
	if (!show_name_is_valid(name))
		return set_err(err, err_size, "\"%s\" can't be a show profile name", name ? name : "");
	if (show_store_find_by_name(name, other))
		return set_err(err, err_size, "A show profile named \"%s\" already exists.", name);
	if (!profile_dir(id, dir, sizeof(dir)) || sfs_exists(dir))
		return set_err(err, err_size, "show profile %s already exists", id);
	char revisions[PATH_MAX_LEN];
	if (!sfs_join(revisions, sizeof(revisions), dir, "revisions") || !sfs_mkdirs(revisions))
		return set_err(err, err_size, "can't create %s", dir);
	cj *meta = cj_new_object();
	cj_set_str(meta, "id", id);
	cj_set_str(meta, "name", name);
	cj_set(meta, "relayProfileId", cj_new_null());
	cj_set_int(meta, "baseRevision", 0);
	cj_set_int(meta, "headSeq", 0);
	cj_set_int(meta, "appliedSeq", 0);
	cj_set_bool(meta, "dirty", false);
	cj_set_str(meta, "savedAt", "");
	cj_set_str(meta, "lastAppliedAt", "");
	cj_set_str(meta, "lastSavedHash", "");
	bool ok = show_store_write_meta(id, meta);
	cj_free(meta);
	if (!ok) {
		sfs_remove_tree(dir);
		return set_err(err, err_size, "can't write %s", dir);
	}
	return true;
}

bool show_store_rename(const char *id, const char *name, char *err, size_t err_size)
{
	char other[37];
	if (!show_name_is_valid(name))
		return set_err(err, err_size, "\"%s\" can't be a show profile name", name ? name : "");
	if (show_store_find_by_name(name, other) && strcmp(other, id) != 0)
		return set_err(err, err_size, "A show profile named \"%s\" already exists.", name);
	cj *meta = show_store_meta(id);
	if (!meta)
		return set_err(err, err_size, "no such show profile");
	cj_set_str(meta, "name", name);
	bool ok = show_store_write_meta(id, meta);
	cj_free(meta);
	return ok || set_err(err, err_size, "can't write the show profile");
}

bool show_store_delete(const char *id)
{
	char dir[PATH_MAX_LEN];
	return profile_dir(id, dir, sizeof(dir)) && sfs_remove_tree(dir);
}

/* ---- assets ---- */

static void asset_ext(const char *name, char ext[12])
{
	ext[0] = '\0';
	const char *dot = name ? strrchr(name, '.') : NULL;
	if (!dot || dot == name)
		return;
	size_t n = strlen(dot + 1);
	if (n == 0 || n > 8)
		return;
	for (size_t i = 0; i < n; i++)
		if (!isalnum((unsigned char)dot[1 + i]))
			return;
	ext[0] = '.';
	for (size_t i = 0; i <= n; i++)
		ext[1 + i] = (char)tolower((unsigned char)dot[1 + i]);
}

bool show_store_asset_path(const char *sha256, const char *name, char *out, size_t size)
{
	char ext[12], sub[3], file[80];
	if (!store_root[0] || !show_is_sha256(sha256))
		return false;
	asset_ext(name, ext);
	sub[0] = sha256[0];
	sub[1] = sha256[1];
	sub[2] = '\0';
	snprintf(file, sizeof(file), "%s%s", sha256, ext);
	char dir[PATH_MAX_LEN];
	return path_join3(dir, sizeof(dir), store_root, "assets", sub) && sfs_join(out, size, dir, file);
}

bool show_store_has_asset(const char *sha256, const char *name, int64_t size)
{
	char path[PATH_MAX_LEN];
	if (!show_store_asset_path(sha256, name, path, sizeof(path)))
		return false;
	int64_t have = sfs_size(path);
	return have >= 0 && (size < 0 || have == size);
}

bool show_store_add_asset(const char *src, const char *sha256, const char *name, char *err, size_t err_size)
{
	char path[PATH_MAX_LEN], part[PATH_MAX_LEN + 8], dir[PATH_MAX_LEN], hex[65];
	if (!show_store_asset_path(sha256, name, path, sizeof(path)))
		return set_err(err, err_size, "bad asset hash");
	if (sfs_exists(path))
		return true;
	snprintf(dir, sizeof(dir), "%s", path);
	char *slash = strrchr(dir, '/');
	if (slash)
		*slash = '\0';
	if (!sfs_mkdirs(dir))
		return set_err(err, err_size, "can't create %s", dir);
	snprintf(part, sizeof(part), "%s.part", path);
	if (!src || !sfs_copy_file(src, part))
		return set_err(err, err_size, "can't copy %s into the asset cache", src ? src : "(no file)");
	if (!sha256_file(part, hex, NULL, NULL, NULL) || strcmp(hex, sha256) != 0) {
		sfs_remove(part);
		return set_err(err, err_size, "%s changed while it was being saved (hash mismatch)", src);
	}
	if (!sfs_rename(part, path)) {
		sfs_remove(part);
		return set_err(err, err_size, "can't place %s in the asset cache", src);
	}
	return true;
}

/* ---- revisions ---- */

struct seq_list {
	int64_t *items;
	size_t count, cap;
};

static bool collect_seq(void *opaque, const char *name, bool is_dir)
{
	struct seq_list *l = opaque;
	if (!is_dir || !*name || strspn(name, "0123456789") != strlen(name) || strlen(name) > 15)
		return true;
	if (l->count == l->cap) {
		size_t cap = l->cap ? l->cap * 2 : 16;
		int64_t *items = realloc(l->items, cap * sizeof(*items));
		if (!items)
			return false;
		l->items = items;
		l->cap = cap;
	}
	l->items[l->count++] = (int64_t)strtoll(name, NULL, 10);
	return true;
}

static int compare_desc(const void *a, const void *b)
{
	int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
	return x < y ? 1 : x > y ? -1 : 0;
}

static struct seq_list list_seqs(const char *id)
{
	struct seq_list l = {0};
	char dir[PATH_MAX_LEN], revisions[PATH_MAX_LEN];
	if (profile_dir(id, dir, sizeof(dir)) && sfs_join(revisions, sizeof(revisions), dir, "revisions"))
		sfs_list(revisions, collect_seq, &l);
	if (l.count > 1)
		qsort(l.items, l.count, sizeof(*l.items), compare_desc);
	return l;
}

cj *show_store_revisions(const char *id)
{
	struct seq_list l = list_seqs(id);
	cj *out = cj_new_array();
	for (size_t i = 0; i < l.count; i++)
		cj_append(out, cj_new_int(l.items[i]));
	free(l.items);
	return out;
}

static void prune(const char *id, int64_t applied)
{
	struct seq_list l = list_seqs(id);
	for (size_t i = SHOW_KEEP_REVISIONS; i < l.count; i++) {
		char dir[PATH_MAX_LEN];
		if (l.items[i] != applied && show_store_revision_dir(id, l.items[i], dir, sizeof(dir)))
			sfs_remove_tree(dir);
	}
	free(l.items);
}

static bool write_text(const char *dir, const char *rel, const void *data, size_t size)
{
	char path[PATH_MAX_LEN], parent[PATH_MAX_LEN];
	if (!sfs_join(path, sizeof(path), dir, rel))
		return false;
	snprintf(parent, sizeof(parent), "%s", path);
	char *slash = strrchr(parent, '/');
	if (slash)
		*slash = '\0';
	return sfs_mkdirs(parent) && sfs_write_atomic(path, data, size);
}

static bool add_checksum(cj *sums, const char *rel, const char *data, size_t size)
{
	char hex[65];
	sha256_hex(data, size, hex);
	return cj_set_str(sums, rel, hex) != NULL;
}

/* {"files": {path: sha256}, "manifest": sha256} -> the content hash: sha256 of the compact "files" object */
static bool files_hash(const cj *checksums, char out[65])
{
	char *text = cj_print(cj_get(checksums, "files"), false);
	if (!text)
		return false;
	sha256_hex(text, strlen(text), out);
	cj_free_str(text);
	return true;
}

static cj *read_checksums(const char *dir)
{
	char path[PATH_MAX_LEN];
	if (!sfs_join(path, sizeof(path), dir, SHOW_FILE_CHECKSUMS))
		return NULL;
	cj *sums = cj_parse_file(path, NULL, 0);
	if (!cj_is_object(sums) || !cj_is_object(cj_get(sums, "files")) || !cj_get_str(sums, "manifest")) {
		cj_free(sums);
		return NULL;
	}
	return sums;
}

bool show_store_bundle_hash(const char *id, int64_t seq, char out[65])
{
	char dir[PATH_MAX_LEN];
	if (!show_store_revision_dir(id, seq, dir, sizeof(dir)))
		return false;
	cj *sums = read_checksums(dir);
	bool ok = sums && files_hash(sums, out);
	cj_free(sums);
	return ok;
}

static bool merge_local(const char *id, const cj *local)
{
	char path[PATH_MAX_LEN];
	if (!local)
		return true;
	if (!profile_file(id, "local.json", path, sizeof(path)))
		return false;
	cj *merged = cj_parse_file(path, NULL, 0);
	if (!cj_is_object(merged)) {
		cj_free(merged);
		merged = cj_new_object();
	}
	const cj *secrets = cj_get(local, "secrets");
	cj *into = cj_get_or_add_object(merged, "secrets");
	for (size_t i = 0; i < cj_size(secrets); i++)
		cj_set(into, cj_key_at(secrets, i), cj_clone(cj_at(secrets, i)));
	const cj *service = cj_get(local, "service");
	if (cj_is_object(service))
		cj_set(merged, "service", cj_clone(service));
	bool ok = cj_write_file(path, merged, true);
	cj_free(merged);
	return ok;
}

static bool cache_assets(const struct show_bundle *b, char *err, size_t err_size)
{
	const cj *assets = cj_get(b->manifest, "assets");
	for (size_t i = 0; i < cj_size(assets); i++) {
		const cj *a = cj_at(assets, i);
		const char *sha = cj_get_str(a, "sha256"), *name = cj_get_str(a, "name");
		if (show_store_has_asset(sha, name, cj_get_int(a, "size", -1)))
			continue;
		const char *src = cj_get_str(b->asset_sources, sha);
		if (!src)
			return set_err(err, err_size, "asset %s (%s) is not in the cache and has no source file", name,
				       sha);
		if (!show_store_add_asset(src, sha, name, err, err_size))
			return false;
	}
	return true;
}

bool show_store_save(const char *id, struct show_bundle *b, enum show_save_origin origin, int64_t *seq_out, char *err,
		     size_t err_size)
{
	char dir[PATH_MAX_LEN], revisions[PATH_MAX_LEN], tmp[PATH_MAX_LEN], final_dir[PATH_MAX_LEN], rnd[33];
	cj *meta = show_store_meta(id);
	cj *sums = NULL;
	char *text = NULL;
	bool ok = false;
	if (seq_out)
		*seq_out = 0;
	if (!meta)
		return set_err(err, err_size, "no such show profile");
	if (!b->manifest || !b->collection) {
		set_err(err, err_size, "the bundle has no manifest or collection");
		goto done;
	}
	if (!show_manifest_validate(b->manifest, err, err_size))
		goto done;
	if (strcmp(cj_get_str(b->manifest, "showProfileId"), id) != 0) {
		set_err(err, err_size, "the bundle belongs to another show profile");
		goto done;
	}
	if (!cache_assets(b, err, err_size))
		goto done;

	profile_dir(id, dir, sizeof(dir));
	sfs_join(revisions, sizeof(revisions), dir, "revisions");
	sfs_random_hex(rnd);
	char tmp_name[48];
	snprintf(tmp_name, sizeof(tmp_name), ".tmp-%.12s", rnd);
	if (!sfs_join(tmp, sizeof(tmp), revisions, tmp_name) || !sfs_mkdirs(tmp)) {
		set_err(err, err_size, "can't write in %s", revisions);
		goto done;
	}

	sums = cj_new_object();
	cj *files = cj_set(sums, "files", cj_new_object());
	text = cj_print(b->collection, true);
	if (!text || !write_text(tmp, SHOW_FILE_COLLECTION, text, strlen(text)) ||
	    !add_checksum(files, SHOW_FILE_COLLECTION, text, strlen(text)))
		goto write_failed;
	cj_free_str(text);
	text = NULL;
	for (size_t i = 0; i < b->file_count; i++) {
		const struct show_file *f = &b->files[i];
		if (!show_path_is_safe(f->path) || strcmp(f->path, SHOW_FILE_MANIFEST) == 0 ||
		    strcmp(f->path, SHOW_FILE_CHECKSUMS) == 0 || strcmp(f->path, SHOW_FILE_COLLECTION) == 0) {
			set_err(err, err_size, "bad bundle file %s", f->path);
			goto cleanup_tmp;
		}
		if (!write_text(tmp, f->path, f->data, f->size) || !add_checksum(files, f->path, f->data, f->size))
			goto write_failed;
	}

	/* identical content to the head: nothing new to keep */
	int64_t head = cj_get_int(meta, "headSeq", 0);
	char new_hash[65], head_hash[65];
	if (!files_hash(sums, new_hash))
		goto write_failed;
	if (origin == SHOW_SAVE_LOCAL && head > 0 && show_store_bundle_hash(id, head, head_hash) &&
	    strcmp(new_hash, head_hash) == 0) {
		sfs_remove_tree(tmp);
		if (!merge_local(id, b->local)) {
			set_err(err, err_size, "can't write this machine's secrets");
			goto done;
		}
		if (seq_out)
			*seq_out = head;
		ok = true;
		goto done;
	}

	text = cj_print(b->manifest, true);
	if (!text || !write_text(tmp, SHOW_FILE_MANIFEST, text, strlen(text)))
		goto write_failed;
	char manifest_hash[65];
	sha256_hex(text, strlen(text), manifest_hash);
	cj_set_str(sums, "manifest", manifest_hash);
	cj_free_str(text);
	text = cj_print(sums, true);
	if (!text || !write_text(tmp, SHOW_FILE_CHECKSUMS, text, strlen(text)))
		goto write_failed;

	/* the local half first: a revision must never be in place without this machine's secrets for it */
	if (!merge_local(id, b->local)) {
		set_err(err, err_size, "can't write this machine's secrets");
		goto cleanup_tmp;
	}
	int64_t seq = head + 1;
	while (show_store_revision_dir(id, seq, final_dir, sizeof(final_dir)) && sfs_exists(final_dir))
		seq++;
	if (!sfs_rename(tmp, final_dir)) {
		set_err(err, err_size, "can't place the new revision in %s", revisions);
		goto cleanup_tmp;
	}

	cj_set_int(meta, "headSeq", seq);
	cj_set_str(meta, "name", cj_get_str(b->manifest, "name"));
	cj_set_str(meta, "savedAt", cj_get_str(b->manifest, "savedAt") ? cj_get_str(b->manifest, "savedAt") : "");
	cj_set_str(meta, "lastSavedHash", new_hash);
	const cj *relay = cj_get(b->manifest, "relayProfileId");
	cj_set(meta, "relayProfileId", relay ? cj_clone(relay) : cj_new_null());
	if (origin == SHOW_SAVE_RELAY) {
		cj_set_int(meta, "baseRevision", cj_get_int(b->manifest, "revision", 0));
		cj_set_bool(meta, "dirty", false);
	} else {
		cj_set_bool(meta, "dirty", true);
	}
	if (!show_store_write_meta(id, meta)) {
		set_err(err, err_size, "can't write the show profile's meta.json");
		goto done;
	}
	prune(id, cj_get_int(meta, "appliedSeq", 0));
	if (seq_out)
		*seq_out = seq;
	ok = true;
	goto done;

write_failed:
	set_err(err, err_size, "can't write the bundle (disk full?)");
cleanup_tmp:
	sfs_remove_tree(tmp);
done:
	cj_free_str(text);
	cj_free(sums);
	cj_free(meta);
	return ok;
}

bool show_store_load(const char *id, int64_t seq, struct show_bundle *b, char *err, size_t err_size)
{
	char dir[PATH_MAX_LEN], path[PATH_MAX_LEN], hex[65];
	cj *sums = NULL;
	char *data = NULL;
	size_t size = 0;
	bool ok = false;
	show_bundle_init(b);
	if (seq <= 0) {
		cj *meta = show_store_meta(id);
		seq = cj_get_int(meta, "headSeq", 0);
		cj_free(meta);
	}
	if (!show_store_revision_dir(id, seq, dir, sizeof(dir)) || !sfs_is_dir(dir))
		return set_err(err, err_size, "revision %lld of this show profile isn't on this machine",
			       (long long)seq);
	sums = read_checksums(dir);
	if (!sums) {
		set_err(err, err_size, "revision %lld is damaged (no checksums)", (long long)seq);
		goto done;
	}

	/* the manifest */
	if (!sfs_join(path, sizeof(path), dir, SHOW_FILE_MANIFEST) || !sfs_read(path, &data, &size)) {
		set_err(err, err_size, "revision %lld is damaged (no manifest)", (long long)seq);
		goto done;
	}
	sha256_hex(data, size, hex);
	if (strcmp(hex, cj_get_str(sums, "manifest")) != 0) {
		set_err(err, err_size, "revision %lld is damaged (manifest checksum)", (long long)seq);
		goto done;
	}
	b->manifest = cj_parse(data, size, err, err_size);
	free(data);
	data = NULL;
	if (!b->manifest || !show_manifest_validate(b->manifest, err, err_size))
		goto done;
	if (strcmp(cj_get_str(b->manifest, "showProfileId"), id) != 0) {
		set_err(err, err_size, "revision %lld belongs to another show profile", (long long)seq);
		goto done;
	}

	/* every file, checked */
	const cj *files = cj_get(sums, "files");
	for (size_t i = 0; i < cj_size(files); i++) {
		const char *rel = cj_key_at(files, i);
		if (!show_path_is_safe(rel) || !sfs_join(path, sizeof(path), dir, rel) ||
		    !sfs_read(path, &data, &size)) {
			set_err(err, err_size, "revision %lld is damaged (%s missing)", (long long)seq, rel);
			goto done;
		}
		sha256_hex(data, size, hex);
		if (!cj_str(cj_at(files, i)) || strcmp(hex, cj_str(cj_at(files, i))) != 0) {
			set_err(err, err_size, "revision %lld is damaged (%s checksum)", (long long)seq, rel);
			goto done;
		}
		if (strcmp(rel, SHOW_FILE_COLLECTION) == 0) {
			b->collection = cj_parse(data, size, err, err_size);
			if (!cj_is_object(b->collection)) {
				set_err(err, err_size, "revision %lld: collection.json is not a scene collection",
					(long long)seq);
				goto done;
			}
		} else if (!show_bundle_set_file(b, rel, data, size)) {
			set_err(err, err_size, "out of memory");
			goto done;
		}
		free(data);
		data = NULL;
	}
	const char *collection_file = cj_get_str(cj_get(b->manifest, "files"), "collection");
	if (!b->collection || !collection_file || strcmp(collection_file, SHOW_FILE_COLLECTION) != 0) {
		set_err(err, err_size, "revision %lld has no collection", (long long)seq);
		goto done;
	}

	/* this machine's half */
	if (profile_file(id, "local.json", path, sizeof(path)))
		b->local = cj_parse_file(path, NULL, 0);
	if (!cj_is_object(b->local)) {
		cj_free(b->local);
		b->local = cj_new_object();
	}
	cj_get_or_add_object(b->local, "secrets");
	ok = true;
done:
	free(data);
	cj_free(sums);
	if (!ok)
		show_bundle_free(b);
	return ok;
}

bool show_store_set_relay_revision(const char *id, int64_t seq, int64_t revision)
{
	char dir[PATH_MAX_LEN], path[PATH_MAX_LEN], sums_path[PATH_MAX_LEN], hex[65];
	if (!show_store_revision_dir(id, seq, dir, sizeof(dir)) ||
	    !sfs_join(path, sizeof(path), dir, SHOW_FILE_MANIFEST) ||
	    !sfs_join(sums_path, sizeof(sums_path), dir, SHOW_FILE_CHECKSUMS))
		return false;
	cj *manifest = cj_parse_file(path, NULL, 0);
	cj *sums = read_checksums(dir);
	bool ok = false;
	if (manifest && sums) {
		cj_set_int(manifest, "revision", revision);
		char *text = cj_print(manifest, true);
		if (text && sfs_write_atomic(path, text, strlen(text))) {
			sha256_hex(text, strlen(text), hex);
			cj_set_str(sums, "manifest", hex);
			ok = cj_write_file(sums_path, sums, true);
		}
		cj_free_str(text);
	}
	cj_free(manifest);
	cj_free(sums);
	if (!ok)
		return false;
	cj *meta = show_store_meta(id);
	if (!meta)
		return false;
	cj_set_int(meta, "baseRevision", revision);
	if (cj_get_int(meta, "headSeq", 0) == seq)
		cj_set_bool(meta, "dirty", false);
	ok = show_store_write_meta(id, meta);
	cj_free(meta);
	return ok;
}

/* ---- device maps (M4) ---- */

static bool machine_ok(const char *machine)
{
	if (!machine || !*machine || strlen(machine) > 63)
		return false;
	for (const char *p = machine; *p; p++)
		if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_')
			return false;
	return true;
}

cj *show_store_device_map(const char *id, const char *machine)
{
	char name[96], path[PATH_MAX_LEN];
	if (!machine_ok(machine))
		return NULL;
	snprintf(name, sizeof(name), "device-map.%s.json", machine);
	if (!profile_file(id, name, path, sizeof(path)))
		return NULL;
	cj *map = cj_parse_file(path, NULL, 0);
	if (map && !cj_is_object(map)) {
		cj_free(map);
		return NULL;
	}
	return map;
}

bool show_store_write_device_map(const char *id, const char *machine, const cj *map)
{
	char name[96], path[PATH_MAX_LEN];
	if (!machine_ok(machine) || !cj_is_object(map))
		return false;
	snprintf(name, sizeof(name), "device-map.%s.json", machine);
	return profile_file(id, name, path, sizeof(path)) && cj_write_file(path, map, true);
}
