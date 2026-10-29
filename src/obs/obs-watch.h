/*
 * Watches OBS's outputs and program scenes and sends their changes to CTD Relay as obs.output and obs.scene
 * events (API v1.4). Start it with the module and stop it before the CTD Relay client shuts down.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void obs_watch_start(void);
void obs_watch_stop(void);

/* The program scene of every canvas, for GetProgramScenes: {"scenes": [{"canvas", "scene", "sceneUuid"}]}. The caller
 * releases it. */
struct obs_data;
struct obs_data *obs_watch_program_scenes(void);

#ifdef __cplusplus
}
#endif
