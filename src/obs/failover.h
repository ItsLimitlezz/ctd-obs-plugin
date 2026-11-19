/*
 * Auto-failover on dead feeds (CTD OBS F2): when a CTD Live Source that is visible on the program scene goes down
 * (no signal, colour bars, a saved picture, or frozen if asked), program moves to another scene: that source's own
 * scene, or the first scene of the fallback list that shows no down source. The rules are failover-core.c (and its
 * tests); this is the OBS side.
 *
 * Settings belong to the scene collection (they name its scenes and sources) and travel in its file. Every switch
 * goes to the OBS log, obs-websocket (FailoverTriggered) and CTD Relay (obs.failover, Relay API 1.12), so CTD Live
 * can tell the operator. The main canvas only: the vertical canvas follows in a later change.
 */

#pragma once

#include "failover-core.h"

#ifdef __cplusplus
extern "C" {
#endif

void failover_start(void);
void failover_stop(void);

/* The current scene collection's settings (a copy), and replacing them. Setting them marks nothing dirty: the
 * caller saves the collection (obs_frontend_save) when the user has confirmed. */
void failover_get_config(struct fo_config *out);
void failover_set_config(const struct fo_config *in);

/* The latest picture of a CTD Live Source, to save as "this is what its dead feed looks like". UI thread. */
bool failover_capture_picture(const char *source, uint8_t out[FO_GRID_BYTES]);

struct failover_status {
	char program[FO_NAME_MAX];
	char last[256];     /* the last switch, as shown to people; "" when there was none */
	int64_t last_at_ms; /* Unix epoch ms */
};
void failover_get_status(struct failover_status *out);

/* Whether a CTD Live Source is visible on the program scene right now (as of the last check, half a second ago at
 * most), and on which scene. For the issues the plugin reports (onProgram, scene). Any thread. */
bool failover_source_on_program(const char *source, char *scene, size_t scene_size);

#ifdef __cplusplus
}
#endif
