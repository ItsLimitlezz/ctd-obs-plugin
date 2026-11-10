/* Sync (T11): the bundle tar, classification, request bodies, the offline queue, and the flows against an in-memory
 * Relay that behaves like API v1.9: save up -> delete the local copy -> pull gives the same setup back; a stale parent
 * is a conflict, and Keep mine / Take theirs / Save as new each keep every version. */
#include "unit.h"
#include "manifest.h"
#include "sha256.h"
#include "show-fs.h"
#include "store.h"
#include "sync-core.h"
#include "sync-flow.h"
#include "tar.h"

#include <stdlib.h>

#define ID "0f8fad5b-d9cb-469f-a165-70867728950e"
#define RELAY_PROFILE "default"
#define SECRET_SOURCE "a1b2c3d4-0000-4000-8000-0000000000aa"
#define SECRET_VALUE "https://widgets.example.invalid/w?token=fixture-secret-123"

/* ------------------------------------------------------- fake Relay --- */

struct fake_relay {
	char blobs[1100]; /* folder: <sha256> files */
	cj *revisions;    /* array of {revision, parentRevision, bundleSha256, manifest, secrets} */
	int64_t head;
	bool offline;
	bool no_secrets_scope;
	int puts, posts;
};

static void blob_path(struct fake_relay *f, const char *sha, char *out, size_t size)
{
	sfs_join(out, size, f->blobs, sha);
}

static bool has_blob(struct fake_relay *f, const char *sha)
{
	char path[1300];
	blob_path(f, sha, path, sizeof(path));
	return sfs_exists(path);
}

static enum show_relay_result f_check(void *opaque, const cj *body, cj **missing)
{
	struct fake_relay *f = opaque;
	if (f->offline)
		return SHOW_RELAY_OFFLINE;
	*missing = cj_new_array();
	const cj *items = cj_get(body, "items");
	for (size_t i = 0; i < cj_size(items); i++) {
		const char *sha = cj_get_str(cj_at(items, i), "sha256");
		if (!has_blob(f, sha))
			cj_append(*missing, cj_new_string(sha));
	}
	return SHOW_RELAY_OK;
}

static enum show_relay_result f_put(void *opaque, const char *sha, const char *kind, const char *path)
{
	struct fake_relay *f = opaque;
	char got[65], dest[1300];
	(void)kind;
	if (f->offline)
		return SHOW_RELAY_OFFLINE;
	if (!sha256_file(path, got, NULL, NULL, NULL) || strcmp(got, sha) != 0)
		return SHOW_RELAY_BAD; /* hash_mismatch */
	blob_path(f, sha, dest, sizeof(dest));
	f->puts++;
	return sfs_copy_file(path, dest) ? SHOW_RELAY_OK : SHOW_RELAY_LOCAL;
}

static cj *item_of(const cj *rev, bool with_manifest)
{
	cj *item = cj_new_object();
	cj_set_str(item, "profileId", RELAY_PROFILE);
	cj_set_int(item, "revision", cj_get_int(rev, "revision", 0));
	const cj *parent = cj_get(rev, "parentRevision");
	cj_set(item, "parentRevision", parent ? cj_clone(parent) : cj_new_null());
	cj_set_str(item, "bundleSha256", cj_get_str(rev, "bundleSha256"));
	if (with_manifest)
		cj_set(item, "manifest", cj_clone(cj_get(rev, "manifest")));
	return item;
}

static enum show_relay_result f_post(void *opaque, const char *profile, const cj *body, cj **reply)
{
	struct fake_relay *f = opaque;
	if (f->offline)
		return SHOW_RELAY_OFFLINE;
	if (strcmp(profile, RELAY_PROFILE) != 0)
		return SHOW_RELAY_NOT_FOUND;
	f->posts++;
	if (cj_get_int(body, "parentRevision", -1) != f->head)
		return SHOW_RELAY_REFUSED; /* 409 revision_conflict */
	const cj *manifest = cj_get(body, "manifest");
	if (!has_blob(f, cj_get_str(body, "bundleSha256")))
		return SHOW_RELAY_BAD; /* missing_blobs */
	const cj *assets = cj_get(manifest, "assets");
	for (size_t i = 0; i < cj_size(assets); i++)
		if (!has_blob(f, cj_get_str(cj_at(assets, i), "sha256")))
			return SHOW_RELAY_BAD;
	cj *rev = cj_append(f->revisions, cj_new_object());
	cj_set_int(rev, "revision", ++f->head);
	if (f->head > 1)
		cj_set_int(rev, "parentRevision", f->head - 1);
	else
		cj_set(rev, "parentRevision", cj_new_null());
	cj_set_str(rev, "bundleSha256", cj_get_str(body, "bundleSha256"));
	cj_set(rev, "manifest", cj_clone(manifest));
	cj_set(rev, "secrets", cj_get(body, "secrets") ? cj_clone(cj_get(body, "secrets")) : cj_new_array());
	*reply = item_of(rev, false);
	return SHOW_RELAY_OK;
}

static enum show_relay_result f_head(void *opaque, const char *profile, int64_t *head)
{
	struct fake_relay *f = opaque;
	(void)profile;
	if (f->offline)
		return SHOW_RELAY_OFFLINE;
	*head = f->head;
	return SHOW_RELAY_OK;
}

static const cj *find_rev(struct fake_relay *f, int64_t revision)
{
	for (size_t i = 0; i < cj_size(f->revisions); i++)
		if (cj_get_int(cj_at(f->revisions, i), "revision", 0) == revision)
			return cj_at(f->revisions, i);
	return NULL;
}

static enum show_relay_result f_get_revision(void *opaque, const char *profile, int64_t revision, cj **item)
{
	struct fake_relay *f = opaque;
	(void)profile;
	if (f->offline)
		return SHOW_RELAY_OFFLINE;
	const cj *rev = find_rev(f, revision);
	if (!rev)
		return SHOW_RELAY_NOT_FOUND;
	*item = item_of(rev, true);
	return SHOW_RELAY_OK;
}

static enum show_relay_result f_get_blob(void *opaque, const char *sha, const char *path, char sha_out[65])
{
	struct fake_relay *f = opaque;
	char src[1300];
	if (f->offline)
		return SHOW_RELAY_OFFLINE;
	blob_path(f, sha, src, sizeof(src));
	if (!sfs_exists(src))
		return SHOW_RELAY_NOT_FOUND;
	if (!sfs_copy_file(src, path))
		return SHOW_RELAY_LOCAL;
	return sha256_file(path, sha_out, NULL, NULL, NULL) ? SHOW_RELAY_OK : SHOW_RELAY_LOCAL;
}

static enum show_relay_result f_get_bundle(void *opaque, const char *profile, int64_t revision, const char *path,
					   char sha_out[65])
{
	struct fake_relay *f = opaque;
	(void)profile;
	const cj *rev = find_rev(f, revision);
	if (!rev)
		return f->offline ? SHOW_RELAY_OFFLINE : SHOW_RELAY_NOT_FOUND;
	return f_get_blob(opaque, cj_get_str(rev, "bundleSha256"), path, sha_out);
}

static enum show_relay_result f_get_secrets(void *opaque, const char *profile, int64_t revision, cj **items)
{
	struct fake_relay *f = opaque;
	(void)profile;
	if (f->offline)
		return SHOW_RELAY_OFFLINE;
	if (f->no_secrets_scope)
		return SHOW_RELAY_DENIED;
	const cj *rev = find_rev(f, revision);
	if (!rev)
		return SHOW_RELAY_NOT_FOUND;
	*items = cj_clone(cj_get(rev, "secrets"));
	return SHOW_RELAY_OK;
}

static struct show_relay_ops fake_ops(struct fake_relay *f)
{
	struct show_relay_ops ops = {f,          f_check,       f_put, f_post, f_head, f_get_revision, f_get_bundle,
				     f_get_blob, f_get_secrets, NULL};
	return ops;
}

/* --------------------------------------------------------- fixtures --- */

static char lut_sha[65];
static char lut_path[1100];

/* A bundle like an export makes: a LUT asset, a browser-source secret, a basic.ini */
static void make_bundle(struct show_bundle *b, const char *id, const char *scene_name)
{
	show_bundle_init(b);
	b->manifest = show_manifest_new(id, "DPRTMNT");
	cj_set_str(b->manifest, "savedAt", "2026-11-10T20:00:00Z");
	cj *files = cj_get(b->manifest, "files");
	cj_set_str(files, "collection", "collection.json");
	cj_append(cj_set(files, "profile", cj_new_array()), cj_new_string("profile/basic.ini"));
	cj *asset = cj_append(cj_get(b->manifest, "assets"), cj_new_object());
	cj_set_str(asset, "sha256", lut_sha);
	cj_set_int(asset, "size", sfs_size(lut_path));
	cj_set_str(asset, "kind", "lut");
	cj_set_str(asset, "name", "Warm.cube");
	cj *secret = cj_append(cj_get(b->manifest, "secrets"), cj_new_object());
	cj_set_str(secret, "source", SECRET_SOURCE);
	cj_set_str(secret, "key", "/settings/url");
	b->collection = cj_new_object();
	cj_set_str(b->collection, "name", scene_name);
	cj *src = cj_append(cj_set(b->collection, "sources", cj_new_array()), cj_new_object());
	cj_set_str(src, "uuid", SECRET_SOURCE);
	cj_set_str(src, "id", "browser_source");
	cj_set_str(cj_set(src, "settings", cj_new_object()), "url", "${ctd-secret}");
	const char *ini = "[Video]\nBaseCX=1920\nBaseCY=1080\n";
	show_bundle_set_file(b, "profile/basic.ini", ini, strlen(ini));
	b->local = cj_new_object();
	cj_set_str(cj_get_or_add_object(b->local, "secrets"), SECRET_SOURCE "|/settings/url", SECRET_VALUE);
	b->asset_sources = cj_new_object();
	cj_set_str(b->asset_sources, lut_sha, lut_path);
}

/* A local save (Update) of the profile with another scene name: dirty until it's on Relay */
static int64_t local_save(const char *id, const char *scene_name)
{
	struct show_bundle b;
	char err[256];
	int64_t seq = 0;
	make_bundle(&b, id, scene_name);
	cj *meta = show_store_meta(id);
	cj_set_int(b.manifest, "parentRevision", cj_get_int(meta, "baseRevision", 0));
	if (cj_get_str(meta, "relayProfileId"))
		cj_set_str(b.manifest, "relayProfileId", cj_get_str(meta, "relayProfileId"));
	cj_free(meta);
	CHECK(show_store_save(id, &b, SHOW_SAVE_LOCAL, &seq, err, sizeof(err)));
	show_bundle_free(&b);
	return seq;
}

/* "Mac Studio" saves a revision on top of Relay's head */
static void other_machine_saves(struct fake_relay *f, const char *scene_name)
{
	struct show_bundle b;
	char *tar = NULL, sha[65], path[1300];
	size_t size = 0;
	make_bundle(&b, "7c9e6679-7425-40de-944b-e07fc1f90ae7", scene_name);
	CHECK(show_tar_write(&b, &tar, &size));
	sha256_hex(tar, size, sha);
	blob_path(f, sha, path, sizeof(path));
	CHECK(sfs_write_atomic(path, tar, size));
	cj *body = show_sync_revision_body(b.manifest, f->head, RELAY_PROFILE, sha, NULL, NULL, NULL);
	cj *reply = NULL;
	CHECK(f_post(f, RELAY_PROFILE, body, &reply) == SHOW_RELAY_OK);
	cj_free(reply);
	cj_free(body);
	free(tar);
	show_bundle_free(&b);
}

static const char *head_scene(const char *id)
{
	static char name[128];
	struct show_bundle b;
	name[0] = '\0';
	if (show_store_load(id, 0, &b, NULL, 0)) {
		snprintf(name, sizeof(name), "%s", cj_get_str(b.collection, "name"));
		show_bundle_free(&b);
	}
	return name;
}

static const char *relay_scene(struct fake_relay *f, int64_t revision)
{
	static char name[128];
	char path[1300], err[256];
	char *data = NULL;
	size_t size = 0;
	struct show_bundle b;
	name[0] = '\0';
	const cj *rev = find_rev(f, revision);
	blob_path(f, cj_get_str(rev, "bundleSha256"), path, sizeof(path));
	show_bundle_init(&b);
	if (rev && sfs_read(path, &data, &size) && show_tar_read(data, size, &b, err, sizeof(err)))
		snprintf(name, sizeof(name), "%s", cj_get_str(b.collection, "name"));
	free(data);
	show_bundle_free(&b);
	return name;
}

/* ------------------------------------------------------------- tests --- */

static void test_tar(void)
{
	struct show_bundle b, back;
	char *tar = NULL, *tar2 = NULL, err[256];
	size_t size = 0, size2 = 0;
	make_bundle(&b, ID, "Main");
	/* a long path takes the ustar prefix */
	const char *deep =
		"profile/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb/"
		"cccccccccccccccccccccccccccccccc.json";
	CHECK(show_bundle_set_file(&b, deep, "{}", 2));
	CHECK(show_tar_write(&b, &tar, &size));
	CHECK(size % 512 == 0);
	/* deterministic: the same bundle gives the same bytes (and bundleSha256) */
	CHECK(show_tar_write(&b, &tar2, &size2));
	CHECK(size == size2 && memcmp(tar, tar2, size) == 0);
	show_bundle_init(&back);
	CHECK(show_tar_read(tar, size, &back, err, sizeof(err)));
	CHECK(cj_equal(back.collection, b.collection));
	CHECK(back.file_count == 2);
	const struct show_file *ini = show_bundle_file(&back, "profile/basic.ini");
	CHECK(ini && ini->size == strlen("[Video]\nBaseCX=1920\nBaseCY=1080\n"));
	CHECK(show_bundle_file(&back, deep) != NULL);
	/* the secret value never goes into the tar */
	char *text = malloc(size + 1);
	memcpy(text, tar, size);
	for (size_t i = 0; i < size; i++)
		if (!text[i])
			text[i] = ' ';
	text[size] = '\0';
	CHECK(!unit_contains(text, "fixture-secret-123"));
	free(text);
	show_bundle_free(&back);

	/* a damaged tar, a path escape, a link, a missing collection: refused */
	tar[100] ^= 1; /* mode byte: the checksum no longer matches */
	show_bundle_init(&back);
	CHECK(!show_tar_read(tar, size, &back, err, sizeof(err)));
	show_bundle_free(&back);
	tar[100] ^= 1;
	struct show_bundle evil;
	show_bundle_init(&evil);
	evil.collection = cj_new_object();
	char *etar = NULL;
	size_t esize = 0;
	CHECK(show_tar_write(&evil, &etar, &esize));
	memcpy(etar, "../escape.json\0", 15); /* rename the entry; fix the checksum */
	memset(etar + 148, ' ', 8);
	unsigned sum = 0;
	for (int i = 0; i < 512; i++)
		sum += (unsigned char)etar[i];
	snprintf(etar + 148, 8, "%06o", sum);
	show_bundle_init(&back);
	CHECK(!show_tar_read(etar, esize, &back, err, sizeof(err)));
	show_bundle_free(&back);
	etar[156] = '2'; /* a symlink */
	memcpy(etar, "collection.json\0", 16);
	memset(etar + 148, ' ', 8);
	sum = 0;
	for (int i = 0; i < 512; i++)
		sum += (unsigned char)etar[i];
	snprintf(etar + 148, 8, "%06o", sum);
	show_bundle_init(&back);
	CHECK(!show_tar_read(etar, esize, &back, err, sizeof(err)));
	show_bundle_free(&back);
	show_bundle_init(&back);
	CHECK(!show_tar_read(etar, 1024, &back, err, sizeof(err)) || back.collection == NULL);
	show_bundle_free(&back);
	char zeros[1024] = {0};
	show_bundle_init(&back);
	CHECK(!show_tar_read(zeros, sizeof(zeros), &back, err, sizeof(err))); /* no collection.json */
	show_bundle_free(&back);
	free(etar);
	show_bundle_free(&evil);
	free(tar);
	free(tar2);
	show_bundle_free(&b);
}

static void test_core(void)
{
	/* classification (the dock's chip) */
	CHECK(show_sync_classify(false, 0, true, 5) == SHOW_SYNC_LOCAL_ONLY);
	CHECK(show_sync_classify(true, 3, false, -1) == SHOW_SYNC_UNKNOWN);
	CHECK(show_sync_classify(true, 3, false, 3) == SHOW_SYNC_UP_TO_DATE);
	CHECK(show_sync_classify(true, 3, true, 3) == SHOW_SYNC_NOT_BACKED_UP);
	CHECK(show_sync_classify(true, 3, false, 4) == SHOW_SYNC_UPDATE_READY);
	CHECK(show_sync_classify(true, 3, true, 4) == SHOW_SYNC_CONFLICT);

	/* conflict plans */
	struct show_conflict_plan p;
	show_conflict_plan(SHOW_CONFLICT_KEEP_MINE, 8, &p);
	CHECK(p.upload_mine && p.parent == 8 && !p.pull_theirs && !p.fork_mine);
	show_conflict_plan(SHOW_CONFLICT_TAKE_THEIRS, 8, &p);
	CHECK(!p.upload_mine && p.pull_theirs && !p.fork_mine);
	show_conflict_plan(SHOW_CONFLICT_SAVE_AS_NEW, 8, &p);
	CHECK(!p.upload_mine && p.pull_theirs && p.fork_mine);

	/* bodies */
	struct show_bundle b;
	make_bundle(&b, ID, "Main");
	const char *bundle_sha = "4b1d000000000000000000000000000000000000000000000000000000000000";
	cj *check = show_sync_check_body(b.manifest, bundle_sha, 2048);
	CHECK(cj_size(cj_get(check, "items")) == 2);
	CHECK_STR(cj_get_str(cj_at(cj_get(check, "items"), 0), "sha256"), bundle_sha);
	CHECK_STR(show_sync_blob_kind(b.manifest, bundle_sha, bundle_sha), "bundle");
	CHECK_STR(show_sync_blob_kind(b.manifest, lut_sha, bundle_sha), "lut");
	cj_free(check);

	cj *top = cj_append(cj_get(b.manifest, "secrets"), cj_new_object());
	cj_set(top, "source", cj_new_null()); /* a top-level value: Relay can't hold it */
	cj_set_str(top, "key", "/x");
	cj_set_str(cj_get(b.local, "secrets"), "|/x", "kept");
	int kept = 0;
	cj *body = show_sync_revision_body(b.manifest, 7, RELAY_PROFILE, bundle_sha, cj_get(b.local, "secrets"), "note",
					   &kept);
	CHECK(cj_get_int(body, "parentRevision", -1) == 7);
	CHECK_STR(cj_get_str(body, "bundleSha256"), bundle_sha);
	CHECK_STR(cj_get_str(body, "note"), "note");
	const cj *m = cj_get(body, "manifest");
	CHECK(cj_get_int(m, "revision", 0) == 8 && cj_get_int(m, "parentRevision", 0) == 7);
	CHECK_STR(cj_get_str(m, "relayProfileId"), RELAY_PROFILE);
	const cj *secrets = cj_get(body, "secrets");
	CHECK(cj_size(secrets) == 1 && kept == 1);
	CHECK_STR(cj_get_str(cj_at(secrets, 0), "source"), SECRET_SOURCE);
	CHECK_STR(cj_get_str(cj_at(secrets, 0), "value"), SECRET_VALUE);
	/* the manifest itself never carries the value */
	char *mtext = cj_print(m, false);
	CHECK(!unit_contains(mtext, "fixture-secret-123"));
	cj_free_str(mtext);
	cj_free(body);
	body = show_sync_revision_body(b.manifest, 0, RELAY_PROFILE, bundle_sha, NULL, NULL, NULL);
	CHECK(cj_get_int(body, "parentRevision", -1) == 0);
	CHECK(cj_kind(cj_get(cj_get(body, "manifest"), "parentRevision")) == CJ_NULL);
	CHECK(cj_get(body, "secrets") == NULL);
	cj_free(body);

	/* secrets back from Relay */
	cj *items = cj_new_array();
	cj *it = cj_append(items, cj_new_object());
	cj_set_str(it, "source", SECRET_SOURCE);
	cj_set_str(it, "key", "/settings/url");
	cj_set_str(it, "value", "v");
	cj_append(items, cj_new_object()); /* junk */
	cj *local = cj_new_object();
	CHECK(show_sync_secrets_to_local(items, local) == 1);
	CHECK_STR(cj_get_str(local, SECRET_SOURCE "|/settings/url"), "v");
	cj_free(local);
	cj_free(items);

	/* pulled manifest: Relay's numbers win */
	cj *pm = cj_clone(b.manifest);
	CHECK(show_sync_prepare_pulled(pm, ID, RELAY_PROFILE, 9, 8));
	CHECK(cj_get_int(pm, "revision", 0) == 9 && cj_get_int(pm, "parentRevision", 0) == 8);
	char err[256];
	CHECK(show_manifest_validate(pm, err, sizeof(err)));
	CHECK(!show_sync_prepare_pulled(pm, "not-a-uuid", RELAY_PROFILE, 9, 8));
	cj_free(pm);
	show_bundle_free(&b);

	/* retry back-off */
	CHECK(show_sync_retry_delay_s(1) == 15);
	CHECK(show_sync_retry_delay_s(2) == 30);
	CHECK(show_sync_retry_delay_s(20) == 600);
}

static bool name_exists(void *opaque, const char *name)
{
	return unit_contains((const char *)opaque, name) && strcmp(name, "DPRTMNT (mine 2)") != 0;
}

static void test_flows(void)
{
	char root[1024], scratch[1100], err[256];
	struct fake_relay f = {0};
	struct show_sync_outcome out;
	unit_scratch_dir("sync", root, sizeof(root));
	CHECK(show_store_open(root));
	snprintf(f.blobs, sizeof(f.blobs), "%s/relay-blobs", root);
	CHECK(sfs_mkdirs(f.blobs));
	f.revisions = cj_new_array();
	snprintf(scratch, sizeof(scratch), "%s/scratch", root);
	snprintf(lut_path, sizeof(lut_path), "%s/warm.cube", root);
	const char *lut_text =
		"TITLE \"warm\"\nLUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n";
	CHECK(sfs_write_atomic(lut_path, lut_text, strlen(lut_text)));
	sha256_hex(lut_text, strlen(lut_text), lut_sha);
	const struct show_relay_ops ops = fake_ops(&f);

	test_core();

	/* the offline queue */
	CHECK(!show_sync_queue_has(ID));
	CHECK(show_sync_queue_add(ID, 1000, "unreachable"));
	CHECK(show_sync_queue_has(ID));
	cj *due = show_sync_queue_due(1000);
	CHECK(cj_size(due) == 0); /* waits 15 s first */
	cj_free(due);
	due = show_sync_queue_due(1015);
	CHECK(cj_size(due) == 1 && strcmp(cj_str(cj_at(due, 0)), ID) == 0);
	cj_free(due);
	CHECK(show_sync_queue_add(ID, 1015, "unreachable"));
	due = show_sync_queue_due(1040);
	CHECK(cj_size(due) == 0); /* then 30 s */
	cj_free(due);
	CHECK(show_sync_queue_remove(ID));
	CHECK(!show_sync_queue_has(ID));

	/* machine A: a local show profile, never on Relay */
	CHECK(show_store_create(ID, "DPRTMNT", err, sizeof(err)));
	CHECK(local_save(ID, "Main") == 1);
	cj *meta = show_store_meta(ID);
	CHECK(cj_get_bool(meta, "dirty", false));
	cj_free(meta);

	/* offline: nothing changes, the caller queues it */
	f.offline = true;
	CHECK(!show_flow_save_up(&ops, ID, RELAY_PROFILE, -1, scratch, NULL, &out));
	CHECK(out.result == SHOW_RELAY_OFFLINE && !out.conflict);
	meta = show_store_meta(ID);
	CHECK(cj_get_bool(meta, "dirty", false) && cj_get_int(meta, "baseRevision", -1) == 0);
	cj_free(meta);
	f.offline = false;

	/* unlinked and no Relay profile chosen */
	CHECK(!show_flow_save_up(&ops, ID, NULL, -1, scratch, NULL, &out));

	/* save up: bundle + LUT uploaded, revision 1, linked, backed up */
	CHECK(show_flow_save_up(&ops, ID, RELAY_PROFILE, -1, scratch, "first", &out));
	CHECK(out.result == SHOW_RELAY_OK && out.revision == 1 && out.uploaded == 2);
	CHECK(f.head == 1 && has_blob(&f, lut_sha));
	meta = show_store_meta(ID);
	CHECK(!cj_get_bool(meta, "dirty", true) && cj_get_int(meta, "baseRevision", 0) == 1);
	CHECK_STR(cj_get_str(meta, "relayProfileId"), RELAY_PROFILE);
	cj_free(meta);
	/* the secret went in the save's secrets[], never into a blob */
	CHECK(cj_size(cj_get(find_rev(&f, 1), "secrets")) == 1);
	CHECK(sfs_exists(scratch)); /* the scratch tar is gone */
	/* again: nothing to do */
	CHECK(show_flow_save_up(&ops, ID, RELAY_PROFILE, -1, scratch, NULL, &out));
	CHECK(out.no_changes && f.head == 1);
	/* a later local save stays linked (export copies relayProfileId from the head) and uploads only the bundle */
	CHECK(local_save(ID, "Main v2") == 2);
	CHECK(show_flow_save_up(&ops, ID, NULL, -1, scratch, NULL, &out));
	CHECK(out.revision == 2 && out.uploaded == 1);

	/* M3: delete the local copy (and the asset cache, like a fresh machine), pull, and get the same setup */
	CHECK(show_store_delete(ID));
	char assets[1100];
	snprintf(assets, sizeof(assets), "%s/assets", root);
	CHECK(sfs_remove_tree(assets));
	CHECK(show_flow_pull(&ops, RELAY_PROFILE, 0, NULL, scratch, &out));
	CHECK(out.result == SHOW_RELAY_OK && out.revision == 2 && out.downloaded == 1 && out.secrets_pulled == 1);
	CHECK_STR(out.id, ID); /* the manifest's showProfileId */
	CHECK_STR(head_scene(ID), "Main v2");
	CHECK(show_store_has_asset(lut_sha, "Warm.cube", (int64_t)strlen(lut_text)));
	struct show_bundle pulled;
	CHECK(show_store_load(ID, 0, &pulled, err, sizeof(err)));
	CHECK_STR(cj_get_str(cj_get(pulled.local, "secrets"), SECRET_SOURCE "|/settings/url"), SECRET_VALUE);
	CHECK(cj_get_int(pulled.manifest, "revision", 0) == 2);
	show_bundle_free(&pulled);
	meta = show_store_meta(ID);
	CHECK(!cj_get_bool(meta, "dirty", true) && cj_get_int(meta, "baseRevision", 0) == 2);
	cj_free(meta);
	/* pulling it again: already here */
	CHECK(show_flow_pull(&ops, RELAY_PROFILE, 0, NULL, scratch, &out));
	CHECK(out.no_changes);

	/* without show:secrets the pull still works; the value just isn't there ("needs re-entry") */
	CHECK(show_store_delete(ID));
	f.no_secrets_scope = true;
	CHECK(show_flow_pull(&ops, RELAY_PROFILE, 0, NULL, scratch, &out));
	CHECK(out.secrets_pulled == 0 && out.downloaded == 0); /* the LUT is cached by hash */
	f.no_secrets_scope = false;

	/* a damaged download changes nothing */
	{
		char path[1300];
		blob_path(&f, cj_get_str(find_rev(&f, 2), "bundleSha256"), path, sizeof(path));
		char *data = NULL;
		size_t size = 0;
		CHECK(sfs_read(path, &data, &size));
		data[600] ^= 1;
		CHECK(sfs_write_atomic(path, data, size));
		CHECK(!show_flow_pull(&ops, RELAY_PROFILE, 2, "a1b2c3d4-0000-4000-8000-0000000000ff", scratch, &out));
		CHECK(out.result == SHOW_RELAY_BAD);
		CHECK(show_store_meta("a1b2c3d4-0000-4000-8000-0000000000ff") ==
		      NULL); /* the new profile was removed */
		data[600] ^= 1;
		CHECK(sfs_write_atomic(path, data, size));
		free(data);
	}

	/* conflict: this machine saves (dirty) while Mac Studio saves revision 3 */
	local_save(ID, "Mine A");
	other_machine_saves(&f, "Theirs 3");
	CHECK(!show_flow_save_up(&ops, ID, NULL, -1, scratch, NULL, &out));
	CHECK(out.conflict && out.head == 3 && f.head == 3);
	/* Keep mine: mine is revision 4 on top; theirs stays in history */
	CHECK(show_flow_resolve(&ops, ID, NULL, SHOW_CONFLICT_KEEP_MINE, out.head, scratch, &out));
	CHECK(out.revision == 4 && f.head == 4);
	CHECK_STR(relay_scene(&f, 4), "Mine A");
	CHECK_STR(relay_scene(&f, 3), "Theirs 3");
	meta = show_store_meta(ID);
	CHECK(!cj_get_bool(meta, "dirty", true) && cj_get_int(meta, "baseRevision", 0) == 4);
	cj_free(meta);

	/* Take theirs: theirs becomes this machine's head; mine stays as an older local revision */
	int64_t mine_seq = local_save(ID, "Mine B");
	other_machine_saves(&f, "Theirs 5");
	CHECK(!show_flow_save_up(&ops, ID, NULL, -1, scratch, NULL, &out) && out.conflict && out.head == 5);
	CHECK(show_flow_resolve(&ops, ID, NULL, SHOW_CONFLICT_TAKE_THEIRS, out.head, scratch, &out));
	CHECK(out.revision == 5 && f.head == 5);
	CHECK_STR(head_scene(ID), "Theirs 5");
	struct show_bundle older;
	CHECK(show_store_load(ID, mine_seq, &older, err, sizeof(err)));
	CHECK_STR(cj_get_str(older.collection, "name"), "Mine B");
	show_bundle_free(&older);
	meta = show_store_meta(ID);
	CHECK(!cj_get_bool(meta, "dirty", true) && cj_get_int(meta, "baseRevision", 0) == 5);
	cj_free(meta);

	/* Save as new profile: mine becomes "DPRTMNT (mine)", local only; this profile takes theirs */
	local_save(ID, "Mine C");
	other_machine_saves(&f, "Theirs 6");
	CHECK(!show_flow_save_up(&ops, ID, NULL, -1, scratch, NULL, &out) && out.conflict && out.head == 6);
	CHECK(show_flow_resolve(&ops, ID, NULL, SHOW_CONFLICT_SAVE_AS_NEW, out.head, scratch, &out));
	CHECK(out.revision == 6 && show_is_uuid(out.forked_id) && strcmp(out.forked_id, ID) != 0);
	CHECK_STR(head_scene(ID), "Theirs 6");
	CHECK_STR(head_scene(out.forked_id), "Mine C");
	meta = show_store_meta(out.forked_id);
	CHECK_STR(cj_get_str(meta, "name"), "DPRTMNT (mine)");
	CHECK(cj_kind(cj_get(meta, "relayProfileId")) == CJ_NULL && cj_get_bool(meta, "dirty", false));
	cj_free(meta);
	CHECK(f.head == 6); /* nothing uploaded */
	char forked[37];
	snprintf(forked, sizeof(forked), "%s", out.forked_id);
	/* its secrets came along (local.json) */
	struct show_bundle fb;
	CHECK(show_store_load(forked, 0, &fb, err, sizeof(err)));
	CHECK_STR(cj_get_str(cj_get(fb.local, "secrets"), SECRET_SOURCE "|/settings/url"), SECRET_VALUE);
	CHECK(cj_kind(cj_get(fb.manifest, "relayProfileId")) == CJ_NULL);
	show_bundle_free(&fb);

	/* the fork's own first save onto a Relay profile that already holds a show profile: a conflict, not an overwrite */
	CHECK(!show_flow_save_up(&ops, forked, RELAY_PROFILE, -1, scratch, NULL, &out));
	CHECK(out.conflict && out.head == 6 && f.head == 6);
	/* Take theirs there: the unlinked profile gets linked to the Relay profile it met */
	CHECK(show_flow_resolve(&ops, forked, RELAY_PROFILE, SHOW_CONFLICT_TAKE_THEIRS, 6, scratch, &out));
	CHECK(out.revision == 6);
	meta = show_store_meta(forked);
	CHECK_STR(cj_get_str(meta, "relayProfileId"), RELAY_PROFILE);
	cj_free(meta);
	CHECK_STR(head_scene(forked), "Theirs 6");

	/* fork names */
	char name[96];
	show_sync_fork_name("DPRTMNT", name_exists, "DPRTMNT (mine)", name, sizeof(name));
	CHECK_STR(name, "DPRTMNT (mine 2)");
	show_sync_fork_name("0123456789012345678901234567890123456789012345678901234567890123", NULL, NULL, name,
			    sizeof(name));
	CHECK(strlen(name) <= SHOW_NAME_MAX && show_name_is_valid(name));

	/* uuids */
	char u[37];
	show_flow_new_uuid(u);
	CHECK(show_is_uuid(u) && u[14] == '4');

	cj_free(f.revisions);
	show_store_close();
}

void test_sync(void)
{
	test_tar();
	test_flows();
}
