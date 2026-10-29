/*
 * The obs-websocket vendor "ctd" (SPEC I1 to I3).
 *
 * Requests (CallVendorRequest, vendorName "ctd"): GetServers, GetStreams {serverId?}, GetFeeds, GetFeed {sourceName},
 * ReconnectFeed {sourceName}, GetIssues. Events (VendorEvent): FeedStateChanged {sourceName, path, state, previous},
 * IssueRaised and IssueCleared {id, rule, severity, source, path, title, cause, firstSeen}.
 */

#pragma once

#include <obs-data.h>

#ifdef __cplusplus
extern "C" {
#endif

/* After every module has loaded (obs_module_post_load), so obs-websocket is there to register with. */
void ws_vendor_register(void);
/* Any thread. Does nothing without obs-websocket. */
void ws_vendor_emit(const char *event_type, obs_data_t *data);

#ifdef __cplusplus
}
#endif
