/* Local store (ticket T7): versioned bundles, checksums, asset cache, pruning, local secrets, leftovers swept. */
#include "unit.h"
#include "manifest.h"
#include "sha256.h"
#include "show-fs.h"
#include "store.h"

#include <stdlib.h>

#define ID "0f8fad5b-d9cb-469f-a165-70867728950e"
#define ID2 "7c9e6679-7425-40de-944b-e07fc1f90ae7"

static void make_bundle(struct show_bundle *b, const char *lut_path, const char *lut_sha, int64_t lut_size,
			const char *scene_name)
{
	show_bundle_init(b);
	b->manifest = show_manifest_new(ID, "DPRTMNT");
	cj_set_str(b->manifest, "savedAt", "2026-11-05T20:00:00Z");
	cj *files = cj_get(b->manifest, "files");
	cj_set_str(files, "collection", "collection.json");
	cj_append(cj_set(files, "profile", cj_new_array()), cj_new_string("profile/basic.ini"));
	cj *asset = cj_append(cj_get(b->manifest, "assets"), cj_new_object());
	cj_set_str(asset, "sha256", lut_sha);
	cj_set_int(asset, "size", lut_size);
	cj_set_str(asset, "kind", "lut");
	cj_set_str(asset, "name", "Warm.CUBE");
	b->collection = cj_new_object();
	cj_set_str(b->collection, "name", scene_name);
	cj_set_str(b->collection, "lut", "${ctd-asset:x}");
	const char *ini = "[Video]\nBaseCX=1920\n";
	show_bundle_set_file(b, "profile/basic.ini", ini, strlen(ini));
	b->local = cj_new_object();
	cj_set_str(cj_get_or_add_object(b->local, "secrets"), "uuid|/settings/password", scene_name);
	b->asset_sources = cj_new_object();
	cj_set_str(b->asset_sources, lut_sha, lut_path);
}

void test_store(void)
{
	char root[1024], lut[1100], sha[65], err[256], path[2048];
	unit_scratch_dir("store", root, sizeof(root));

	/* a leftover from a crash is swept on open */
	char leftover[1100];
	snprintf(leftover, sizeof(leftover), "%s/show-profiles/%s/revisions/.tmp-dead", root,
		 "5d1e7a9c-0000-4000-8000-000000000001");
	sfs_mkdirs(leftover);
	CHECK(show_store_open(root));
	CHECK(!sfs_exists(leftover));

	snprintf(lut, sizeof(lut), "%s/warm.cube", root);
	const char *lut_text = "TITLE \"warm\"\nLUT_3D_SIZE 2\n";
	CHECK(sfs_write_atomic(lut, lut_text, strlen(lut_text)));
	sha256_hex(lut_text, strlen(lut_text), sha);

	CHECK(show_store_create(ID, "DPRTMNT", err, sizeof(err)));
	CHECK(!show_store_create(ID2, "dprtmnt", err, sizeof(err))); /* names are unique, any case */
	CHECK(!show_store_create("../evil", "X", err, sizeof(err)));
	char found[37];
	CHECK(show_store_find_by_name("DPRTMNT", found) && strcmp(found, ID) == 0);

	struct show_bundle b;
	int64_t seq = 0;
	make_bundle(&b, lut, sha, (int64_t)strlen(lut_text), "rev1");
	CHECK(show_store_save(ID, &b, SHOW_SAVE_LOCAL, &seq, err, sizeof(err)));
	if (err[0] && seq == 0)
		fprintf(stderr, "  save: %s\n", err);
	CHECK(seq == 1);
	show_bundle_free(&b);
	CHECK(show_store_has_asset(sha, "Warm.CUBE", (int64_t)strlen(lut_text)));
	CHECK(show_store_asset_path(sha, "Warm.CUBE", path, sizeof(path)));
	CHECK(unit_contains(path, "/assets/"));
	CHECK(unit_contains(path, ".cube"));

	cj *meta = show_store_meta(ID);
	CHECK(cj_get_int(meta, "headSeq", 0) == 1);
	CHECK(cj_get_bool(meta, "dirty", false));
	cj_free(meta);

	/* the same content again is not a new revision */
	make_bundle(&b, lut, sha, (int64_t)strlen(lut_text), "rev1");
	cj_set_str(b.manifest, "savedAt", "2026-11-06T10:00:00Z");
	CHECK(show_store_save(ID, &b, SHOW_SAVE_LOCAL, &seq, err, sizeof(err)));
	CHECK(seq == 1);
	show_bundle_free(&b);

	/* load checks everything and brings the local half */
	CHECK(show_store_load(ID, 0, &b, err, sizeof(err)));
	CHECK_STR(cj_get_str(b.collection, "name"), "rev1");
	CHECK(show_bundle_file(&b, "profile/basic.ini") != NULL);
	CHECK_STR(cj_get_str(cj_get(b.local, "secrets"), "uuid|/settings/password"), "rev1");
	show_bundle_free(&b);

	/* a revision whose asset can't be cached is refused, and nothing is written */
	make_bundle(&b, lut, "1111111111111111111111111111111111111111111111111111111111111111", 3, "bad");
	CHECK(!show_store_save(ID, &b, SHOW_SAVE_LOCAL, &seq, err, sizeof(err)));
	CHECK(unit_contains(err, "hash mismatch"));
	show_bundle_free(&b);
	cj *revs = show_store_revisions(ID);
	CHECK(cj_size(revs) == 1);
	cj_free(revs);

	/* an invalid manifest is refused */
	make_bundle(&b, lut, sha, (int64_t)strlen(lut_text), "bad");
	cj_set_int(b.manifest, "schema", 9);
	CHECK(!show_store_save(ID, &b, SHOW_SAVE_LOCAL, &seq, err, sizeof(err)));
	show_bundle_free(&b);

	/* more revisions: pruned to 5 + the applied one */
	meta = show_store_meta(ID);
	cj_set_int(meta, "appliedSeq", 1);
	CHECK(show_store_write_meta(ID, meta));
	cj_free(meta);
	for (int i = 2; i <= 8; i++) {
		char name[16];
		snprintf(name, sizeof(name), "rev%d", i);
		make_bundle(&b, lut, sha, (int64_t)strlen(lut_text), name);
		CHECK(show_store_save(ID, &b, SHOW_SAVE_LOCAL, &seq, err, sizeof(err)));
		CHECK(seq == i);
		show_bundle_free(&b);
	}
	revs = show_store_revisions(ID);
	CHECK(cj_size(revs) == 6);
	CHECK(cj_int(cj_at(revs, 0), 0) == 8);
	CHECK(cj_int(cj_at(revs, 5), 0) == 1); /* the applied one stays */
	cj_free(revs);

	/* local secrets merge across revisions: the newest value wins */
	CHECK(show_store_load(ID, 1, &b, err, sizeof(err)));
	CHECK_STR(cj_get_str(cj_get(b.local, "secrets"), "uuid|/settings/password"), "rev8");
	show_bundle_free(&b);

	/* corrupting a file makes the revision unloadable (never applied) */
	char rev_dir[2048], file[2100];
	CHECK(show_store_revision_dir(ID, 8, rev_dir, sizeof(rev_dir)));
	snprintf(file, sizeof(file), "%s/collection.json", rev_dir);
	CHECK(sfs_write_atomic(file, "{\"name\":\"tampered\"}", 19));
	CHECK(!show_store_load(ID, 8, &b, err, sizeof(err)));
	CHECK(unit_contains(err, "checksum"));
	CHECK(b.manifest == NULL && b.collection == NULL);
	snprintf(file, sizeof(file), "%s/profile/basic.ini", rev_dir);
	CHECK(show_store_load(ID, 7, &b, err, sizeof(err)));
	show_bundle_free(&b);
	CHECK(!show_store_load(ID, 99, &b, err, sizeof(err)));

	/* Relay's revision number (M3) */
	CHECK(show_store_set_relay_revision(ID, 7, 12));
	meta = show_store_meta(ID);
	CHECK(cj_get_int(meta, "baseRevision", 0) == 12);
	CHECK(cj_get_bool(meta, "dirty", false)); /* 7 isn't the head */
	cj_free(meta);
	CHECK(show_store_load(ID, 7, &b, err, sizeof(err)));
	CHECK(cj_get_int(b.manifest, "revision", 0) == 12);
	show_bundle_free(&b);

	/* a pulled revision isn't dirty */
	make_bundle(&b, lut, sha, (int64_t)strlen(lut_text), "from relay");
	cj_set_int(b.manifest, "revision", 13);
	cj_set_int(b.manifest, "parentRevision", 12);
	CHECK(show_store_save(ID, &b, SHOW_SAVE_RELAY, &seq, err, sizeof(err)));
	show_bundle_free(&b);
	meta = show_store_meta(ID);
	CHECK(!cj_get_bool(meta, "dirty", true));
	CHECK(cj_get_int(meta, "baseRevision", 0) == 13);
	cj_free(meta);

	/* device maps (M4) */
	cj *map = cj_new_object();
	cj_set_str(map, "slot", "device");
	CHECK(show_store_write_device_map(ID, "STUDIO-PC", map));
	CHECK(!show_store_write_device_map(ID, "../x", map));
	cj *read = show_store_device_map(ID, "STUDIO-PC");
	CHECK(cj_equal(map, read));
	cj_free(read);
	cj_free(map);

	/* list, rename, delete */
	CHECK(show_store_create(ID2, "I'm Your Bus", err, sizeof(err)));
	cj *list = show_store_list();
	CHECK(cj_size(list) == 2);
	CHECK_STR(cj_get_str(cj_at(list, 0), "name"), "DPRTMNT");
	cj_free(list);
	CHECK(!show_store_rename(ID2, "dprtmnt", err, sizeof(err)));
	CHECK(show_store_rename(ID2, "Bus", err, sizeof(err)));
	CHECK(show_store_delete(ID2));
	CHECK(show_store_meta(ID2) == NULL);
	show_store_close();
}
