/* File helpers for Show Profiles: UTF-8 paths on every platform (wide APIs on Windows), atomic writes.
 *
 * Pure C, no libobs, so the store runs in ctd-show-tests too. '/' works as a separator everywhere. */
#ifndef CTD_SHOW_FS_H
#define CTD_SHOW_FS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

FILE *sfs_fopen(const char *path, const char *mode);
bool sfs_exists(const char *path);
bool sfs_is_dir(const char *path);
int64_t sfs_size(const char *path); /* -1 when missing or a directory */
/* Creates path and every missing parent. */
bool sfs_mkdirs(const char *path);
/* Reads a whole file; *data is malloc'd and NUL-terminated (not counted in *size). */
bool sfs_read(const char *path, char **data, size_t *size);
/* Writes path.<random>.tmp in the same folder, flushes it to disk, then renames it over path. */
bool sfs_write_atomic(const char *path, const void *data, size_t size);
/* Renames a file or a directory; a file replaces an existing file. A directory target must not exist. */
bool sfs_rename(const char *from, const char *to);
bool sfs_remove(const char *path);                    /* a file; true if it is gone afterwards */
bool sfs_remove_tree(const char *path);               /* a directory and everything in it; true if gone afterwards */
bool sfs_copy_file(const char *from, const char *to); /* atomic, like sfs_write_atomic */
bool sfs_copy_tree(const char *from, const char *to); /* to must not exist */

/* Calls cb for each entry of dir (not "." or ".."); stop by returning false. Returns false if dir can't be read. */
typedef bool (*sfs_list_cb)(void *ctx, const char *name, bool is_dir);
bool sfs_list(const char *dir, sfs_list_cb cb, void *ctx);

/* dst = a + "/" + b (no doubled separator). Returns false if it doesn't fit. */
bool sfs_join(char *dst, size_t size, const char *a, const char *b);

/* 16 random bytes as lowercase hex (32 chars + NUL), for temp names and ids. */
void sfs_random_hex(char out[33]);

#ifdef __cplusplus
}
#endif

#endif
