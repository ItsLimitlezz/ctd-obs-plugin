/* OBS profile basic.ini for Show Profiles (ticket T9, design section 1.1): what a bundle carries and how it lands on
 * a machine. Pure C, no libobs.
 *
 * A bundle carries an allow-listed set of sections/keys: video (base/output resolution, FPS, scale type, color),
 * audio (sample rate, channels), output mode + simple/advanced encoder and bitrate settings, replay buffer length.
 * Never carried: record/replay paths, file name formats, hotkeys, monitor/device ids, anything secret-named. */
#ifndef CTD_SHOW_INI_H
#define CTD_SHOW_INI_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Is section/key carried in a bundle? */
bool show_ini_key_allowed(const char *section, const char *key);

/* The allow-listed subset of an INI text (sections in their original order, keys too). Returns malloc'd text
 * (free()); NULL only when out of memory. */
char *show_ini_filter(const char *ini);

/* Apply: the bundle's allow-listed keys over the machine's existing profile (local_ini may be NULL or ""): local
 * keys the bundle doesn't carry stay as they are (record path, hotkeys...), carried keys take the bundle's value,
 * and [General] Name is set to profile_name. Returns malloc'd text (free()). */
char *show_ini_merge(const char *bundle_ini, const char *local_ini, const char *profile_name);

/* Reads one value (section/key) from an INI text into out; false when absent. */
bool show_ini_get(const char *ini, const char *section, const char *key, char *out, unsigned size);

#ifdef __cplusplus
}
#endif

#endif
