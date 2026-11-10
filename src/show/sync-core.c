/* Show Profiles sync, the pure part. See sync-core.h. */
#include "sync-core.h"
#include "manifest.h"
#include "show-fs.h"
#include "store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QUEUE_FILE "show-sync.json"
#define PATH_LEN 2048

enum show_sync_status show_sync_classify(bool linked, int64_t base, bool dirty, int64_t remote_head)
{
	if (!linked)
		return SHOW_SYNC_LOCAL_ONLY;
	if (remote_head < 0)
		return SHOW_SYNC_UNKNOWN;
	bool newer = remote_head > base;
	if (dirty && newer)
		return SHOW_SYNC_CONFLICT;
	if (dirty)
		return SHOW_SYNC_NOT_BACKED_UP;
	if (newer)
		return SHOW_SYNC_UPDATE_READY;
	return SHOW_SYNC_UP_TO_DATE;
}

const char *show_sync_status_name(enum show_sync_status status)
{
	switch (status) {
	case SHOW_SYNC_LOCAL_ONLY:
		return "local-only";
	case SHOW_SYNC_UNKNOWN:
		return "unknown";
	case SHOW_SYNC_UP_TO_DATE:
		return "up-to-date";
	case SHOW_SYNC_NOT_BACKED_UP:
		return "not-backed-up";
	case SHOW_SYNC_UPDATE_READY:
		return "update-ready";
	case SHOW_SYNC_CONFLICT:
		return "conflict";
	}
	return "unknown";
}

void show_conflict_plan(enum show_conflict_choice choice, int64_t head, struct show_conflict_plan *out)
{
	memset(out, 0, sizeof(*out));
	switch (choice) {
	case SHOW_CONFLICT_KEEP_MINE:
		out->upload_mine = true;
		out->parent = head;
		break;
	case SHOW_CONFLICT_TAKE_THEIRS:
		out->pull_theirs = true;
		break;
	case SHOW_CONFLICT_SAVE_AS_NEW:
		out->fork_mine = true;
		out->pull_theirs = true;
		break;
	}
}

/* ------------------------------------------------------------ bodies --- */

static void check_item(cj *items, const char *sha, int64_t size)
{
	cj *item = cj_append(items, cj_new_object());
	cj_set_str(item, "sha256", sha);
	cj_set_int(item, "size", size);
}

cj *show_sync_check_body(const cj *manifest, const char *bundle_sha256, int64_t bundle_size)
{
	cj *body = cj_new_object();
	cj *items = cj_set(body, "items", cj_new_array());
	if (!items) {
		cj_free(body);
		return NULL;
	}
	if (bundle_sha256 && show_is_sha256(bundle_sha256))
		check_item(items, bundle_sha256, bundle_size);
	const cj *assets = cj_get(manifest, "assets");
	for (size_t i = 0; i < cj_size(assets); i++) {
		const cj *a = cj_at(assets, i);
		const char *sha = cj_get_str(a, "sha256");
		if (show_is_sha256(sha) && (!bundle_sha256 || strcmp(sha, bundle_sha256) != 0))
			check_item(items, sha, cj_get_int(a, "size", 0));
	}
	return body;
}

const char *show_sync_blob_kind(const cj *manifest, const char *sha256, const char *bundle_sha256)
{
	if (sha256 && bundle_sha256 && strcmp(sha256, bundle_sha256) == 0)
		return "bundle";
	const cj *assets = cj_get(manifest, "assets");
	for (size_t i = 0; sha256 && i < cj_size(assets); i++) {
		const cj *a = cj_at(assets, i);
		if (strcmp(cj_get_str(a, "sha256") ? cj_get_str(a, "sha256") : "", sha256) == 0) {
			const char *kind = cj_get_str(a, "kind");
			return kind ? kind : "other";
		}
	}
	return "other";
}

cj *show_sync_revision_body(const cj *manifest, int64_t parent, const char *relay_profile_id, const char *bundle_sha256,
			    const cj *local_secrets, const char *note, int *kept_local)
{
	if (kept_local)
		*kept_local = 0;
	if (!cj_is_object(manifest) || !bundle_sha256)
		return NULL;
	cj *body = cj_new_object();
	cj *m = cj_clone(manifest);
	if (!body || !m) {
		cj_free(body);
		cj_free(m);
		return NULL;
	}
	cj_set_int(m, "revision", parent + 1);
	if (parent > 0)
		cj_set_int(m, "parentRevision", parent);
	else
		cj_set(m, "parentRevision", cj_new_null());
	if (relay_profile_id && *relay_profile_id)
		cj_set_str(m, "relayProfileId", relay_profile_id);

	cj_set_int(body, "parentRevision", parent);
	cj_set_str(body, "bundleSha256", bundle_sha256);
	if (note && *note)
		cj_set_str(body, "note", note);

	cj *secrets = cj_new_array();
	const cj *wanted = cj_get(manifest, "secrets");
	for (size_t i = 0; i < cj_size(wanted); i++) {
		const cj *w = cj_at(wanted, i);
		const char *source = cj_get_str(w, "source"), *key = cj_get_str(w, "key");
		if (!key)
			continue;
		if (!source || !*source || strlen(source) > 64 || strlen(key) > 128) {
			if (kept_local)
				(*kept_local)++;
			continue;
		}
		char local_key[256];
		snprintf(local_key, sizeof(local_key), "%s|%s", source, key);
		const char *value = cj_get_str(local_secrets, local_key);
		if (!value || strlen(value) > 8192) {
			if (kept_local && value)
				(*kept_local)++;
			continue;
		}
		cj *s = cj_append(secrets, cj_new_object());
		cj_set_str(s, "source", source);
		cj_set_str(s, "key", key);
		cj_set_str(s, "value", value);
	}
	cj_set(body, "manifest", m);
	if (cj_size(secrets))
		cj_set(body, "secrets", secrets);
	else
		cj_free(secrets);
	return body;
}

int show_sync_secrets_to_local(const cj *items, cj *local_secrets)
{
	int taken = 0;
	if (!cj_is_object(local_secrets))
		return 0;
	for (size_t i = 0; i < cj_size(items); i++) {
		const cj *s = cj_at(items, i);
		const char *source = cj_get_str(s, "source"), *key = cj_get_str(s, "key"),
			   *value = cj_get_str(s, "value");
		if (!source || !*source || !key || !*key || !value)
			continue;
		char local_key[256];
		snprintf(local_key, sizeof(local_key), "%s|%s", source, key);
		if (cj_set_str(local_secrets, local_key, value))
			taken++;
	}
	return taken;
}

bool show_sync_prepare_pulled(cj *manifest, const char *local_id, const char *relay_profile_id, int64_t revision,
			      int64_t parent)
{
	if (!cj_is_object(manifest) || !show_is_uuid(local_id) || revision <= 0)
		return false;
	cj_set_str(manifest, "showProfileId", local_id);
	if (relay_profile_id && *relay_profile_id)
		cj_set_str(manifest, "relayProfileId", relay_profile_id);
	cj_set_int(manifest, "revision", revision);
	if (parent > 0 && parent < revision)
		cj_set_int(manifest, "parentRevision", parent);
	else
		cj_set(manifest, "parentRevision", cj_new_null());
	return true;
}

bool show_sync_fork(struct show_bundle *b, const char *new_id, const char *new_name)
{
	if (!b || !cj_is_object(b->manifest) || !show_is_uuid(new_id) || !show_name_is_valid(new_name))
		return false;
	cj_set_str(b->manifest, "showProfileId", new_id);
	cj_set_str(b->manifest, "name", new_name);
	cj_set(b->manifest, "relayProfileId", cj_new_null());
	cj_set_int(b->manifest, "revision", 0);
	cj_set(b->manifest, "parentRevision", cj_new_null());
	/* the tie to the old profile (only present in collections saved by OBS) */
	cj *modules = cj_get(b->collection, "modules");
	if (cj_is_object(modules))
		cj_remove(modules, "ctd-show-profile");
	return true;
}

/* back off to a UTF-8 boundary */
static size_t utf8_cut(const char *s, size_t max)
{
	size_t n = strlen(s);
	if (n <= max)
		return n;
	while (max > 0 && ((unsigned char)s[max] & 0xC0) == 0x80)
		max--;
	return max;
}

void show_sync_fork_name(const char *name, bool (*exists)(void *opaque, const char *name), void *opaque, char *out,
			 size_t size)
{
	char suffix[24];
	for (int n = 1; n < 1000; n++) {
		if (n == 1)
			snprintf(suffix, sizeof(suffix), " (mine)");
		else
			snprintf(suffix, sizeof(suffix), " (mine %d)", n);
		size_t room = SHOW_NAME_MAX - strlen(suffix);
		size_t keep = utf8_cut(name ? name : "", room);
		while (keep && name[keep - 1] == ' ')
			keep--;
		snprintf(out, size, "%.*s%s", (int)keep, name ? name : "", suffix);
		if (!exists || !exists(opaque, out))
			return;
	}
}

/* ------------------------------------------------------------- queue --- */

static bool queue_path(char *out, size_t size)
{
	const char *root = show_store_root();
	return root && *root && sfs_join(out, size, root, QUEUE_FILE);
}

static cj *queue_load(char *path, size_t size)
{
	if (!queue_path(path, size))
		return NULL;
	cj *doc = cj_parse_file(path, NULL, 0);
	if (!cj_is_object(doc)) {
		cj_free(doc);
		doc = cj_new_object();
	}
	cj_get_or_add_object(doc, "queue");
	return doc;
}

bool show_sync_queue_add(const char *id, int64_t now_s, const char *error)
{
	char path[PATH_LEN];
	if (!show_is_uuid(id))
		return false;
	cj *doc = queue_load(path, sizeof(path));
	if (!doc)
		return false;
	cj *queue = cj_get(doc, "queue");
	cj *entry = cj_get(queue, id);
	if (!cj_is_object(entry)) {
		entry = cj_set(queue, id, cj_new_object());
		cj_set_int(entry, "queuedAt", now_s);
		cj_set_int(entry, "attempts", 0);
	}
	int64_t attempts = cj_get_int(entry, "attempts", 0) + 1;
	cj_set_int(entry, "attempts", attempts);
	cj_set_int(entry, "nextAt", now_s + show_sync_retry_delay_s((int)attempts));
	cj_set_str(entry, "lastError", error ? error : "");
	bool ok = cj_write_file(path, doc, true);
	cj_free(doc);
	return ok;
}

bool show_sync_queue_remove(const char *id)
{
	char path[PATH_LEN];
	cj *doc = queue_load(path, sizeof(path));
	if (!doc)
		return false;
	bool ok = true;
	if (cj_remove(cj_get(doc, "queue"), id))
		ok = cj_write_file(path, doc, true);
	cj_free(doc);
	return ok;
}

bool show_sync_queue_has(const char *id)
{
	char path[PATH_LEN];
	cj *doc = queue_load(path, sizeof(path));
	bool has = id && cj_get(cj_get(doc, "queue"), id) != NULL;
	cj_free(doc);
	return has;
}

static int compare_queued(const void *a, const void *b)
{
	const int64_t x = cj_get_int(*(const cj *const *)a, "queuedAt", 0);
	const int64_t y = cj_get_int(*(const cj *const *)b, "queuedAt", 0);
	return x < y ? -1 : x > y ? 1 : 0;
}

cj *show_sync_queue_due(int64_t now_s)
{
	char path[PATH_LEN];
	cj *out = cj_new_array();
	cj *doc = queue_load(path, sizeof(path));
	const cj *queue = cj_get(doc, "queue");
	size_t n = cj_size(queue);
	const cj **due = n ? calloc(n, sizeof(*due)) : NULL;
	size_t count = 0;
	for (size_t i = 0; due && i < n; i++) {
		const cj *e = cj_at(queue, i);
		if (show_is_uuid(cj_key_at(queue, i)) && cj_get_int(e, "nextAt", 0) <= now_s) {
			due[count++] = e;
		}
	}
	if (count)
		qsort(due, count, sizeof(*due), compare_queued);
	for (size_t i = 0; i < count; i++)
		for (size_t j = 0; j < n; j++)
			if (cj_at(queue, j) == due[i])
				cj_append(out, cj_new_string(cj_key_at(queue, j)));
	free(due);
	cj_free(doc);
	return out;
}

int64_t show_sync_retry_delay_s(int attempts)
{
	int64_t delay = 15;
	for (int i = 1; i < attempts && delay < 600; i++)
		delay *= 2;
	return delay > 600 ? 600 : delay;
}
