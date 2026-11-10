/* Device identity extractor for Show Profiles (design section 5, ticket T6).
 *
 * Pure logic: C only, no libobs. A source's settings are read through a small string getter so the same code runs in
 * the plugin (wrap obs_data_get_string) and in ctd-show-tests (fixtures). Scoring and UI are out of scope. */
#ifndef CTD_SHOW_DEVICES_IDENTITY_H
#define CTD_SHOW_DEVICES_IDENTITY_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the string setting for `key`, or NULL when it is missing. The pointer only has to stay valid until the
 * next call. Plugin adapter: return obs_data_get_string(settings, key) when obs_data_has_user_value/default. */
typedef const char *(*devices_identity_get_fn)(void *ctx, const char *key);

typedef struct devices_identity {
	char cls[12];     /* "video", "audio", "display" or "window" */
	char id[512];     /* device id string exactly as stored in the settings; "" when none */
	char name[256];   /* display name; "" when unknown */
	char vid_pid[10]; /* "0fd9:0066" (lowercase) or "" */
	char serial[128]; /* USB serial / instance id (lowercase) or "" */
} devices_identity_t;

/* Fills `out` for a source of type `source_type` (the OBS source id, e.g. "dshow_input"). Returns false, with `out`
 * zeroed, for a type that is not in the design section 5 table. A known type with empty settings returns true with
 * empty fields. */
bool devices_identity(const char *source_type, devices_identity_get_fn get, void *ctx, devices_identity_t *out);

/* The settings keys of a source type's device: id_key (the device id, e.g. "video_device_id") and name_key (the display
 * name, or NULL when the type stores none). Returns false for a type that is not in the table. Used by the M4 mapper to
 * write a chosen device into a source's settings and to read a candidate's identity. */
bool devices_identity_keys(const char *source_type, const char **id_key, const char **name_key);

/* Class ("video", "audio", "display", "window") of a source type, or NULL when it is not in the table. */
const char *devices_identity_class(const char *source_type);

/* dshow `video_device_id` ("<name>:<device path>", where ':' and '#' in the name are escaped as #3A and #22) ->
 * name and USB ids from a path like \\?\usb#vid_0fd9&pid_0066&mi_00#6&1a2b&0&0000#{guid}. Missing parts stay "".
 * Returns true when a VID and PID were found. */
bool devices_identity_parse_dshow(const char *device_id, devices_identity_t *out);

/* macOS AVFoundation uniqueID for USB devices: "0x" + location id + 4 hex VID + 4 hex PID. Returns true on match. */
bool devices_identity_parse_avcapture(const char *unique_id, devices_identity_t *out);

#ifdef __cplusplus
}
#endif

#endif
