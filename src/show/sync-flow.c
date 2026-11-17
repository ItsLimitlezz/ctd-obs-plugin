/* Show Profiles sync flows: save up, pull, conflict answers. See sync-flow.h. */
#include "sync-flow.h"
#include "manifest.h"
#include "sha256.h"
#include "show-fs.h"
#include "store.h"
#include "tar.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PATH_LEN 2048

const char *show_relay_result_text(enum show_relay_result r)
{
	switch (r) {
	case SHOW_RELAY_OK:
		return "OK";
	case SHOW_RELAY_OFFLINE:
		return "CTD Relay can't be reached";
	case SHOW_RELAY_DENIED:
		return "CTD Relay refused this machine's token (pair again to enable Show Profiles)";
	case SHOW_RELAY_NOT_FOUND:
		return "not found on CTD Relay";
	case SHOW_RELAY_BAD:
		return "CTD Relay rejected the request";
	case SHOW_RELAY_REFUSED:
		return "CTD Relay refused it";
	case SHOW_RELAY_LOCAL:
		return "a file on this machine couldn't be read or written";
	case SHOW_RELAY_ABORTED:
		return "stopped";
	}
	return "unknown error";
}

static bool fail(struct show_sync_outcome *out, enum show_relay_result r, const char *fmt, ...)
{
	out->result = r;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(out->error, sizeof(out->error), fmt, ap);
	va_end(ap);
	return false;
}

void show_flow_new_uuid(char out[37])
{
	char hex[33];
	sfs_random_hex(hex);
	hex[12] = '4';                                                               /* version 4 */
	hex[16] = "89ab"[(hex[16] >= 'a' ? hex[16] - 'a' + 10 : hex[16] - '0') & 3]; /* variant */
	snprintf(out, 37, "%.8s-%.4s-%.4s-%.4s-%.12s", hex, hex + 8, hex + 12, hex + 16, hex + 20);
}

bool show_flow_find_linked(const char *relay_profile, char id_out[37])
{
	bool found = false;
	cj *list = show_store_list();
	for (size_t i = 0; relay_profile && *relay_profile && !found && i < cj_size(list); i++) {
		const cj *meta = cj_at(list, i);
		const char *relay = cj_get_str(meta, "relayProfileId");
		if (relay && strcmp(relay, relay_profile) == 0) {
			snprintf(id_out, 37, "%s", cj_get_str(meta, "id"));
			found = true;
		}
	}
	cj_free(list);
	return found;
}

static bool scratch_file(const char *scratch, const char *ext, char *out, size_t size)
{
	char rnd[33], name[64];
	if (!scratch || !*scratch || !sfs_mkdirs(scratch))
		return false;
	sfs_random_hex(rnd);
	snprintf(name, sizeof(name), "sync-%.16s%s", rnd, ext);
	return sfs_join(out, size, scratch, name);
}

static void progress(const struct show_relay_ops *ops, const char *step, uint64_t done, uint64_t total)
{
	if (ops->progress)
		ops->progress(ops->opaque, step, done, total);
}

static const cj *asset_by_sha(const cj *manifest, const char *sha)
{
	const cj *assets = cj_get(manifest, "assets");
	for (size_t i = 0; sha && i < cj_size(assets); i++) {
		const char *have = cj_get_str(cj_at(assets, i), "sha256");
		if (have && strcmp(have, sha) == 0)
			return cj_at(assets, i);
	}
	return NULL;
}

/* PUT every blob Relay says it is missing */
static bool upload_missing(const struct show_relay_ops *ops, const cj *body, const cj *manifest, const char *bundle_sha,
			   const char *bundle_path, struct show_sync_outcome *out)
{
	cj *missing = NULL;
	enum show_relay_result r = ops->check(ops->opaque, body, &missing);
	if (r != SHOW_RELAY_OK) {
		cj_free(missing);
		return fail(out, r, "Couldn't check which files CTD Relay has: %s.", show_relay_result_text(r));
	}
	const size_t n = cj_size(missing);
	for (size_t i = 0; i < n; i++) {
		const char *sha = cj_str(cj_at(missing, i));
		char path[PATH_LEN];
		if (!show_is_sha256(sha))
			continue;
		if (strcmp(sha, bundle_sha) == 0) {
			snprintf(path, sizeof(path), "%s", bundle_path);
		} else {
			const cj *a = asset_by_sha(manifest, sha);
			if (!a || !show_store_asset_path(sha, cj_get_str(a, "name"), path, sizeof(path)) ||
			    !sfs_exists(path)) {
				cj_free(missing);
				return fail(out, SHOW_RELAY_LOCAL, "The file %s isn't on this machine any more.",
					    a ? cj_get_str(a, "name") : sha);
			}
		}
		progress(ops, "Uploading", (uint64_t)i, (uint64_t)n);
		r = ops->put_blob(ops->opaque, sha, show_sync_blob_kind(manifest, sha, bundle_sha), path);
		if (r != SHOW_RELAY_OK) {
			const cj *a = asset_by_sha(manifest, sha);
			cj_free(missing);
			return fail(out, r, "Couldn't upload %s: %s.", a ? cj_get_str(a, "name") : "the bundle",
				    show_relay_result_text(r));
		}
		out->uploaded++;
	}
	progress(ops, "Uploading", (uint64_t)n, (uint64_t)n);
	cj_free(missing);
	return true;
}

bool show_flow_save_up(const struct show_relay_ops *ops, const char *id, const char *relay_profile, int64_t parent,
		       const char *scratch, const char *note, struct show_sync_outcome *out)
{
	memset(out, 0, sizeof(*out));
	out->head = -1;
	char err[512] = "", tar_path[PATH_LEN] = "", bundle_sha[65];
	struct show_bundle b;
	show_bundle_init(&b);
	cj *meta = show_store_meta(id), *check = NULL, *body = NULL, *reply = NULL;
	char *tar = NULL;
	size_t tar_size = 0;
	bool ok = false;

	if (!meta) {
		fail(out, SHOW_RELAY_LOCAL, "That show profile is gone.");
		goto done;
	}
	const char *linked = cj_get_str(meta, "relayProfileId");
	const bool is_linked = linked && *linked;
	const char *relay = is_linked ? linked : relay_profile;
	if (!relay || !*relay) {
		fail(out, SHOW_RELAY_LOCAL, "Pick a CTD Relay profile in the dock first.");
		goto done;
	}
	const int64_t seq = cj_get_int(meta, "headSeq", 0);
	if (seq <= 0) {
		fail(out, SHOW_RELAY_LOCAL, "Nothing is saved in this show profile yet.");
		goto done;
	}
	const int64_t base = parent >= 0 ? parent : cj_get_int(meta, "baseRevision", 0);
	if (parent < 0 && is_linked && !cj_get_bool(meta, "dirty", false) && base > 0) {
		out->no_changes = true;
		out->revision = base;
		out->result = SHOW_RELAY_OK;
		ok = true;
		goto done;
	}
	if (!show_store_load(id, seq, &b, err, sizeof(err))) {
		fail(out, SHOW_RELAY_LOCAL, "Can't read the saved setup: %s", err);
		goto done;
	}
	if (!show_tar_write(&b, &tar, &tar_size) || tar_size > SHOW_BUNDLE_MAX_BYTES + 64 * 512) {
		fail(out, SHOW_RELAY_LOCAL, "The setup is too big for one bundle (16 MB of scene files).");
		goto done;
	}
	sha256_hex(tar, tar_size, bundle_sha);
	if (!scratch_file(scratch, ".tar", tar_path, sizeof(tar_path)) || !sfs_write_atomic(tar_path, tar, tar_size)) {
		fail(out, SHOW_RELAY_LOCAL, "Can't write the bundle to upload.");
		tar_path[0] = '\0';
		goto done;
	}

	check = show_sync_check_body(b.manifest, bundle_sha, (int64_t)tar_size);
	if (!upload_missing(ops, check, b.manifest, bundle_sha, tar_path, out))
		goto done;

	body = show_sync_revision_body(b.manifest, base, relay, bundle_sha, cj_get(b.local, "secrets"), note,
				       &out->secrets_local);
	progress(ops, "Saving", 0, 1);
	enum show_relay_result r = ops->post_revision(ops->opaque, relay, body, &reply);
	if (r == SHOW_RELAY_BAD) {
		/* 400 missing_blobs (a blob pruned between check and save) or invalid: upload again, then once more */
		cj_free(reply);
		reply = NULL;
		if (!upload_missing(ops, check, b.manifest, bundle_sha, tar_path, out))
			goto done;
		r = ops->post_revision(ops->opaque, relay, body, &reply);
	}
	if (r == SHOW_RELAY_REFUSED) {
		/* 409 revision_conflict or 413: the head tells them apart */
		int64_t head = -1;
		if (ops->head(ops->opaque, relay, &head) == SHOW_RELAY_OK && head != base) {
			out->conflict = true;
			out->head = head;
			fail(out, SHOW_RELAY_REFUSED,
			     "%s changed on CTD Relay (revision %lld) since this machine's copy.",
			     cj_get_str(b.manifest, "name"), (long long)head);
			goto done;
		}
		fail(out, r, "CTD Relay refused the save (over a size limit or the storage quota?).");
		goto done;
	}
	if (r != SHOW_RELAY_OK) {
		fail(out, r, "Couldn't save to CTD Relay: %s.", show_relay_result_text(r));
		goto done;
	}
	int64_t revision = cj_get_int(reply, "revision", 0);
	if (revision <= 0)
		revision = base + 1;
	if (!show_store_set_relay_link(id, seq, revision, is_linked ? NULL : relay)) {
		fail(out, SHOW_RELAY_LOCAL, "Saved on CTD Relay as revision %lld, but couldn't note it here.",
		     (long long)revision);
		goto done;
	}
	progress(ops, "Saving", 1, 1);
	out->revision = revision;
	out->head = revision;
	out->result = SHOW_RELAY_OK;
	ok = true;
done:
	if (tar_path[0])
		sfs_remove(tar_path);
	free(tar);
	cj_free(check);
	cj_free(body);
	cj_free(reply);
	cj_free(meta);
	show_bundle_free(&b);
	return ok;
}

struct name_ctx {
	const char *self;
};

static bool name_taken(void *opaque, const char *name)
{
	const struct name_ctx *n = opaque;
	char other[37];
	return show_store_find_by_name(name, other) && (!n->self || strcmp(other, n->self) != 0);
}

static void unique_name(const char *name, const char *self, char *out, size_t size)
{
	struct name_ctx n = {self};
	if (!name_taken(&n, name)) {
		snprintf(out, size, "%s", name);
		return;
	}
	for (int i = 2; i < 1000; i++) {
		char suffix[16];
		snprintf(suffix, sizeof(suffix), " (%d)", i);
		size_t keep = strlen(name);
		if (keep + strlen(suffix) > SHOW_NAME_MAX)
			keep = SHOW_NAME_MAX - strlen(suffix);
		while (keep && ((unsigned char)name[keep] & 0xC0) == 0x80)
			keep--;
		snprintf(out, size, "%.*s%s", (int)keep, name, suffix);
		if (!name_taken(&n, out))
			return;
	}
}

static void choose_local_id(const char *relay, const cj *manifest, const char *given, char id[37])
{
	if (given && show_is_uuid(given)) {
		snprintf(id, 37, "%s", given);
		return;
	}
	if (show_flow_find_linked(relay, id))
		return;
	const char *theirs = cj_get_str(manifest, "showProfileId");
	if (show_is_uuid(theirs)) {
		cj *meta = show_store_meta(theirs);
		const char *linked = cj_get_str(meta, "relayProfileId");
		bool usable = !meta || !linked || !*linked || strcmp(linked, relay) == 0;
		cj_free(meta);
		if (usable) {
			snprintf(id, 37, "%s", theirs);
			return;
		}
	}
	show_flow_new_uuid(id);
}

static bool ensure_parent_dir(const char *path)
{
	char dir[PATH_LEN];
	snprintf(dir, sizeof(dir), "%s", path);
	char *slash = strrchr(dir, '/');
	char *back = strrchr(dir, '\\');
	if (back > slash)
		slash = back;
	if (!slash)
		return true;
	*slash = '\0';
	return sfs_mkdirs(dir);
}

bool show_flow_pull(const struct show_relay_ops *ops, const char *relay_profile, int64_t revision, const char *local_id,
		    const char *scratch, struct show_sync_outcome *out)
{
	memset(out, 0, sizeof(*out));
	out->head = -1;
	char err[512] = "", tar_path[PATH_LEN] = "", got[65] = "", name[SHOW_NAME_MAX + 16];
	struct show_bundle b;
	show_bundle_init(&b);
	cj *item = NULL, *meta = NULL, *secrets = NULL;
	char *data = NULL;
	size_t size = 0;
	bool ok = false, created = false;
	enum show_relay_result r;

	if (!relay_profile || !*relay_profile)
		return fail(out, SHOW_RELAY_LOCAL, "No CTD Relay profile to pull from.");
	if (revision <= 0) {
		int64_t head = 0;
		r = ops->head(ops->opaque, relay_profile, &head);
		if (r != SHOW_RELAY_OK)
			return fail(out, r, "Couldn't ask CTD Relay for the latest revision: %s.",
				    show_relay_result_text(r));
		if (head <= 0)
			return fail(out, SHOW_RELAY_NOT_FOUND, "Nothing is saved on CTD Relay for this profile yet.");
		revision = head;
		out->head = head;
	}
	progress(ops, "Downloading", 0, 1);
	r = ops->get_revision(ops->opaque, relay_profile, revision, &item);
	const cj *remote_manifest = cj_get(item, "manifest");
	if (r != SHOW_RELAY_OK || !cj_is_object(remote_manifest)) {
		fail(out, r == SHOW_RELAY_OK ? SHOW_RELAY_BAD : r, "Couldn't read revision %lld from CTD Relay: %s.",
		     (long long)revision, show_relay_result_text(r == SHOW_RELAY_OK ? SHOW_RELAY_BAD : r));
		goto done;
	}
	const char *bundle_sha = cj_get_str(item, "bundleSha256");
	const int64_t rev = cj_get_int(item, "revision", revision);
	const int64_t parent = cj_get_int(item, "parentRevision", 0);
	if (!show_is_sha256(bundle_sha)) {
		fail(out, SHOW_RELAY_BAD, "CTD Relay's revision %lld names no bundle.", (long long)rev);
		goto done;
	}

	choose_local_id(relay_profile, remote_manifest, local_id, out->id);
	meta = show_store_meta(out->id);
	if (meta && cj_get_int(meta, "baseRevision", 0) == rev && !cj_get_bool(meta, "dirty", false) &&
	    cj_get_int(meta, "headSeq", 0) > 0) {
		out->no_changes = true;
		out->revision = rev;
		out->seq = cj_get_int(meta, "headSeq", 0);
		out->result = SHOW_RELAY_OK;
		ok = true;
		goto done;
	}
	const char *remote_name = cj_get_str(remote_manifest, "name");
	unique_name(show_name_is_valid(remote_name) ? remote_name : "Show profile", out->id, name, sizeof(name));
	if (meta && name_taken(&(struct name_ctx){out->id}, remote_name ? remote_name : ""))
		snprintf(name, sizeof(name), "%s", cj_get_str(meta, "name")); /* keep this machine's name */
	if (!meta) {
		if (!show_store_create(out->id, name, err, sizeof(err))) {
			fail(out, SHOW_RELAY_LOCAL, "Can't make the show profile here: %s", err);
			goto done;
		}
		created = true;
	}

	/* the bundle */
	if (!scratch_file(scratch, ".tar", tar_path, sizeof(tar_path))) {
		fail(out, SHOW_RELAY_LOCAL, "Can't write in the plugin's folder.");
		tar_path[0] = '\0';
		goto done;
	}
	r = ops->get_bundle(ops->opaque, relay_profile, rev, tar_path, got);
	if (r != SHOW_RELAY_OK) {
		fail(out, r, "Couldn't download revision %lld: %s.", (long long)rev, show_relay_result_text(r));
		goto done;
	}
	if (!got[0] && !sha256_file(tar_path, got, NULL, NULL, NULL))
		got[0] = '\0';
	if (strcmp(got, bundle_sha) != 0) {
		fail(out, SHOW_RELAY_BAD, "Revision %lld arrived damaged (checksum); nothing was changed.",
		     (long long)rev);
		goto done;
	}
	if (!sfs_read(tar_path, &data, &size) || !show_tar_read(data, size, &b, err, sizeof(err))) {
		fail(out, SHOW_RELAY_BAD, "Revision %lld can't be read: %s", (long long)rev, err);
		goto done;
	}
	b.manifest = cj_clone(remote_manifest);
	if (!show_sync_prepare_pulled(b.manifest, out->id, relay_profile, rev, parent)) {
		fail(out, SHOW_RELAY_BAD, "Revision %lld has a bad manifest.", (long long)rev);
		goto done;
	}
	cj_set_str(b.manifest, "name", name);
	if (!show_manifest_validate(b.manifest, err, sizeof(err))) {
		fail(out, SHOW_RELAY_BAD, "Revision %lld can't be used here: %s", (long long)rev, err);
		goto done;
	}

	/* the assets this machine doesn't have yet (cached by hash: never fetched twice) */
	const cj *assets = cj_get(b.manifest, "assets");
	const size_t n = cj_size(assets);
	for (size_t i = 0; i < n; i++) {
		const cj *a = cj_at(assets, i);
		const char *sha = cj_get_str(a, "sha256"), *file = cj_get_str(a, "name");
		char path[PATH_LEN], blob_sha[65] = "";
		if (show_store_has_asset(sha, file, cj_get_int(a, "size", -1)))
			continue;
		if (!show_store_asset_path(sha, file, path, sizeof(path)) || !ensure_parent_dir(path)) {
			fail(out, SHOW_RELAY_LOCAL, "Can't write in the asset cache.");
			goto done;
		}
		progress(ops, "Downloading", (uint64_t)i, (uint64_t)n);
		r = ops->get_blob(ops->opaque, sha, path, blob_sha);
		if (r != SHOW_RELAY_OK) {
			fail(out, r, "Couldn't download %s: %s.", file, show_relay_result_text(r));
			goto done;
		}
		if (!blob_sha[0] && !sha256_file(path, blob_sha, NULL, NULL, NULL))
			blob_sha[0] = '\0';
		if (strcmp(blob_sha, sha) != 0) {
			sfs_remove(path);
			fail(out, SHOW_RELAY_BAD, "%s arrived damaged (checksum).", file);
			goto done;
		}
		out->downloaded++;
	}

	/* third-party secrets (show:secrets): without the scope the sources keep their placeholder ("needs re-entry") */
	b.local = cj_new_object();
	cj *local_secrets = cj_set(b.local, "secrets", cj_new_object());
	if (cj_size(cj_get(b.manifest, "secrets")) && ops->get_secrets) {
		r = ops->get_secrets(ops->opaque, relay_profile, rev, &secrets);
		if (r == SHOW_RELAY_OK)
			out->secrets_pulled = show_sync_secrets_to_local(secrets, local_secrets);
	}

	if (!show_store_save(out->id, &b, SHOW_SAVE_RELAY, &out->seq, err, sizeof(err))) {
		fail(out, SHOW_RELAY_LOCAL, "Couldn't keep revision %lld here: %s", (long long)rev, err);
		goto done;
	}
	progress(ops, "Downloading", 1, 1);
	out->revision = rev;
	if (out->head < rev)
		out->head = rev;
	out->result = SHOW_RELAY_OK;
	ok = true;
done:
	if (tar_path[0])
		sfs_remove(tar_path);
	free(data);
	cj_free(item);
	cj_free(meta);
	cj_free(secrets);
	show_bundle_free(&b);
	if (!ok && created)
		show_store_delete(out->id);
	return ok;
}

static bool fork_mine(const char *id, struct show_sync_outcome *out)
{
	char err[512] = "", name[SHOW_NAME_MAX + 16];
	struct show_bundle b;
	cj *meta = show_store_meta(id);
	bool ok = false;
	show_bundle_init(&b);
	if (!meta || !show_store_load(id, 0, &b, err, sizeof(err))) {
		fail(out, SHOW_RELAY_LOCAL, "Can't read this machine's copy: %s", err[0] ? err : "it is gone");
		goto done;
	}
	struct name_ctx n = {NULL};
	show_sync_fork_name(cj_get_str(meta, "name"), name_taken, &n, name, sizeof(name));
	show_flow_new_uuid(out->forked_id);
	if (!show_store_create(out->forked_id, name, err, sizeof(err)) || !show_sync_fork(&b, out->forked_id, name) ||
	    !show_store_save(out->forked_id, &b, SHOW_SAVE_LOCAL, NULL, err, sizeof(err))) {
		show_store_delete(out->forked_id);
		out->forked_id[0] = '\0';
		fail(out, SHOW_RELAY_LOCAL, "Couldn't save your version as a new show profile: %s", err);
		goto done;
	}
	ok = true;
done:
	cj_free(meta);
	show_bundle_free(&b);
	return ok;
}

bool show_flow_resolve(const struct show_relay_ops *ops, const char *id, const char *relay_profile,
		       enum show_conflict_choice choice, int64_t head, const char *scratch,
		       struct show_sync_outcome *out)
{
	struct show_conflict_plan plan;
	char forked[37] = "";
	show_conflict_plan(choice, head, &plan);
	memset(out, 0, sizeof(*out));
	cj *meta = show_store_meta(id);
	char relay[128];
	const char *linked = cj_get_str(meta, "relayProfileId");
	snprintf(relay, sizeof(relay), "%s", linked && *linked ? linked : (relay_profile ? relay_profile : ""));
	cj_free(meta);
	if (plan.fork_mine) {
		if (!fork_mine(id, out))
			return false;
		snprintf(forked, sizeof(forked), "%s", out->forked_id);
	}
	if (plan.pull_theirs) {
		if (!relay[0])
			return fail(out, SHOW_RELAY_LOCAL, "This show profile isn't linked to a CTD Relay profile.");
		bool ok = show_flow_pull(ops, relay, head, id, scratch, out);
		snprintf(out->forked_id, sizeof(out->forked_id), "%s", forked);
		return ok;
	}
	if (plan.upload_mine)
		return show_flow_save_up(ops, id, relay[0] ? relay : NULL, plan.parent, scratch, NULL, out);
	return true;
}
