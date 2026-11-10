/* Device mapping for Show Profiles (design section 5, tickets T11 scoring + T13 dialog). Pure C, no libobs.
 *
 * A bundle names its devices by slot (the source's uuid) with the identity captured on the machine that saved it
 * (manifest.devices[], from T6). On this machine each slot is filled from this machine's devices of the same source
 * type ("candidates"), in this order:
 *
 *   1. remembered: this machine's device-map.<machine>.json for the profile names a device for the slot and that device
 *      is present. Always wins (Juan's decision 3: exact on the same machine).
 *   2. scored, highest first, each device used by one slot (unless the bundle itself shared one device between slots):
 *        same device id 100 · alias (a device fixed for the same identity in another profile on this machine) 98 ·
 *        same VID:PID + serial 95 · same VID:PID and only one such device here 80 · same display name 70 ·
 *        normalized name 60 · same brand family 30
 *      >= 70 auto, 30..69 suggested (needs a confirm), < 30 missing: the source keeps its settings, never deleted.
 *
 * Window and game capture are portable and never mapped.
 *
 * JSON shapes (all cj):
 *   candidates  {"<sourceType>": [{"id": "...", "name": "..."}, ...], ...}   this machine's devices
 *   remembered  {"<slot>": {"id": "...", "name": "..."}, ...}                device-map.<machine>.json (NULL: none)
 *   aliases     {"<identity key>": {"id": "...", "name": "..."}, ...}        device-aliases.<machine>.json (NULL: none)
 *   mapping     [{"slot", "sourceType", "class", "label", "was", "status", "score", "id", "name",
 *                 "rememberedMissing"}, ...]
 *               status is "remembered", "auto", "suggested" or "missing"; id/name are "" when missing. */
#ifndef CTD_SHOW_DEVICES_H
#define CTD_SHOW_DEVICES_H

#include "json.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHOW_DEVICE_SCORE_AUTO 70
#define SHOW_DEVICE_SCORE_SUGGEST 30
#define SHOW_DEVICE_SCORE_ALIAS 98

#define SHOW_DEVICE_REMEMBERED "remembered"
#define SHOW_DEVICE_AUTO "auto"
#define SHOW_DEVICE_SUGGESTED "suggested"
#define SHOW_DEVICE_MISSING "missing"

/* Lowercase, "(2)" / "#2" / trailing " video" removed, runs of non-alphanumerics folded to one space. */
void show_device_normalize_name(const char *name, char *out, size_t size);
/* "elgato", "magewell", "blackmagic", "avermedia", "logitech", ... or NULL when the name names no known brand. */
const char *show_device_brand(const char *name);

/* The score of one of this machine's devices for a captured identity ({name, id, vidPid, serial}). same_vidpid is how
 * many of the candidates share the device's VID:PID (the 80 rule needs exactly one). */
int show_device_score(const char *source_type, const cj *identity, const char *dev_id, const char *dev_name,
		      int same_vidpid);

/* A stable key for a captured identity, for the alias table: "usb:<vid:pid>:<serial>", "id:<id>" or
 * "name:<normalized name>" (written into out; "" when the identity has nothing to key on). */
void show_device_identity_key(const char *source_type, const cj *identity, char *out, size_t size);

/* The mapping of a manifest's devices[] on this machine. Returns a new array (caller frees). */
cj *show_devices_map(const cj *devices, const cj *candidates, const cj *remembered, const cj *aliases);

/* True when the "Fix mapping" dialog should open: a slot is suggested or missing, or remembered but not present. */
bool show_devices_needs_fix(const cj *mapping);

/* Writes each mapped device (any status with an id) into the collection: settings[<id key>] = id and, when the type
 * has one, settings[<name key>] = name. Missing slots are left exactly as they are. Returns how many sources changed. */
int show_devices_apply(cj *collection, const cj *mapping);

/* The remembered map after a switch: every remembered or auto slot is written into map (an object), so the next switch
 * to this profile on this machine fills them exactly the same. Suggested and missing slots are not (they wait for a
 * confirm in the dialog). Returns true if map changed. */
bool show_devices_remember_auto(cj *map, const cj *mapping);

/* A choice made in the dialog: map[slot] = {id, name, confirmed: true}. An empty id forgets the slot. */
bool show_devices_remember(cj *map, const char *slot, const char *id, const char *name);

/* The alias table entry for a confirmed choice: aliases[<identity key>] = {id, name}. False if the identity has no key. */
bool show_devices_alias(cj *aliases, const char *source_type, const cj *identity, const char *id, const char *name);

#ifdef __cplusplus
}
#endif

#endif
