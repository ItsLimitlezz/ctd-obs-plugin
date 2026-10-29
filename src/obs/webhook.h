/*
 * The webhook (SPEC I4): every event the plugin sends to obs-websocket (FeedStateChanged, IssueRaised, IssueCleared,
 * SyncStatusChanged, ProgramSceneChanged) is also POSTed as JSON to the webhook URLs set on the server profiles, for
 * things that don't speak OBS WebSocket (a graphics server showing "signal lost", say):
 *
 *   {"source": "ctd-obs-plugin", "type": "FeedStateChanged", "timeMs": 1761043200000, "data": {...}}
 *
 * An event naming a server profile (its data has "serverId") goes to that profile's webhook only; the rest go to every
 * profile's. One background thread sends them in order with a 1 s timeout each; fire and forget: a slow or failing
 * webhook drops events (the oldest first past 200 waiting) and never touches playback. The URL is a secret (it may
 * carry a token) and is never logged.
 */

#pragma once

#include <obs-data.h>

#ifdef __cplusplus
extern "C" {
#endif

void webhook_start(void);
void webhook_stop(void);
void webhook_post(const char *type, obs_data_t *data);

#ifdef __cplusplus
}
#endif
