/*
 * One scene of a CTD Overlay overlay as its own OBS browser source (SKP 04 §4.9): CTD Overlay puts the scenes a URL
 * may pin on each overlay's CTD Relay resource (meta.scenes: [{ id, name, core? }]); a scene's URL is the overlay's
 * with scene=<id> set. Pure C: no libobs, so test/overlay-scenes can check it on every platform.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The overlay URL showing only `scene`: every scene= parameter it had is dropped, then scene=<id> (percent-encoded)
 * goes last in the query, before any #fragment. A NULL or empty scene gives the whole overlay (the URL without
 * scene=). Everything else, the key included, is kept byte for byte. A new string (free()), or NULL when out of
 * memory or url is NULL. */
char *ctd_overlay_scene_url(const char *url, const char *scene);

/* The scene= value of an overlay URL, decoded, or NULL when it has none (the whole overlay). A new string (free()). */
char *ctd_overlay_scene_of_url(const char *url);

/* Does the OBS scene `obs_scene` stand for the overlay scene (`id`, `name`)? Compared on letters and digits only,
 * ignoring case ("Starting Soon" = "starting-soon" = "STARTING_SOON"); non-ASCII bytes compare as they are. Never true
 * for a name with no letters or digits. */
bool ctd_overlay_scene_matches(const char *obs_scene, const char *id, const char *name);

#ifdef __cplusplus
}
#endif
