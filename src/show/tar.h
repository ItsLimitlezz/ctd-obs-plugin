/* The bundle as Relay stores it (API v1.9 section 14: "one tar of small JSON files"). Pure C, no libobs.
 *
 * The tar holds collection.json and the profile files (profile/basic.ini, profile/streamEncoder.json and recordEncoder.json, profile/service.json):
 * exactly the bytes the local store keeps for the revision. The manifest travels beside it (the revision's own field)
 * and checksums.json is the store's business, so neither is in the tar.
 *
 * Written deterministically (ustar, entries sorted by path, mode 0644, uid/gid 0, mtime 0, no owner names) so the
 * same revision always gives the same bytes and the same bundleSha256, on every machine. */
#ifndef CTD_SHOW_TAR_H
#define CTD_SHOW_TAR_H

#include "bundle.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHOW_BUNDLE_MAX_BYTES (16u * 1024u * 1024u) /* Relay's limit for a revision's bundle */

/* Writes the bundle's collection (printed as the store prints it) and files. *out is malloc'd (free()). */
bool show_tar_write(const struct show_bundle *b, char **out, size_t *size);

/* Reads a tar into b->collection and b->files (b must be initialized; manifest and local are left alone). Refuses
 * unsafe paths, links, devices, duplicate entries, entries past SHOW_BUNDLE_MAX_BYTES and a tar without
 * collection.json. Directory entries are skipped. */
bool show_tar_read(const char *data, size_t size, struct show_bundle *b, char *err, size_t err_size);

#ifdef __cplusplus
}
#endif

#endif
