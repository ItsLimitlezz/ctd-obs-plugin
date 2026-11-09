/* Local show-profile store (ticket T7, design section 2). Pure C, no libobs: everything lives under one root folder
 * (the plugin's config folder in OBS, a temp folder in ctd-show-tests).
 *
 *   <root>/show-profiles/<id>/meta.json            {id, name, relayProfileId, baseRevision, headSeq, appliedSeq,
 *                                                   dirty, savedAt, lastAppliedAt, lastSavedHash}
 *   <root>/show-profiles/<id>/revisions/<seq>/     one bundle (bundle.h layout); seq counts local saves and pulls
 *   <root>/show-profiles/<id>/local.json           this machine's secrets + stream service (bundle.h "local")
 *   <root>/show-profiles/<id>/device-map.<machine>.json   M4: this machine's slot -> device map
 *   <root>/assets/<ab>/<sha256>.<ext>              content-addressed asset cache shared by every show profile
 *
 * Local revisions are numbered by a local sequence (seq), not Relay's revision: a local save works offline and is
 * renumbered by nobody. manifest.revision is Relay's number once the revision is on Relay (M3:
 * show_store_set_relay_revision), and manifest.parentRevision the Relay revision it was based on.
 *
 * Writes are atomic: a revision is written to revisions/.tmp-<random>/ and renamed into place; meta.json and
 * local.json go through a temp file + rename. A crash leaves at worst a .tmp-* folder, swept by show_store_open.
 * Every call is safe from any one thread at a time (the plugin serializes them on its worker). */
#ifndef CTD_SHOW_STORE_H
#define CTD_SHOW_STORE_H

#include "bundle.h"
#include "json.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHOW_KEEP_REVISIONS 5 /* design section 2: the last 5 bundles applied or pulled */

/* Opens (creates) the store under root and sweeps leftovers (.tmp folders and files, *.part downloads). */
bool show_store_open(const char *root);
void show_store_close(void);
const char *show_store_root(void);

/* Show profiles. show_store_list returns an array of meta objects sorted by name (caller frees). */
cj *show_store_list(void);
cj *show_store_meta(const char *id); /* NULL when there is no such profile */
bool show_store_write_meta(const char *id, const cj *meta);
bool show_store_create(const char *id, const char *name, char *err, size_t err_size);
bool show_store_rename(const char *id, const char *name, char *err, size_t err_size);
bool show_store_delete(const char *id);
/* Finds a profile by name (exact, case-insensitive ASCII). Writes its id into id_out (37 bytes). */
bool show_store_find_by_name(const char *name, char id_out[37]);

/* Revisions. */
enum show_save_origin {
	SHOW_SAVE_LOCAL, /* a save on this machine: dirty until it is on Relay */
	SHOW_SAVE_RELAY, /* M3: a revision pulled from Relay: not dirty, baseRevision = manifest.revision */
};
/* Validates the manifest, caches every asset in b->asset_sources (hash checked), writes the bundle as revision
 * headSeq + 1, writes b->local to local.json (merged over what is there: values from other revisions stay), updates
 * meta and prunes old revisions (keeping the newest SHOW_KEEP_REVISIONS plus the applied one). */
bool show_store_save(const char *id, struct show_bundle *b, enum show_save_origin origin, int64_t *seq_out, char *err,
		     size_t err_size);
/* Loads a revision (seq 0 = head) and checks every file against checksums.json and the manifest; local is loaded
 * from local.json. Fails on any mismatch: a corrupt bundle is never applied. */
bool show_store_load(const char *id, int64_t seq, struct show_bundle *b, char *err, size_t err_size);
/* Newest first. */
cj *show_store_revisions(const char *id);
/* M3: records Relay's revision number for a local revision once it is saved up (and clears dirty if it is the
 * head). */
bool show_store_set_relay_revision(const char *id, int64_t seq, int64_t revision);
/* The folder of a revision (for M3's tar of the bundle). */
bool show_store_revision_dir(const char *id, int64_t seq, char *out, size_t size);
/* sha256 over the bundle's checksums.json: equal for two saves with identical content (meta.lastSavedHash). */
bool show_store_bundle_hash(const char *id, int64_t seq, char out[65]);

/* Assets. The cache path of an asset: <root>/assets/<ab>/<sha256><ext>, ext taken from name (".cube", ".png"...;
 * none when name has no short alphanumeric extension). */
bool show_store_asset_path(const char *sha256, const char *name, char *out, size_t size);
/* Copies src into the cache unless it is there already; the copy is hashed and must equal sha256. */
bool show_store_add_asset(const char *src, const char *sha256, const char *name, char *err, size_t err_size);
/* Present in the cache with this size (-1: any size). */
bool show_store_has_asset(const char *sha256, const char *name, int64_t size);

/* M4: this machine's device map for a profile (an object, NULL when none) and saving it. machine is a file-safe name. */
cj *show_store_device_map(const char *id, const char *machine);
bool show_store_write_device_map(const char *id, const char *machine, const cj *map);

#ifdef __cplusplus
}
#endif

#endif
