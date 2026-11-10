/* Export the current OBS setup into a bundle (ticket T9, design sections 1 and 4 "Save up" steps 2-3).
 * Pure C, no libobs: the plugin's UI thread saves OBS (obs_frontend_save) and finds the files; this reads them.
 *
 * Steps: read the collection -> show_scrub_collection (T10) -> show_assets_rewrite -> devices[] (devices_identity,
 * T6) -> profile files (basic.ini through show_ini_filter + show_scrub_ini; stream/record encoder JSON through
 * show_scrub_settings; service.json through show_scrub_service) -> manifest fields -> show_manifest_validate. */
#ifndef CTD_SHOW_EXPORT_H
#define CTD_SHOW_EXPORT_H

#include "assets.h"
#include "bundle.h"
#include "json.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct show_export_input {
	const char *id;              /* show profile id (uuid) */
	const char *name;            /* show profile name */
	const char *collection_path; /* the scene collection JSON file, just saved */
	const char *profile_dir;     /* the OBS profile folder (basic.ini, service.json, *Encoder.json) */
	const char *app_id;          /* manifest.savedBy.appId */
	const char *os;              /* "windows", "macos", "linux" */
	const char *obs_version, *ctd_obs_version, *plugin_version; /* manifest.versions ("" when unknown) */
	const char *saved_at;                                       /* ISO 8601 UTC, e.g. "2026-11-05T20:00:00Z" */
	const cj *previous_manifest; /* the profile's head manifest, or NULL: relayProfileId and revisions carry over */
	show_hash_fn hash;           /* NULL: plain sha256_file */
	void *hash_opaque;
};

/* Fills out (show_bundle_init'd by the caller). On failure out may be partly filled: show_bundle_free it. warnings
 * (an array, may be NULL) gets one string per thing the user should know (a missing asset file, ...). */
bool show_export(const struct show_export_input *in, struct show_bundle *out, cj *warnings, char *err, size_t err_size);

#ifdef __cplusplus
}
#endif

#endif
