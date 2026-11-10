/* Export tests (ticket T9): a fixture scene collection and profile become a bundle that matches a golden manifest,
 * keeps no fixture secret outside the machine-local half, and round-trips its asset paths. */
#include "bundle.h"
#include "export.h"
#include "ini.h"
#include "manifest.h"
#include "scrub.h"
#include "show-fs.h"
#include "unit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void slashes(char *s)
{
	for (; *s; s++)
		if (*s == '\\')
			*s = '/';
}

static void make_asset(const char *dir, const char *rel, const char *content)
{
	char path[1024], parent[1024];
	CHECK(sfs_join(path, sizeof(path), dir, rel));
	snprintf(parent, sizeof(parent), "%s", path);
	char *slash = strrchr(parent, '/');
	if (slash) {
		*slash = '\0';
		CHECK(sfs_mkdirs(parent));
	}
	CHECK(sfs_write_atomic(path, content, strlen(content)));
}

/* The fixture collection with @ASSETS@ replaced by dir, written to out_path. */
static bool write_collection(const char *dir, const char *out_path)
{
	char *text = unit_fixture_text("export/collection.json");
	char base[1024];
	size_t cap, w = 0;
	char *out;
	bool ok;
	if (!text)
		return false;
	snprintf(base, sizeof(base), "%s", dir);
	slashes(base);
	cap = strlen(text) + 64 * (strlen(base) + 1) + 1;
	out = malloc(cap);
	for (const char *p = text; *p;) {
		if (strncmp(p, "@ASSETS@", 8) == 0) {
			w += (size_t)snprintf(out + w, cap - w, "%s", base);
			p += 8;
		} else {
			out[w++] = *p++;
		}
	}
	out[w] = '\0';
	ok = sfs_write_atomic(out_path, out, w);
	free(out);
	free(text);
	return ok;
}

static bool path_from_sources(void *opaque, const cj *asset, char *out, size_t size)
{
	const char *p = cj_get_str((const cj *)opaque, cj_get_str(asset, "sha256"));
	if (!p)
		return false;
	snprintf(out, size, "%s", p);
	return true;
}

static bool has_fixture_secret(const char *text)
{
	return text && unit_contains(text, "FIXTURE-");
}

static void check_no_secrets_outside_local(const struct show_bundle *b)
{
	char *s = cj_print(b->collection, true);
	CHECK(!has_fixture_secret(s));
	cj_free_str(s);
	s = cj_print(b->manifest, true);
	CHECK(!has_fixture_secret(s));
	cj_free_str(s);
	for (size_t i = 0; i < b->file_count; i++)
		CHECK(!has_fixture_secret(b->files[i].data));
}

static const cj *object_by_uuid(cj *root, const char *uuid)
{
	return uuid ? show_find_by_uuid(root, uuid) : NULL;
}

/* Every manifest ref of the re-resolved collection holds the path of the original collection. */
static void check_round_trip(cj *orig, cj *resolved, const cj *manifest, const cj *asset_sources)
{
	const cj *assets = cj_get(manifest, "assets");
	int checked = 0;
	for (size_t i = 0; i < cj_size(assets); i++) {
		const cj *refs = cj_get(cj_at(assets, i), "refs");
		for (size_t j = 0; j < cj_size(refs); j++) {
			const cj *ref = cj_at(refs, j);
			const cj *o = object_by_uuid(orig, cj_get_str(ref, "source"));
			const cj *r = object_by_uuid(resolved, cj_get_str(ref, "source"));
			if (cj_get_str(ref, "filter")) {
				o = object_by_uuid((cj *)o, cj_get_str(ref, "filter"));
				r = object_by_uuid((cj *)r, cj_get_str(ref, "filter"));
			}
			const cj *ov = cj_pointer(o, cj_get_str(ref, "key"));
			const cj *rv = cj_pointer(r, cj_get_str(ref, "key"));
			CHECK(cj_is_string(ov) && cj_is_string(rv));
			if (cj_is_string(ov) && cj_is_string(rv)) {
				CHECK_STR(cj_str(rv), cj_str(ov));
				CHECK_STR(cj_str(rv),
					  cj_get_str(asset_sources, cj_get_str(cj_at(assets, i), "sha256")));
			}
			checked++;
		}
	}
	CHECK(checked == 8);
}

void test_export(void)
{
	char dir[1024], assets[1024], collection[1100], profile[1024], err[512];
	unit_scratch_dir("export", dir, sizeof(dir));
	sfs_join(assets, sizeof(assets), dir, "assets");
	sfs_join(collection, sizeof(collection), dir, "collection.json");
	make_asset(assets, "logo.png", "ASSET-BYTES logo.png");
	make_asset(assets, "intro.mp4", "ASSET-BYTES intro.mp4");
	make_asset(assets, "warm.cube", "ASSET-BYTES warm.cube");
	make_asset(assets, "lowerthird.html", "ASSET-BYTES lowerthird.html");
	make_asset(assets, "music/track1.mp3", "ASSET-BYTES track1.mp3");
	make_asset(assets, "music/track2.mp3", "ASSET-BYTES track2.mp3");
	make_asset(assets, "stinger.mov", "ASSET-BYTES stinger.mov");
	CHECK(write_collection(assets, collection));
	unit_fixture_path("export/profile", profile, sizeof(profile));

	struct show_export_input in = {0};
	in.id = "22222222-2222-4222-8222-222222222222";
	in.name = "DPRTMNT";
	in.collection_path = collection;
	in.profile_dir = profile;
	in.app_id = "ctd-obs-plugin-studio";
	in.os = "windows";
	in.obs_version = "33.0.1";
	in.ctd_obs_version = "1.2.3";
	in.plugin_version = "0.1.0";
	in.saved_at = "2026-11-05T20:00:00Z";

	struct show_bundle b;
	show_bundle_init(&b);
	cj *warnings = cj_new_array();
	err[0] = '\0';
	bool ok = show_export(&in, &b, warnings, err, sizeof(err));
	if (!ok)
		fprintf(stderr, "export failed: %s\n", err);
	CHECK(ok);
	if (!ok) {
		show_bundle_free(&b);
		cj_free(warnings);
		return;
	}

	/* golden manifest (savedAt is the one volatile field) */
	cj *expected = unit_fixture_json("export/expected-manifest.json");
	cj_set_str(b.manifest, "savedAt", "");
	if (expected) {
		cj_set_str(expected, "savedAt", "");
		bool same = cj_equal(b.manifest, expected);
		CHECK(same);
		if (!same) {
			char *s = cj_print(b.manifest, true);
			fprintf(stderr, "exported manifest:\n%s\n", s);
			cj_free_str(s);
		}
	}
	cj_free(expected);
	cj_set_str(b.manifest, "savedAt", in.saved_at);
	CHECK(show_manifest_validate(b.manifest, err, sizeof(err)));

	/* warnings: the one image that is gone */
	CHECK(cj_size(warnings) == 1);
	CHECK(cj_size(warnings) == 1 && unit_contains(cj_str(cj_at(warnings, 0)), "Missing file for Old photo: "));
	CHECK(cj_size(warnings) == 1 && unit_contains(cj_str(cj_at(warnings, 0)), "gone.png"));

	/* profile files */
	const struct show_file *ini = show_bundle_file(&b, SHOW_FILE_BASIC_INI);
	CHECK(ini != NULL);
	if (ini) {
		char v[64];
		CHECK(show_ini_get(ini->data, "Video", "BaseCX", v, sizeof(v)) && strcmp(v, "1920") == 0);
		CHECK(show_ini_get(ini->data, "AdvOut", "Encoder", v, sizeof(v)) && strcmp(v, "obs_x264") == 0);
		CHECK(!show_ini_get(ini->data, "SimpleOutput", "FilePath", v, sizeof(v)));
		CHECK(!show_ini_get(ini->data, "AdvOut", "RecFilePath", v, sizeof(v)));
		CHECK(!unit_contains(ini->data, "Hotkeys"));
		CHECK(!unit_contains(ini->data, "Users"));
	}
	CHECK(show_bundle_file(&b, SHOW_FILE_STREAM_ENCODER) != NULL);
	CHECK(show_bundle_file(&b, SHOW_FILE_RECORD_ENCODER) == NULL);
	const struct show_file *svc = show_bundle_file(&b, SHOW_FILE_SERVICE);
	CHECK(svc != NULL);
	if (svc) {
		CHECK(unit_contains(svc->data, "rtmp_custom"));
		CHECK(unit_contains(svc->data, "ingest.example.invalid"));
	}
	CHECK(b.file_count == 3);

	/* nothing secret outside the machine-local half; the local half has the originals */
	check_no_secrets_outside_local(&b);
	char *local = cj_print(b.local, true);
	CHECK(unit_contains(local, "FIXTURE-STREAM-KEY-5678"));
	CHECK(unit_contains(local, "FIXTURE-OVERLAY-KEY-1234"));
	cj_free_str(local);
	CHECK(cj_is_object(cj_get(b.local, "secrets")));
	CHECK(cj_is_object(cj_get(b.local, "service")));
	CHECK(cj_size(b.asset_sources) == 7);

	/* the printed collection has no local asset path */
	char *printed = cj_print(b.collection, false);
	char base[1024];
	snprintf(base, sizeof(base), "%s", assets);
	slashes(base);
	CHECK(!unit_contains(printed, "logo.png\""));
	CHECK(unit_contains(printed, "gone.png")); /* the missing one is left as it was */
	{
		char *p = printed;
		int placeholders = 0;
		while ((p = strstr(p, "${ctd-asset:")) != NULL) {
			placeholders++;
			p++;
		}
		CHECK(placeholders == 8);
	}
	cj_free_str(printed);

	/* re-import check: resolve with the cached paths gives back the original paths */
	char cerr[256];
	cj *orig = cj_parse_file(collection, cerr, sizeof(cerr));
	CHECK(orig != NULL);
	if (orig) {
		CHECK(show_assets_resolve(b.collection, b.manifest, path_from_sources, b.asset_sources, err,
					  sizeof(err)));
		check_round_trip(orig, b.collection, b.manifest, b.asset_sources);
		printed = cj_print(b.collection, false);
		CHECK(!unit_contains(printed, "${ctd-asset:"));
		cj_free_str(printed);
		cj_free(orig);
	}

	/* a second save: relay id and revision carry over */
	cj *prev = show_manifest_new(in.id, "DPRTMNT");
	cj_set_str(prev, "relayProfileId", "33333333-3333-4333-8333-333333333333");
	cj_set_int(prev, "revision", 7);
	cj_set_int(prev, "parentRevision", 6);
	in.previous_manifest = prev;
	struct show_bundle b2;
	show_bundle_init(&b2);
	CHECK(show_export(&in, &b2, NULL, err, sizeof(err)));
	CHECK_STR(cj_get_str(b2.manifest, "relayProfileId"), "33333333-3333-4333-8333-333333333333");
	CHECK(cj_get_int(b2.manifest, "parentRevision", -1) == 7);
	CHECK(cj_get_int(b2.manifest, "revision", -1) == 0); /* Relay numbers it (M3) */
	check_no_secrets_outside_local(&b2);
	show_bundle_free(&b2);

	/* a local save on top of a local save keeps the Relay base */
	cj_set_int(prev, "revision", 0);
	show_bundle_init(&b2);
	CHECK(show_export(&in, &b2, NULL, err, sizeof(err)));
	CHECK(cj_get_int(b2.manifest, "parentRevision", -1) == 6);
	CHECK(cj_get_int(b2.manifest, "revision", -1) == 0);
	show_bundle_free(&b2);
	cj_free(prev);

	/* failures */
	struct show_bundle b3;
	show_bundle_init(&b3);
	in.previous_manifest = NULL;
	in.collection_path = "/nonexistent/ctd-show/collection.json";
	CHECK(!show_export(&in, &b3, NULL, err, sizeof(err)));
	CHECK(err[0] != '\0');
	show_bundle_free(&b3);
	show_bundle_init(&b3);
	in.collection_path = collection;
	in.name = "bad/name";
	CHECK(!show_export(&in, &b3, NULL, err, sizeof(err)));
	CHECK(unit_contains(err, "name"));
	show_bundle_free(&b3);

	/* no profile folder: a collection-only bundle */
	show_bundle_init(&b3);
	in.name = "DPRTMNT";
	in.profile_dir = NULL;
	CHECK(show_export(&in, &b3, NULL, err, sizeof(err)));
	CHECK(b3.file_count == 0);
	CHECK(cj_size(cj_get(cj_get(b3.manifest, "files"), "profile")) == 0);
	show_bundle_free(&b3);

	show_bundle_free(&b);
	cj_free(warnings);
}
