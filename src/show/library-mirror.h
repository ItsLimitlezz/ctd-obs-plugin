/*
 * Mirror of the profile's CTD Relay asset library (Show Profiles ticket T14, design section 3.3). LUTs first.
 *
 * What it does: asks Relay for the library (GET /library?profile=<id>&kind=lut), downloads the files it doesn't have
 * (GET /blobs/<sha256>), and removes the ones that were deleted on Relay. The cache is a folder inside the plugin's own
 * config folder, which OBS can read (the Apply LUT filter's file picker points at it):
 *
 *   <plugin config>/library/<sha256>.cube   the files, named by their hash (a file with that hash is never fetched twice)
 *   <plugin config>/library/<sha256>.png
 *   <plugin config>/library/index.json      the readable index: name, file, sha256, size for every entry
 *
 * Everything happens on one worker thread of its own; library_mirror_request() only stores the request and returns, so
 * it is safe from any thread, the UI thread included. If Relay can't be reached the cache is left as it is, so the LUTs
 * already mirrored keep working offline. A request that arrives during a sync (the profile changed, another
 * library.changed) cancels the running transfer and starts over; files already finished are not fetched again.
 *
 * The caller (multistream.c) asks for a sync when the pairing completes, when the profile is chosen and on the
 * library.changed event. Never log or store the token.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the worker. Call once, after OBS has loaded the module (obs_module_config_path works). */
void library_mirror_init(void);
/* Stops the worker (aborting a transfer in progress) and waits for it. */
void library_mirror_shutdown(void);

/* "Sync now": api_base is Relay's API URL up to and including "/api/v1" (no trailing slash), token the bearer token,
 * profile_id the chosen profile. All three are copied. Returns at once; the sync runs on the worker. */
void library_mirror_request(const char *api_base, const char *token, const char *profile_id);

/* The cache folder (free it with bfree), or NULL before library_mirror_init. It exists after the first sync. */
char *library_mirror_dir(void);

#ifdef __cplusplus
}
#endif
