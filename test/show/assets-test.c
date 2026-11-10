/* Asset scanner/rewriter tests (ticket T9): kinds, known keys, the fallback scan, refs, resolve. */
#include "assets.h"
#include "manifest.h"
#include "show-fs.h"
#include "unit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Writes <dir>/<name> with content and returns its path with '/' separators (JSON-safe on Windows). */
static void make_file(const char *dir, const char *name, const char *content, char *path, size_t size)
{
	char full[1024];
	sfs_join(full, sizeof(full), dir, name);
	CHECK(sfs_write_atomic(full, content, strlen(content)));
	snprintf(path, size, "%s", full);
	for (char *p = path; *p; p++)
		if (*p == '\\')
			*p = '/';
}

static cj *parse_text(const char *text)
{
	char err[256];
	cj *v = cj_parse(text, strlen(text), err, sizeof(err));
	if (!v) {
		unit_failures++;
		fprintf(stderr, "assets-test: bad JSON: %s\n", err);
	}
	return v;
}

static void test_kinds(void)
{
	CHECK(strcmp(show_asset_kind("clut_filter", "x.png"), "lut") == 0);
	CHECK(strcmp(show_asset_kind("clut_filter_v2", NULL), "lut") == 0);
	CHECK(strcmp(show_asset_kind("image_source", "a/b/WARM.CUBE"), "lut") == 0);
	CHECK(strcmp(show_asset_kind("image_source", "C:\\x\\Logo.PNG"), "image") == 0);
	CHECK(strcmp(show_asset_kind(NULL, "a.jpeg"), "image") == 0);
	CHECK(strcmp(show_asset_kind(NULL, "a.webp"), "image") == 0);
	CHECK(strcmp(show_asset_kind("ffmpeg_source", "/x/clip.MkV"), "video") == 0);
	CHECK(strcmp(show_asset_kind(NULL, "a.mxf"), "video") == 0);
	CHECK(strcmp(show_asset_kind(NULL, "a.opus"), "audio") == 0);
	CHECK(strcmp(show_asset_kind(NULL, "a.flac"), "audio") == 0);
	CHECK(strcmp(show_asset_kind(NULL, "a.otf"), "font") == 0);
	CHECK(strcmp(show_asset_kind(NULL, "a.TTF"), "font") == 0);
	CHECK(strcmp(show_asset_kind("browser_source", "a.htm"), "html") == 0);
	CHECK(strcmp(show_asset_kind(NULL, "a.html"), "html") == 0);
	CHECK(strcmp(show_asset_kind(NULL, "a.bin"), "other") == 0);
	CHECK(strcmp(show_asset_kind(NULL, "noext"), "other") == 0);
	CHECK(strcmp(show_asset_kind(NULL, "dir.png/noext"), "other") == 0);
	CHECK(strcmp(show_asset_kind(NULL, NULL), "other") == 0);
}

struct hash_count {
	int calls;
	bool fail;
};

#include "sha256.h"
static bool counting_hash(void *opaque, const char *path, char sha[65], int64_t *size)
{
	struct hash_count *c = opaque;
	c->calls++;
	if (c->fail)
		return false;
	return sha256_file(path, sha, size, NULL, NULL);
}

/* the local file for an asset: found in asset_sources */
static bool path_from_sources(void *opaque, const cj *asset, char *out, size_t size)
{
	const char *p = cj_get_str((const cj *)opaque, cj_get_str(asset, "sha256"));
	if (!p)
		return false;
	snprintf(out, size, "%s", p);
	return true;
}

static bool path_never(void *opaque, const cj *asset, char *out, size_t size)
{
	(void)opaque, (void)asset, (void)out, (void)size;
	return false;
}

static const cj *find_asset_by_name(const cj *manifest, const char *name)
{
	const cj *assets = cj_get(manifest, "assets");
	for (size_t i = 0; i < cj_size(assets); i++)
		if (strcmp(cj_get_str(cj_at(assets, i), "name") ? cj_get_str(cj_at(assets, i), "name") : "", name) == 0)
			return cj_at(assets, i);
	return NULL;
}

static void test_rewrite_and_resolve(void)
{
	char dir[1024], img[1100], font[1100], gone[1100], aux[1100], other[1100], text[16384], err[256];
	unit_scratch_dir("assets-rewrite", dir, sizeof(dir));
	make_file(dir, "Img.PNG", "image-bytes", img, sizeof(img));
	make_file(dir, "Face.ttf", "font-bytes", font, sizeof(font));
	make_file(dir, "track.wav", "audio-bytes", aux, sizeof(aux));
	make_file(dir, "notes.bin", "other-bytes", other, sizeof(other));
	snprintf(gone, sizeof(gone), "%s/gone.png", dir);

	snprintf(
		text, sizeof(text),
		"{\"sources\":["
		" {\"id\":\"image_source\",\"uuid\":\"U1\",\"name\":\"Img\",\"settings\":{\"file\":\"%s\"},"
		"  \"filters\":[{\"id\":\"mask_filter\",\"uuid\":\"F1\",\"settings\":{\"image_path\":\"%s\"}}]},"
		" {\"versioned_id\":\"text_gdiplus_v2\",\"uuid\":\"U2\",\"name\":\"Txt\","
		"  \"settings\":{\"read_from_file\":true,\"file\":\"%s\"}},"
		" {\"id\":\"ffmpeg_source\",\"uuid\":\"U3\",\"name\":\"Net\","
		"  \"settings\":{\"is_local_file\":false,\"local_file\":\"%s\",\"input\":\"srt://example.invalid:1\"}},"
		" {\"id\":\"color_source_v3\",\"name\":\"NoUuid\",\"settings\":{\"deep\":{\"list\":[\"x\",\"%s\"]},"
		"  \"other\":\"%s\",\"nofile\":\"%s/nothing-here.bin\",\"dir\":\"%s\"}},"
		" {\"id\":\"browser_source\",\"uuid\":\"U5\",\"name\":\"Web\",\"settings\":{\"url\":\"http://example.com/x\","
		"  \"local_file\":\"\",\"alt\":\"file:///tmp/x.html\"}}],"
		"\"groups\":[],\"transitions\":[],"
		"\"AuxAudioDevice1\":{\"id\":\"wasapi_input_capture\",\"uuid\":\"U6\",\"name\":\"Aux\","
		"  \"settings\":{\"jingle\":\"%s\"}},"
		"\"current_scene\":\"x\",\"hotkeys\":{\"id\":\"nope\"}}",
		img, img, gone, gone, font, other, dir, dir, aux);

	cj *coll = parse_text(text), *orig = cj_clone(coll);
	cj *manifest = show_manifest_new("11111111-1111-4111-8111-111111111111", "T");
	cj *sources = cj_new_object(), *missing = cj_new_array();
	struct hash_count hc = {0, false};
	CHECK(show_assets_rewrite(coll, manifest, sources, missing, counting_hash, &hc, err, sizeof(err)));

	/* Img.PNG: two refs (source file + filter image_path), merged into one entry; hashed once */
	const cj *a = find_asset_by_name(manifest, "Img.PNG");
	CHECK(a != NULL);
	CHECK(cj_size(cj_get(a, "refs")) == 2);
	CHECK_STR(cj_get_str(a, "kind"), "image");
	CHECK(cj_get_int(a, "size", -1) == 11);
	CHECK(show_is_sha256(cj_get_str(a, "sha256")));
	const cj *r0 = cj_at(cj_get(a, "refs"), 0), *r1 = cj_at(cj_get(a, "refs"), 1);
	CHECK_STR(cj_get_str(r0, "source"), "U1");
	CHECK(cj_kind(cj_get(r0, "filter")) == CJ_NULL);
	CHECK_STR(cj_get_str(r0, "key"), "/settings/file");
	CHECK_STR(cj_get_str(r1, "source"), "U1");
	CHECK_STR(cj_get_str(r1, "filter"), "F1");
	CHECK_STR(cj_get_str(r1, "key"), "/settings/image_path");
	CHECK_STR(cj_get_str(sources, cj_get_str(a, "sha256")), img);
	char ph[96];
	show_asset_placeholder(cj_get_str(a, "sha256"), ph, sizeof(ph));
	CHECK_STR(cj_get_str(cj_pointer(coll, "/sources/0/settings"), "file"), ph);
	CHECK_STR(cj_get_str(cj_pointer(coll, "/sources/0/filters/0/settings"), "image_path"), ph);

	/* fallback finds the font deep in an unknown type; a source without a uuid has a null source */
	const cj *f = find_asset_by_name(manifest, "Face.ttf");
	CHECK(f != NULL && strcmp(cj_get_str(f, "kind"), "font") == 0);
	CHECK(cj_kind(cj_get(cj_at(cj_get(f, "refs"), 0), "source")) == CJ_NULL);
	CHECK_STR(cj_get_str(cj_at(cj_get(f, "refs"), 0), "key"), "/settings/deep/list/1");
	const cj *o = find_asset_by_name(manifest, "notes.bin");
	CHECK(o != NULL && strcmp(cj_get_str(o, "kind"), "other") == 0);

	/* a global audio device object is scanned; objects that are not sources are not */
	const cj *w = find_asset_by_name(manifest, "track.wav");
	CHECK(w != NULL && strcmp(cj_get_str(w, "kind"), "audio") == 0);
	CHECK_STR(cj_get_str(cj_at(cj_get(w, "refs"), 0), "source"), "U6");

	/* nothing else: Img, Face, notes, aux */
	CHECK(cj_size(cj_get(manifest, "assets")) == 4);
	CHECK(cj_size(sources) == 4);
	CHECK(hc.calls == 4);

	/* a known key whose file is gone stays as it is and is reported; the unused local_file (is_local_file=false) is
	 * not reported; the fallback ignores a missing path and a directory */
	CHECK(cj_size(missing) == 1);
	CHECK_STR(cj_get_str(cj_at(missing, 0), "source"), "U2");
	CHECK_STR(cj_get_str(cj_at(missing, 0), "key"), "/settings/file");
	CHECK_STR(cj_get_str(cj_at(missing, 0), "path"), gone);
	CHECK_STR(cj_get_str(cj_pointer(coll, "/sources/1/settings"), "file"), gone);
	CHECK_STR(cj_get_str(cj_pointer(coll, "/sources/2/settings"), "local_file"), gone);
	CHECK_STR(cj_get_str(cj_pointer(coll, "/sources/3/settings"), "dir"), dir);
	CHECK_STR(cj_get_str(cj_pointer(coll, "/sources/4/settings"), "alt"), "file:///tmp/x.html");
	CHECK_STR(cj_get_str(cj_pointer(coll, "/hotkeys"), "id"), "nope");

	/* no original path survives in the printed collection */
	char *printed = cj_print(coll, false);
	CHECK(!unit_contains(printed, img));
	CHECK(!unit_contains(printed, font));
	CHECK(!unit_contains(printed, aux));
	cj_free_str(printed);

	/* resolve back to the same paths: exactly the original document */
	CHECK(show_assets_resolve(coll, manifest, path_from_sources, sources, err, sizeof(err)));
	CHECK(cj_equal(coll, orig));

	/* resolve is all or nothing */
	cj *again = cj_clone(orig);
	CHECK(show_assets_rewrite(again, manifest, NULL, NULL, NULL, NULL, err, sizeof(err)));
	cj *rewritten = cj_clone(again);
	CHECK(!cj_equal(again, orig));
	CHECK(!show_assets_resolve(again, manifest, path_never, NULL, err, sizeof(err)));
	CHECK(err[0] != '\0');
	CHECK(cj_equal(again, rewritten));
	cj *bad = cj_get_or_add_object(again, "extra");
	cj_set_str(bad, "x", "${ctd-asset:0000000000000000000000000000000000000000000000000000000000000000}");
	CHECK(!show_assets_resolve(again, manifest, path_from_sources, sources, err, sizeof(err)));
	CHECK(unit_contains(err, "not in the manifest"));
	CHECK(unit_contains(cj_get_str(cj_pointer(again, "/sources/0/settings"), "file"), "${ctd-asset:"));

	/* hashing failure fails the call */
	cj *c2 = cj_clone(orig), *m2 = show_manifest_new("11111111-1111-4111-8111-111111111111", "T");
	hc.fail = true;
	CHECK(!show_assets_rewrite(c2, m2, NULL, NULL, counting_hash, &hc, err, sizeof(err)));
	CHECK(unit_contains(err, "Img.PNG"));

	cj_free(c2);
	cj_free(m2);
	cj_free(rewritten);
	cj_free(again);
	cj_free(coll);
	cj_free(orig);
	cj_free(manifest);
	cj_free(sources);
	cj_free(missing);
}

static void test_known_lists(void)
{
	char dir[1024], a[1100], b[1100], text[16384], err[256];
	unit_scratch_dir("assets-lists", dir, sizeof(dir));
	make_file(dir, "a.mp4", "AAAA", a, sizeof(a));
	make_file(dir, "b.png", "BBBBBB", b, sizeof(b));
	snprintf(
		text, sizeof(text),
		"{\"sources\":["
		"{\"id\":\"vlc_source\",\"uuid\":\"V\",\"settings\":{\"playlist\":[{\"value\":\"%s\"},{\"value\":\"%s\"},"
		"{\"value\":\"%s\"}]}},"
		"{\"id\":\"slideshow_v2\",\"uuid\":\"S\",\"settings\":{\"files\":[{\"value\":\"%s\"}]}}],"
		"\"transitions\":[{\"id\":\"obs_stinger_transition\",\"uuid\":\"T\",\"settings\":{\"path\":\"%s\","
		"\"track_matte_path\":\"%s/nope.mov\"}}]}",
		a, b, a, b, a, dir);
	cj *coll = parse_text(text), *orig = cj_clone(coll);
	cj *m = show_manifest_new("11111111-1111-4111-8111-111111111111", "T");
	cj *missing = cj_new_array();
	CHECK(show_assets_rewrite(coll, m, NULL, missing, NULL, NULL, err, sizeof(err)));
	CHECK(cj_size(cj_get(m, "assets")) == 2);
	const cj *aa = find_asset_by_name(m, "a.mp4");
	CHECK(aa && cj_size(cj_get(aa, "refs")) == 3);
	CHECK_STR(cj_get_str(cj_at(cj_get(aa, "refs"), 1), "key"), "/settings/playlist/2/value");
	CHECK_STR(cj_get_str(cj_at(cj_get(aa, "refs"), 2), "source"), "T");
	CHECK_STR(cj_get_str(cj_at(cj_get(aa, "refs"), 2), "key"), "/settings/path");
	CHECK(cj_size(missing) == 1);
	CHECK_STR(cj_get_str(cj_at(missing, 0), "key"), "/settings/track_matte_path");
	/* the entries pass manifest validation once the manifest has its files.collection */
	cj_set_str(cj_get_or_add_object(m, "files"), "collection", "collection.json");
	CHECK(show_manifest_validate(m, err, sizeof(err)));
	cj_free(coll);
	cj_free(orig);
	cj_free(m);
	cj_free(missing);
}

void test_assets(void)
{
	test_kinds();
	test_rewrite_and_resolve();
	test_known_lists();
}
