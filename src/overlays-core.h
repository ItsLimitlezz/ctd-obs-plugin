/*
 * The CTD Overlays dock's pure logic (no libobs, no Qt): version comparison and update kinds for CTD Overlay's
 * templates, the links the dock opens in CTD Overlay's admin UI and on GitHub, and whether a browser source shows a
 * given overlay. Tested on every platform by test/overlays-dock.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SemVer order of two versions ("1.2.3", "v1.2.3", "2.0.0-beta.1"; a missing minor or patch counts as 0; build
 * metadata after '+' is ignored): <0, 0 or >0. Anything that is not a version sorts before every version. */
int ctd_semver_compare(const char *a, const char *b);

enum ctd_update_kind {
	CTD_UPDATE_NONE,  /* not newer, or not versions */
	CTD_UPDATE_PATCH, /* 1.2.3 → 1.2.4: CTD Overlay moves overlays by itself */
	CTD_UPDATE_MINOR, /* 1.2.3 → 1.3.0: by itself too */
	CTD_UPDATE_MAJOR, /* 1.x → 2.0, 0.2 → 0.3, or a pre-release on either side: waits for the operator */
};

/* What going from `from` to `to` is, by CTD Overlay's own rule (docs/template-releases.md). */
enum ctd_update_kind ctd_update_kind_of(const char *from, const char *to);

/* "none", "patch", "minor" or "major" (as CTD Overlay's summary says it) → the kind; anything else: NONE. */
enum ctd_update_kind ctd_update_kind_parse(const char *text);

/* An overlay's page in CTD Overlay's admin UI: "<ui>/#/overlays/<slug>", with "?tab=version" (the upgrade screen) when
 * upgrade is set. ui must be an http(s) address (a trailing '/' is dropped); slug is percent-encoded. A new string
 * (free()), or NULL when ui or slug is unusable. */
char *ctd_overlay_admin_url(const char *ui, const char *slug, bool upgrade);

/* A template's page in CTD Overlay's admin UI: "<ui>/#/templates/<id>". NULL as above. */
char *ctd_template_admin_url(const char *ui, const char *template_id);

/* The releases page of a GitHub repository "owner/name": "https://github.com/owner/name/releases" (with "/tag/v<ver>"
 * when version is set). NULL unless repo looks like owner/name. */
char *ctd_github_releases_url(const char *repo, const char *version);

/* Does a browser source showing `source_url` show the overlay at `overlay_url`, whatever scene= it pins? Both are
 * compared without their scene= parameters and #fragment; everything else (the key included) must be the same. */
bool ctd_overlay_url_same(const char *source_url, const char *overlay_url);

/* "scheme://host[:port]" of an http(s) URL (no credentials), or NULL: where CTD Overlay's admin UI is when its summary
 * doesn't say (an overlay's URL is on the same server). A new string (free()). */
char *ctd_url_origin(const char *url);

/* The overlay slug of a CTD Overlay overlay URL (".../o/<slug>?key=…"), decoded, or NULL. A new string (free()). */
char *ctd_overlay_slug_of_url(const char *url);

/* Case-insensitive (ASCII) "does text contain needle"; an empty or NULL needle matches everything. */
bool ctd_text_contains(const char *text, const char *needle);

#ifdef __cplusplus
}
#endif
