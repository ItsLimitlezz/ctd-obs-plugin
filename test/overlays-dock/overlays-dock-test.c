/* src/overlays-core.c: version order and update kinds, the links the CTD Overlays dock opens, and which browser
 * sources show an overlay. */
#include "overlays-core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static int sign(int v)
{
	return v < 0 ? -1 : v > 0;
}

static void order_is(const char *a, const char *b, int want)
{
	const int got = sign(ctd_semver_compare(a, b));
	const int back = sign(ctd_semver_compare(b, a));
	if (got != want || back != -want) {
		fprintf(stderr, "FAIL compare(%s, %s) = %d (and %d back), want %d\n", a ? a : "NULL", b ? b : "NULL",
			got, back, want);
		failures++;
	}
}

static void kind_is(const char *from, const char *to, enum ctd_update_kind want)
{
	const enum ctd_update_kind got = ctd_update_kind_of(from, to);
	if (got != want) {
		fprintf(stderr, "FAIL kind(%s → %s) = %d, want %d\n", from ? from : "NULL", to ? to : "NULL", got,
			want);
		failures++;
	}
}

static void str_is(const char *what, char *got, const char *want)
{
	if ((got == NULL) != (want == NULL) || (got && strcmp(got, want) != 0)) {
		fprintf(stderr, "FAIL %s: got %s, want %s\n", what, got ? got : "NULL", want ? want : "NULL");
		failures++;
	}
	free(got);
}

static void check(const char *what, bool got, bool want)
{
	if (got != want) {
		fprintf(stderr, "FAIL %s: got %d, want %d\n", what, got, want);
		failures++;
	}
}

int main(void)
{
	/* order */
	order_is("1.2.3", "1.2.3", 0);
	order_is("v1.2.3", "1.2.3", 0);
	order_is("1.2", "1.2.0", 0);
	order_is("1.2.3+build.7", "1.2.3", 0);
	order_is("1.2.3", "1.2.4", -1);
	order_is("1.9.0", "1.10.0", -1); /* numbers, not text */
	order_is("1.10.0", "2.0.0", -1);
	order_is("2.0.0-beta.1", "2.0.0", -1); /* a release sorts after its pre-releases */
	order_is("2.0.0-alpha", "2.0.0-beta", -1);
	order_is("2.0.0-beta.2", "2.0.0-beta.11", -1);
	order_is("2.0.0-1", "2.0.0-alpha", -1); /* numeric identifiers before text */
	order_is("2.0.0-beta", "2.0.0-beta.1", -1);
	order_is("latest", "0.0.1", -1); /* not a version: before every version */
	order_is(NULL, "1.0.0", -1);
	order_is("1.0.0x", "1.0.0", -1);
	order_is("junk", NULL, 0);

	/* update kinds: CTD Overlay's rule (docs/template-releases.md) */
	kind_is("1.2.3", "1.2.4", CTD_UPDATE_PATCH);
	kind_is("1.2.3", "1.3.0", CTD_UPDATE_MINOR);
	kind_is("1.2.3", "2.0.0", CTD_UPDATE_MAJOR);
	kind_is("0.2.0", "0.3.0", CTD_UPDATE_MAJOR); /* before 1.0.0 a minor bump counts as major */
	kind_is("0.2.0", "0.2.5", CTD_UPDATE_PATCH);
	kind_is("1.0.0", "1.1.0-rc.1", CTD_UPDATE_MAJOR); /* a pre-release waits */
	kind_is("1.3.0", "1.3.0", CTD_UPDATE_NONE);
	kind_is("1.3.0", "1.2.9", CTD_UPDATE_NONE);
	kind_is("1.3.0", NULL, CTD_UPDATE_NONE);
	kind_is("installed", "1.0.0", CTD_UPDATE_NONE);
	check("parse major", ctd_update_kind_parse("major") == CTD_UPDATE_MAJOR, true);
	check("parse minor", ctd_update_kind_parse("minor") == CTD_UPDATE_MINOR, true);
	check("parse patch", ctd_update_kind_parse("patch") == CTD_UPDATE_PATCH, true);
	check("parse other", ctd_update_kind_parse("MAJOR") == CTD_UPDATE_NONE, true);
	check("parse NULL", ctd_update_kind_parse(NULL) == CTD_UPDATE_NONE, true);

	/* admin UI links */
	str_is("control panel", ctd_overlay_admin_url("http://overlay.test:4848", "lower-third", false),
	       "http://overlay.test:4848/#/overlays/lower-third");
	str_is("upgrade screen", ctd_overlay_admin_url("https://overlay.test/", "club-bug", true),
	       "https://overlay.test/#/overlays/club-bug?tab=version");
	str_is("slug encoded", ctd_overlay_admin_url("http://overlay.test", "a b/c?d", false),
	       "http://overlay.test/#/overlays/a%20b%2Fc%3Fd");
	str_is("path kept", ctd_overlay_admin_url("http://nas.test:8080/overlay//", "x", false),
	       "http://nas.test:8080/overlay/#/overlays/x");
	str_is("no scheme", ctd_overlay_admin_url("overlay.test:4848", "x", false), NULL);
	str_is("javascript", ctd_overlay_admin_url("javascript:alert(1)", "x", false), NULL);
	str_is("file", ctd_overlay_admin_url("file:///etc", "x", false), NULL);
	str_is("credentials", ctd_overlay_admin_url("http://user:pw@overlay.test", "x", false), NULL);
	str_is("query", ctd_overlay_admin_url("http://overlay.test/?key=1", "x", false), NULL);
	str_is("no host", ctd_overlay_admin_url("http://", "x", false), NULL);
	str_is("no slug", ctd_overlay_admin_url("http://overlay.test", "", false), NULL);
	str_is("NULL ui", ctd_overlay_admin_url(NULL, "x", true), NULL);
	str_is("template", ctd_template_admin_url("http://overlay.test:4848/", "com.ctd.lower"),
	       "http://overlay.test:4848/#/templates/com.ctd.lower");
	str_is("template none", ctd_template_admin_url("http://overlay.test", NULL), NULL);

	/* GitHub releases */
	str_is("releases", ctd_github_releases_url("someone/lower-third", NULL, NULL),
	       "https://github.com/someone/lower-third/releases");
	str_is("release tag", ctd_github_releases_url("someone/lower.third_2", NULL, "1.3.0"),
	       "https://github.com/someone/lower.third_2/releases/tag/v1.3.0");
	str_is("release v tag", ctd_github_releases_url("someone/x", "", "v2.0.0"),
	       "https://github.com/someone/x/releases/tag/v2.0.0");
	str_is("tag prefix", ctd_github_releases_url("ItsLimitlezz/DPRTMNT-stream-Kit", "tn-v", "1.2.0"),
	       "https://github.com/ItsLimitlezz/DPRTMNT-stream-Kit/releases/tag/tn-v1.2.0");
	str_is("tag prefix encoded", ctd_github_releases_url("o/r", "a b/", "1.0.0"),
	       "https://github.com/o/r/releases/tag/a%20b%2F1.0.0");
	str_is("not a version", ctd_github_releases_url("someone/x", "tn-v", "latest"),
	       "https://github.com/someone/x/releases");
	str_is("no owner", ctd_github_releases_url("/x", NULL, NULL), NULL);
	str_is("no name", ctd_github_releases_url("someone/", NULL, NULL), NULL);
	str_is("three parts", ctd_github_releases_url("a/b/c", NULL, NULL), NULL);
	str_is("dotdot", ctd_github_releases_url("a/..", NULL, NULL), NULL);
	str_is("url", ctd_github_releases_url("https://evil.test/a", NULL, NULL), NULL);
	str_is("NULL repo", ctd_github_releases_url(NULL, NULL, "1.0.0"), NULL);

	/* which sources show an overlay */
	const char *o = "http://overlay.test:4848/o/lower?key=skp_ovl_abc";
	check("same", ctd_overlay_url_same(o, o), true);
	check("a scene of it", ctd_overlay_url_same("http://overlay.test:4848/o/lower?key=skp_ovl_abc&scene=brb", o),
	      true);
	check("scene first", ctd_overlay_url_same("http://overlay.test:4848/o/lower?scene=brb&key=skp_ovl_abc", o),
	      true);
	check("fragment", ctd_overlay_url_same("http://overlay.test:4848/o/lower?key=skp_ovl_abc#x", o), true);
	check("other key", ctd_overlay_url_same("http://overlay.test:4848/o/lower?key=skp_ovl_old", o), false);
	check("other canvas", ctd_overlay_url_same("http://overlay.test:4848/o/lower?key=skp_ovl_abc&canvas=v", o),
	      false);
	check("other overlay", ctd_overlay_url_same("http://overlay.test:4848/o/lower2?key=skp_ovl_abc", o), false);
	check("empty", ctd_overlay_url_same("", o), false);
	check("NULL", ctd_overlay_url_same(o, NULL), false);

	/* where the admin UI is when the summary doesn't say, and the slug of an overlay URL */
	str_is("origin", ctd_url_origin("http://overlay.test:4848/o/lower?key=skp_ovl_abc"),
	       "http://overlay.test:4848");
	str_is("origin https", ctd_url_origin("https://overlay.test"), "https://overlay.test");
	str_is("origin creds", ctd_url_origin("http://u:p@overlay.test/o/x"), NULL);
	str_is("origin other", ctd_url_origin("ftp://overlay.test/"), NULL);
	str_is("origin empty host", ctd_url_origin("http:///o/x"), NULL);
	str_is("slug", ctd_overlay_slug_of_url("http://overlay.test:4848/o/lower-third?key=skp_ovl_abc&canvas=v"),
	       "lower-third");
	str_is("slug encoded", ctd_overlay_slug_of_url("http://overlay.test/o/a%2Db"), "a-b");
	str_is("slug none", ctd_overlay_slug_of_url("http://overlay.test/overlay/x"), NULL);
	str_is("slug empty", ctd_overlay_slug_of_url("http://overlay.test/o/?key=1"), NULL);
	str_is("slug NULL", ctd_overlay_slug_of_url(NULL), NULL);

	/* search */
	check("contains", ctd_text_contains("Miami Or Bust · Lower third", "lower"), true);
	check("contains case", ctd_text_contains("club-bug", "CLUB"), true);
	check("not", ctd_text_contains("club-bug", "lower"), false);
	check("empty needle", ctd_text_contains("x", ""), true);
	check("NULL needle", ctd_text_contains(NULL, NULL), true);
	check("NULL text", ctd_text_contains(NULL, "x"), false);

	if (failures) {
		fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	printf("overlays-dock: all passed\n");
	return 0;
}
