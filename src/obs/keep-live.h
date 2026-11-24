/*
 * Keep scenes live for CTD apps (bug B1): the "ctd" vendor requests KeepScenesLive, ReleaseScenesLive and
 * GetKeepScenesLive (docs/api.md).
 *
 * OBS only runs a scene's sources while something shows it: Program, studio-mode Preview, the Multiview or a
 * projector. CTD Live Studio's multiview reads the other scenes as obs-websocket screenshots, which show a scene for one
 * frame only, so browser, game/window/display capture and "deactivate when not showing" cameras hold their last frame.
 * While a client renews its lease, the plugin holds a show reference (obs_source_inc_showing) on each scene it named,
 * exactly as OBS's own Multiview does, so those scenes keep running.
 *
 * A lease lasts leaseSeconds (default 10, 3 to 30) and is released when it isn't renewed (obs-websocket doesn't tell
 * vendors when a client disconnects, so the lease is how a vanished client is noticed), when the client asks, when a
 * scene is removed or renamed, when the scene collection changes, when the setting is turned off and when the module
 * unloads. Show references are taken and given back on OBS's UI thread only, like the Multiview's; the bookkeeping is in
 * keep-live-core.c.
 *
 * Setting: "Let CTD apps keep multiview scenes live", on by default, saved in OBS's user config
 * ([CTDLive] KeepScenesLive).
 */

#pragma once

#include <obs-data.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* obs_module_load / obs_module_unload (UI thread) */
void keep_live_start(void);
void keep_live_stop(void);

bool keep_live_enabled(void);
/* UI thread. Saves the setting; turning it off releases every scene at once. */
void keep_live_set_enabled(bool enabled);

/* The vendor requests (src/obs/ws-vendor.c); any thread. */
void keep_live_request(obs_data_t *request, obs_data_t *response);
void keep_live_release_request(obs_data_t *request, obs_data_t *response);
void keep_live_status_request(obs_data_t *request, obs_data_t *response);

#ifdef __cplusplus
}
#endif
