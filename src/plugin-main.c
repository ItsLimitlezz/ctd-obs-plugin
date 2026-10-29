/*
 * CTD OBS Plugin: module registration.
 * GPL-2.0-or-later (links against libobs and FFmpeg inside OBS).
 */

#include <obs-module.h>
#include <plugin-support.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-ctd-live-source", "en-US")

#include "multistream.h"
#include "obs/obs-watch.h"
#include "obs/servers.h"
#include "api/poller.h"
#include "obs/ws-vendor.h"
#include "obs/issues.h"
#include "obs/sync-groups.h"
#include "obs/webhook.h"

extern struct obs_source_info ctd_live_source_info;
extern struct obs_source_info ctd_live_audio_source_info;

/* src/ui/multistream-dock.cpp, src/ui/hub-dock.cpp */
void ctd_multistream_dock_register(void);
void ctd_hub_dock_register(void);

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Live-edge SRT source for MediaMTX";
}

bool obs_module_load(void)
{
	servers_init(); /* before any source is created */
	obs_register_source(&ctd_live_source_info);
	obs_register_source(&ctd_live_audio_source_info);
	ms_init();
	obs_watch_start();
	ctd_multistream_dock_register();
	ctd_servers_dialog_register();
	ctd_hub_dock_register();
	blog(LOG_INFO, "[ctd-live] plugin loaded (version %s)", PLUGIN_VERSION);
	return true;
}

/* obs-websocket has loaded by now (I1) */
void obs_module_post_load(void)
{
	ws_vendor_register();
	issues_start();      /* D1: after the vendor, so its first issues reach obs-websocket */
	sync_groups_start(); /* S9 */
	webhook_start();     /* I4 */
}

void obs_module_unload(void)
{
	sync_groups_stop();
	issues_stop();
	obs_watch_stop();
	webhook_stop(); /* after everything that sends events */
	ms_shutdown();
	mtx_pollers_shutdown(); /* the sources are gone by now; anything else still holding one */
	servers_shutdown();
}
