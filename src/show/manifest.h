/* Show-profile manifest, schema 1 (design section 1.2): validation and helpers. Pure C, no libobs.
 *
 * A reader refuses a schema it doesn't know and never half-applies one (design 1.3), so every bundle is validated
 * before the store saves it and again before apply stages it. */
#ifndef CTD_SHOW_MANIFEST_H
#define CTD_SHOW_MANIFEST_H

#include "json.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHOW_MANIFEST_SCHEMA 1
#define SHOW_NAME_MAX 64 /* characters of a show profile's name, in bytes of UTF-8 */

/* Asset kinds (design 1.1). */
extern const char *const show_asset_kinds[]; /* NULL-terminated: "lut", "image", "video", "audio", "font", "html", "other" */

/* A new manifest: schema, showProfileId, name, revision 0, parentRevision 0, relayProfileId null, empty files/assets/
 * devices/overlays/secrets. */
cj *show_manifest_new(const char *id, const char *name);

/* Checks everything a reader relies on: schema == 1, id (uuid text), name (1..SHOW_NAME_MAX, no control characters),
 * integer revisions (>= 0, parent <= revision), files (each path safe), assets (sha256 hex, size >= 0, a known kind,
 * refs), devices (slot + sourceType), overlays (source + slot + canvas), secrets (key). Unknown extra members are
 * allowed (forward-compatible within a schema). On failure, err says what and where. */
bool show_manifest_validate(const cj *manifest, char *err, size_t err_size);

/* A bundle-relative path: not empty, not absolute ("/x", "\x", "C:"), no "." or ".." segment, no backslash, no
 * control characters, no empty segment, at most 255 bytes. (design section 8, "Paths") */
bool show_path_is_safe(const char *path);

/* 64 lowercase hex digits. */
bool show_is_sha256(const char *s);
/* 8-4-4-4-12 hex (either case). */
bool show_is_uuid(const char *s);
/* A name that may be shown and used in "CTD · <name>": trimmed, 1..SHOW_NAME_MAX bytes, valid UTF-8, no control
 * characters and none of / \ : * ? " < > | (it also ends up in OBS's menus). */
bool show_name_is_valid(const char *name);

/* "${ctd-asset:<sha256>}" -> the sha256 (into out), else false. */
bool show_asset_placeholder_parse(const char *value, char out[65]);
/* Writes "${ctd-asset:<sha256>}" into out (at least 78 bytes). */
void show_asset_placeholder(const char *sha256, char *out, size_t size);

#ifdef __cplusplus
}
#endif

#endif
