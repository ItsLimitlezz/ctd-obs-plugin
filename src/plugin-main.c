/*
 * CTD OBS Plugin: module registration.
 * GPL-2.0-or-later (links against libobs and FFmpeg inside OBS).
 */

#include <obs-module.h>
#include <plugin-support.h>

#include <curl/curl.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-ctd-live-source", "en-US")

#include "multistream.h"
#include "obs/obs-watch.h"
#include "obs/servers.h"
#include "api/poller.h"
#include "obs/ws-vendor.h"
#include "obs/issues.h"
#include "obs/keep-live.h"
#include "obs/sync-groups.h"
#include "obs/webhook.h"
#include "show/show.h"
#include "obs/ctd-settings.h"

extern struct obs_source_info ctd_live_source_info;
extern struct obs_source_info ctd_live_audio_source_info;

/* src/ui/multistream-dock.cpp, src/ui/feeds-dock.cpp */
void ctd_multistream_dock_register(void);
void ctd_feeds_dock_register(void);
/* src/ui/settings-section.cpp: CTD OBS's Settings ▸ CTD page */
void ctd_settings_section_register(void);
/* src/ui/producer-dock.cpp: the CTD Producer dock (viewers per platform and the merged chat) */
void ctd_producer_dock_register(void);
/* src/ui/overlays-dock.cpp: the CTD Overlays dock (CTD Overlay's overlays, updates and the template kit) */
void ctd_overlays_dock_register(void);
void ctd_overlays_dock_shutdown(void);

/* What OBS shows for this module (the plugin list, the log). Display text only; the module's file name and its source
 * type ids stay as they are, so saved scenes keep loading. */
OBS_MODULE_AUTHOR("CineTech Digital")

MODULE_EXPORT const char *obs_module_name(void)
{
	return "CTD Live Source";
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Receive camera feeds from CTD Live apps and control CTD Relay.";
}

bool obs_module_load(void)
{
	/* file downloads (http_get_file) use libcurl; the call is counted, OBS makes its own */
	curl_global_init(CURL_GLOBAL_DEFAULT);
	ctd_settings_init(); /* before anything reads a settings file: CTD OBS's CTD folder (gap 17) */
	servers_init();      /* before any source is created */
	obs_register_source(&ctd_live_source_info);
	obs_register_source(&ctd_live_audio_source_info);
	ms_init();
	obs_watch_start();
	ctd_multistream_dock_register();
	ctd_servers_dialog_register();
	ctd_producer_dock_register(); /* after the CTD Relay dock, whose connection it shows */
	ctd_overlays_dock_register(); /* the same */
	ctd_feeds_dock_register();
	keep_live_start(); /* B1: CTD apps' multiview scenes; reads its setting before Settings ▸ CTD shows it */
	ctd_settings_section_register();
	show_init(); /* Show Profiles: turns itself on once OBS has loaded, only inside CTD OBS */
	blog(LOG_INFO, "[CTD] live-source: plugin loaded (version %s)", PLUGIN_VERSION);
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
	keep_live_stop(); /* gives back any show references still held */
	show_shutdown();  /* waits for a running save or switch step */
	sync_groups_stop();
	issues_stop();
	obs_watch_stop();
	webhook_stop(); /* after everything that sends events */
	ms_shutdown();
	mtx_pollers_shutdown(); /* the sources are gone by now; anything else still holding one */
	servers_shutdown();
	ctd_overlays_dock_shutdown(); /* its template-kit downloads use libcurl */
	ctd_settings_shutdown();
	curl_global_cleanup();
}
