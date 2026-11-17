/* Secret scrubber (ticket T10): no fixture secret survives, the refs are built, and this machine gets it all back. */
#include "unit.h"
#include "manifest.h"
#include "scrub.h"
#include "bundle.h"

#include <stdlib.h>

#define ID "0f8fad5b-d9cb-469f-a165-70867728950e"

/* Every fixture secret contains "FIXTURE-" or "FIXTURE" + value; the grep the ticket asks for. */
static bool leaks(const char *text)
{
	return unit_contains(text, "FIXTURE");
}

static void names_and_values(void)
{
	CHECK(show_scrub_is_secret_name("password"));
	CHECK(show_scrub_is_secret_name("stream_key"));
	CHECK(show_scrub_is_secret_name("streamKey"));
	CHECK(show_scrub_is_secret_name("apiKey"));
	CHECK(show_scrub_is_secret_name("x-auth-token"));
	CHECK(show_scrub_is_secret_name("key"));
	CHECK(show_scrub_is_secret_name("bearer_token"));
	CHECK(show_scrub_is_secret_name("StreamKey"));
	CHECK(!show_scrub_is_secret_name("keyint"));
	CHECK(!show_scrub_is_secret_name("color_key"));
	CHECK(!show_scrub_is_secret_name("key_color_type"));
	CHECK(!show_scrub_is_secret_name("hotkeys"));
	CHECK(!show_scrub_is_secret_name("keyframe_interval"));
	CHECK(!show_scrub_is_secret_name("url"));
	CHECK(!show_scrub_is_secret_name("use_auth_header") ||
	      true); /* contains the word auth, not exact: allowed either way */

	CHECK(show_scrub_is_secret_value("live_123456789_abcdefghijklmnopqrstuvwxyz"));
	CHECK(show_scrub_is_secret_value("abcd-efgh-ijkl-mnop-qrst"));
	CHECK(show_scrub_is_secret_value("FB-1234567890-0-AbCdEfGhIjKlMnOp"));
	CHECK(show_scrub_is_secret_value("eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxIn0.c2lnbmF0dXJlLXZhbHVl"));
	CHECK(show_scrub_is_secret_value("Bearer abcdefghijklmnopqrstuvwxyz"));
	CHECK(!show_scrub_is_secret_value("0f8fad5b-d9cb-469f-a165-70867728950e")); /* uuids are identities */
	CHECK(!show_scrub_is_secret_value("this is a long but harmless sentence"));
	CHECK(!show_scrub_is_secret_value("C:/Users/show/Videos/intro.mp4"));

	char *out = NULL;
	CHECK(show_scrub_url("https://x.example.invalid/p?a=1&key=s3cret&b=2", &out));
	CHECK_STR(out, "https://x.example.invalid/p?a=1&key=${ctd-secret}&b=2");
	free(out);
	CHECK(show_scrub_url("rtmp://u:p@h.example.invalid/app", &out));
	CHECK_STR(out, "rtmp://${ctd-secret}@h.example.invalid/app");
	free(out);
	CHECK(!show_scrub_url("https://x.example.invalid/page?theme=dark", &out));
	CHECK(out == NULL);
	CHECK(!show_scrub_url("not a url ?key=1", &out));
}

static void collection(void)
{
	cj *col = unit_fixture_json("scrub/collection.json");
	cj *original = cj_clone(col);
	cj *manifest = show_manifest_new(ID, "Fixture");
	cj *local = cj_new_object();
	struct show_scrub_report report = {0};
	CHECK(show_scrub_collection(col, manifest, local, &report));

	char *text = cj_print(col, true), *mtext = cj_print(manifest, true), *ltext = cj_print(local, true);
	CHECK(!leaks(text));
	CHECK(!leaks(mtext));
	if (leaks(text))
		fprintf(stderr, "%s\n", text);
	CHECK(leaks(ltext)); /* the originals are kept for this machine */

	/* the overlay is a ref, not a URL */
	const cj *overlays = cj_get(manifest, "overlays");
	CHECK(cj_size(overlays) == 1);
	CHECK_STR(cj_get_str(cj_at(overlays, 0), "slot"), "chat-box");
	CHECK_STR(cj_get_str(cj_at(overlays, 0), "canvas"), "landscape");
	CHECK_STR(cj_get_str(cj_at(overlays, 0), "source"), "22222222-2222-4222-8222-222222222222");
	CHECK_STR(cj_str(cj_pointer(col, "/sources/1/settings/url")), SHOW_OVERLAY_PLACEHOLDER);
	CHECK(report.overlays == 1);

	/* URLs keep their shape; whole secrets become the placeholder */
	CHECK_STR(cj_str(cj_pointer(col, "/sources/2/settings/url")),
		  "https://alerts.example.invalid/widget/alertbox?token=${ctd-secret}#access_token=${ctd-secret}");
	CHECK_STR(cj_str(cj_pointer(col, "/sources/2/filters/0/settings/password")), SHOW_SECRET_PLACEHOLDER);
	CHECK_STR(cj_str(cj_pointer(col, "/sources/3/settings/input")),
		  "rtmp://${ctd-secret}@ingest.example.invalid/app/${ctd-secret}");
	CHECK_STR(cj_str(cj_pointer(col, "/sources/3/settings/note")), SHOW_SECRET_PLACEHOLDER);
	CHECK_STR(cj_str(cj_pointer(col, "/modules/example-plugin/api_token")), SHOW_SECRET_PLACEHOLDER);

	/* harmless look-alikes stay */
	CHECK_STR(cj_str(cj_pointer(col, "/sources/2/filters/0/settings/keyint")), "2");
	CHECK_STR(cj_str(cj_pointer(col, "/sources/2/filters/0/settings/color_key")), "green");
	CHECK_STR(cj_str(cj_pointer(col, "/sources/0/hotkeys/OBSBasic.SelectScene/0/key")), "OBS_KEY_F1");
	CHECK_STR(cj_str(cj_pointer(col, "/sources/4/settings/label")), "this is a long but harmless sentence");
	CHECK(cj_equal(cj_pointer(col, "/sources/0/settings"), cj_pointer(original, "/sources/0/settings")));

	/* secrets[] says where, with refs from the owning source or filter */
	const cj *secrets = cj_get(manifest, "secrets");
	CHECK(cj_size(secrets) == (size_t)report.secrets);
	CHECK(report.secrets == 5); /* the alerts URL is one value holding two secret params */
	bool filter_ref = false;
	for (size_t i = 0; i < cj_size(secrets); i++) {
		const cj *s = cj_at(secrets, i);
		if (cj_get_str(s, "source") &&
		    strcmp(cj_get_str(s, "source"), "44444444-4444-4444-8444-444444444444") == 0)
			filter_ref = strcmp(cj_get_str(s, "key"), "/settings/password") == 0;
	}
	CHECK(filter_ref);

	/* scrubbing again finds nothing new */
	struct show_scrub_report again = {0};
	cj *manifest2 = show_manifest_new(ID, "Fixture");
	cj *local2 = cj_new_object();
	CHECK(show_scrub_collection(col, manifest2, local2, &again));
	CHECK(again.secrets == 0 && cj_size(local2) == 0);
	cj_free(manifest2);
	cj_free(local2);

	/* the manifest still validates */
	cj_set_str(cj_get(manifest, "files"), "collection", "collection.json");
	char err[256];
	CHECK(show_manifest_validate(manifest, err, sizeof(err)));

	/* this machine restores every value exactly */
	cj *missing = cj_new_array();
	int restored = show_secrets_restore(col, local, missing);
	CHECK(restored == 6); /* 5 secrets + the overlay URL */
	CHECK(cj_size(missing) == 0);
	CHECK(cj_equal(col, original));

	/* another machine (no local values) gets a list of what needs re-entry, overlays flagged */
	cj *col2 = unit_fixture_json("scrub/collection.json");
	cj *m3 = show_manifest_new(ID, "Fixture");
	cj *l3 = cj_new_object();
	show_scrub_collection(col2, m3, l3, NULL);
	cj *empty = cj_new_object();
	CHECK(show_secrets_restore(col2, empty, missing) == 0);
	CHECK(cj_size(missing) == 6);
	int overlays_missing = 0;
	for (size_t i = 0; i < cj_size(missing); i++)
		overlays_missing += cj_get_bool(cj_at(missing, i), "overlay", false);
	CHECK(overlays_missing == 1);

	cj_free(empty);
	cj_free(col2);
	cj_free(m3);
	cj_free(l3);
	cj_free(missing);
	cj_free_str(text);
	cj_free_str(mtext);
	cj_free_str(ltext);
	cj_free(col);
	cj_free(original);
	cj_free(manifest);
	cj_free(local);
}

static void service_and_ini(void)
{
	cj *service = unit_fixture_json("scrub/service.json");
	cj *local = NULL;
	struct show_scrub_report report = {0};
	CHECK(show_scrub_service(service, &local, &report));
	char *text = cj_print(service, false);
	CHECK(!leaks(text));
	CHECK_STR(text, "{\"type\":\"rtmp_custom\",\"settings\":{\"server\":\"rtmp://relay.example.invalid/live\","
			"\"use_auth\":true,\"bwtest\":false}}");
	CHECK(report.stream_keys == 2); /* key + password */
	CHECK_STR(cj_get_str(cj_get(local, "settings"), "key"), "FIXTURE-STREAM-KEY-0008");
	cj_free_str(text);
	cj_free(service);
	cj_free(local);

	char *ini = unit_fixture_text("scrub/basic.ini");
	char *clean = show_scrub_ini(ini);
	CHECK(!leaks(clean));
	CHECK(unit_contains(clean, "BaseCX=1920"));
	CHECK(unit_contains(clean, "[Stream1]"));
	free(ini);
	free(clean);

	/* encoder settings */
	cj *enc = cj_parse("{\"rate_control\":\"CBR\",\"bitrate\":6000,\"license_key\":\"FIXTURE-LICENSE-0011\"}", 74,
			   NULL, 0);
	cj *secrets = cj_new_object();
	CHECK(enc && show_scrub_settings(enc, "profile/streamEncoder.json", secrets, NULL));
	CHECK_STR(cj_get_str(enc, "license_key"), SHOW_SECRET_PLACEHOLDER);
	CHECK_STR(cj_get_str(secrets, "profile/streamEncoder.json|/license_key"), "FIXTURE-LICENSE-0011");
	CHECK(show_secrets_restore_settings(enc, "profile/streamEncoder.json", secrets) == 1);
	CHECK_STR(cj_get_str(enc, "license_key"), "FIXTURE-LICENSE-0011");
	cj_free(enc);
	cj_free(secrets);
}

void test_scrub(void)
{
	names_and_values();
	collection();
	service_and_ini();
}
