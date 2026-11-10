/* An in-memory show-profile bundle (design section 1): what export produces, what the store saves and loads, and what
 * apply stages. Pure C, no libobs.
 *
 * On disk (store.c) a bundle is a folder:
 *   manifest.json         design section 1.2, schema 1
 *   collection.json       the scene collection, scrubbed (secrets, keys) and with asset paths as ${ctd-asset:<sha256>}
 *   profile/basic.ini     the allow-listed OBS profile settings
 *   profile/streamEncoder.json, profile/recordEncoder.json   when the profile has them
 *   profile/service.json  the stream service reduced to its type (+ server for custom): never a key
 *   checksums.json        {"<relative path>": "<sha256>"} of every file above, checked before any apply
 * The bundle's machine-local half (local) never goes into that folder, never to Relay. */
#ifndef CTD_SHOW_BUNDLE_H
#define CTD_SHOW_BUNDLE_H

#include "json.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHOW_FILE_MANIFEST "manifest.json"
#define SHOW_FILE_COLLECTION "collection.json"
#define SHOW_FILE_CHECKSUMS "checksums.json"
#define SHOW_FILE_BASIC_INI "profile/basic.ini"
#define SHOW_FILE_STREAM_ENCODER "profile/streamEncoder.json"
#define SHOW_FILE_RECORD_ENCODER "profile/recordEncoder.json"
#define SHOW_FILE_SERVICE "profile/service.json"

/* Placeholders inside collection.json (design section 1.2) */
#define SHOW_ASSET_PREFIX "${ctd-asset:" /* + 64 hex + "}" */
#define SHOW_SECRET_PLACEHOLDER "${ctd-secret}"

struct show_file {
	char *path; /* relative, '/'-separated, validated with show_path_is_safe */
	char *data;
	size_t size;
};

struct show_bundle {
	cj *manifest;
	cj *collection;
	/* every other bundle file (profile/...), in a stable order */
	struct show_file *files;
	size_t file_count;
	/* This machine only, never uploaded:
	 *   {"secrets": {"<source uuid or ''>|<JSON pointer>": "<original value>", ...},
	 *    "service": {<the original service.json settings, stream key included>}}
	 * Kept beside the profile (store: local.json), so switching back on this machine restores them. */
	cj *local;
	/* export only: {"<sha256>": "<absolute path of the file on this machine>"} so the store can cache it */
	cj *asset_sources;
};

void show_bundle_init(struct show_bundle *b);
void show_bundle_free(struct show_bundle *b);
/* Adds or replaces a file (data is copied). Returns false on an unsafe path or no memory. */
bool show_bundle_set_file(struct show_bundle *b, const char *path, const void *data, size_t size);
const struct show_file *show_bundle_file(const struct show_bundle *b, const char *path);

#ifdef __cplusplus
}
#endif

#endif
