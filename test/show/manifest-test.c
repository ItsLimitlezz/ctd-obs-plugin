/* Manifest validation (src/show/manifest.c) and path safety (design section 8). */
#include "unit.h"
#include "manifest.h"

#define ID "0f8fad5b-d9cb-469f-a165-70867728950e"
#define SHA "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08"

static cj *valid(void)
{
	cj *m = show_manifest_new(ID, "DPRTMNT");
	cj_set_str(cj_get(m, "files"), "collection", "collection.json");
	cj *profile = cj_set(cj_get(m, "files"), "profile", cj_new_array());
	cj_append(profile, cj_new_string("profile/basic.ini"));
	cj *asset = cj_append(cj_get(m, "assets"), cj_new_object());
	cj_set_str(asset, "sha256", SHA);
	cj_set_int(asset, "size", 12);
	cj_set_str(asset, "kind", "lut");
	cj_set_str(asset, "name", "warm.cube");
	cj *ref = cj_append(cj_get_or_add_array(asset, "refs"), cj_new_object());
	cj_set_str(ref, "source", "11111111-1111-4111-8111-111111111111");
	cj_set(ref, "filter", cj_new_null());
	cj_set_str(ref, "key", "/settings/image_path");
	return m;
}

static void expect_invalid(cj *m, const char *why)
{
	char err[256];
	bool ok = show_manifest_validate(m, err, sizeof(err));
	CHECK(!ok);
	if (ok)
		fprintf(stderr, "  accepted: %s\n", why);
	cj_free(m);
}

void test_manifest(void)
{
	char err[256];
	cj *m = valid();
	CHECK(show_manifest_validate(m, err, sizeof(err)));
	if (err[0])
		fprintf(stderr, "  %s\n", err);
	cj_free(m);

	m = valid();
	cj_set_int(m, "schema", 2);
	CHECK(!show_manifest_validate(m, err, sizeof(err)));
	CHECK(unit_contains(err, "update CTD OBS"));
	cj_free(m);

	m = valid();
	cj_set_str(m, "showProfileId", "../../etc");
	expect_invalid(m, "bad id");
	m = valid();
	cj_set_str(m, "name", "a/b");
	expect_invalid(m, "name with slash");
	m = valid();
	cj_set_str(m, "name", "");
	expect_invalid(m, "empty name");
	m = valid();
	cj_set_str(cj_get(m, "files"), "collection", "../collection.json");
	expect_invalid(m, "parent path");
	m = valid();
	cj_append(cj_get(cj_get(m, "files"), "profile"), cj_new_string("C:\\x.ini"));
	expect_invalid(m, "windows path");
	m = valid();
	cj_set_str(cj_at(cj_get(m, "assets"), 0), "sha256", "ABC");
	expect_invalid(m, "bad sha");
	m = valid();
	cj_set_str(cj_at(cj_get(m, "assets"), 0), "kind", "exe");
	expect_invalid(m, "unknown kind");
	m = valid();
	cj_set_str(cj_at(cj_get(m, "assets"), 0), "name", "../x.cube");
	expect_invalid(m, "asset name with path");
	m = valid();
	cj_append(cj_get(m, "assets"), cj_clone(cj_at(cj_get(m, "assets"), 0)));
	expect_invalid(m, "duplicate asset");
	m = valid();
	cj_set_int(m, "revision", 3);
	cj_set_int(m, "parentRevision", 3);
	expect_invalid(m, "parent == revision");
	m = valid();
	cj *secret = cj_append(cj_get(m, "secrets"), cj_new_object());
	cj_set_str(secret, "key", "/settings/password");
	cj_set_str(secret, "value", "leak");
	expect_invalid(m, "secret value in manifest");
	m = valid();
	cj_append(cj_get(m, "overlays"), cj_new_object());
	expect_invalid(m, "overlay without slot");

	/* forward-compatible: unknown members are fine */
	m = valid();
	cj_set_str(m, "futureField", "x");
	CHECK(show_manifest_validate(m, err, sizeof(err)));
	cj_free(m);

	CHECK(show_path_is_safe("profile/basic.ini"));
	CHECK(!show_path_is_safe(""));
	CHECK(!show_path_is_safe("/abs"));
	CHECK(!show_path_is_safe("a//b"));
	CHECK(!show_path_is_safe("a/./b"));
	CHECK(!show_path_is_safe("a/.."));
	CHECK(!show_path_is_safe("a\\b"));
	CHECK(!show_path_is_safe("C:x"));
	CHECK(!show_path_is_safe("a/\x01"));
	CHECK(show_name_is_valid("I'm Your Bus"));
	CHECK(show_name_is_valid("Caf\xC3\xA9"));
	CHECK(!show_name_is_valid(" lead"));
	CHECK(!show_name_is_valid("bad\xC3"));
	CHECK(!show_name_is_valid("a:b"));
	CHECK(show_is_uuid(ID));
	CHECK(!show_is_uuid("0f8fad5b-d9cb-469f-a165-70867728950"));
	char out[80], sha[65];
	show_asset_placeholder(SHA, out, sizeof(out));
	CHECK(show_asset_placeholder_parse(out, sha));
	CHECK_STR(sha, SHA);
	CHECK(!show_asset_placeholder_parse("${ctd-asset:abc}", sha));
}
