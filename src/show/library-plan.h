/* Pure helpers for the Relay library mirror (Show Profiles ticket T14). C only, no libobs, so ctd-library-tests can
 * run them anywhere. library-mirror.c does the talking to Relay and the disk. */
#ifndef CTD_SHOW_LIBRARY_PLAN_H
#define CTD_SHOW_LIBRARY_PLAN_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LIBRARY_SHA256_LEN 64

/* 64 hex characters (either case). */
bool library_valid_sha256(const char *sha256);

/* The file extension (with the dot, lowercase) a library entry is stored under: ".png" for a name that ends in .png,
 * else ".cube". Returns NULL for an entry the mirror doesn't keep yet (a kind other than "lut"). `kind` may be NULL or
 * empty (treated as "lut"). */
const char *library_extension(const char *kind, const char *name);

/* "<sha256 lowercase><ext>" into out. False when sha256 isn't valid or ext is NULL. */
bool library_cache_name(const char *sha256, const char *ext, char *out, size_t size);

/* Whether a file found in the cache folder should be deleted: true for one of our hash-named files ("<64 hex>.<ext>")
 * that is not in `wanted` (the cache names that should stay), and for a leftover download ("*.part", "*.dl").
 * Everything else (index.json, dot files, anything the user put there) is kept: false. */
bool library_is_stale(const char *name, const char *const *wanted, size_t wanted_count);

/* A readable name for the index: the entry's name without a path or control characters, at most 100 bytes, never
 * empty ("untitled"). */
void library_display_name(const char *name, char *out, size_t size);

#ifdef __cplusplus
}
#endif

#endif
