/* Shared between show.c (state, save, delete, lifetime) and apply.c (guard, staged apply, swap, verify, rollback).
 * Not for the dock: it uses show.h. */
#ifndef CTD_SHOW_INTERNAL_H
#define CTD_SHOW_INTERNAL_H

#include "show.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHOW_LOG_PREFIX "[ctd-show] "

/* state (show.c) */
void show_set_busy(enum show_busy busy);
void show_set_message(bool error, const char *fmt, ...);
void show_set_warnings(cj *warnings); /* takes ownership (NULL clears) */
void show_set_active(const char *id); /* "" or NULL: none */
void show_set_reentry(const char *id, int count);
void show_reload_entries(void);                         /* re-reads the store (any thread) */
bool show_run_worker(void *(*fn)(void *), void *param); /* one at a time; false if one is running */

/* hooks (show.c) */
bool show_run_stage_hooks(struct show_stage_ctx *ctx, char *err, size_t err_size);
void show_run_applied_hooks(const char *id, int64_t seq, const cj *manifest);

/* paths (show.c): OBS's basic/ folder (".../obs-studio/basic"), from the current profile's path */
bool show_basic_dir(char *out, size_t size);
/* the scene collection file whose "name" is name, under basic/scenes; false when none */
bool show_find_collection_file(const char *basic_dir, const char *name, char *out, size_t size);
void show_now_iso(char *out, size_t size);

/* apply.c */
bool show_core_available(void);                     /* the C1 procs answer */
void show_apply_start(const char *id, int64_t seq); /* UI thread; guard + worker staging */
void show_apply_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
