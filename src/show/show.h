/* Show Profiles in the plugin (design sections 2, 6): the one API the dock (T12), the sync client (M3, T11) and the
 * fix-mapping dialog (M4, T13) call.
 *
 * Show Profiles turns on only inside CTD OBS: show_available() is true once the core's C1 procs answer on the global
 * proc handler. On stock OBS everything here is inert and the dock hides the section (design section 7).
 *
 * Threading: show_* calls that start work (save, update, switch, delete) are made on OBS's UI thread and return at
 * once; the work runs on one worker thread, one operation at a time, and the outcome shows up in show_state_get()
 * (poll it from the dock's timer: seq goes up on every change). The swap and verify steps run back on the UI thread. */
#ifndef CTD_SHOW_SHOW_H
#define CTD_SHOW_SHOW_H

#include "json.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Core C1 procs (CTD OBS, design section 7), called through proc_handler_call(obs_get_proc_handler(), ...):
 *   void ctd_show_apply(in string collection_path, in string profile_path, out bool success, out string error)
 *       loads the OBS profile in profile_path (a folder) then the scene collection in collection_path (a .json file),
 *       without first saving the active collection over anything.
 *   void ctd_show_refresh()
 *       re-reads the scene collection and profile lists (menus) from disk. */
#define SHOW_PROC_APPLY "ctd_show_apply"
#define SHOW_PROC_REFRESH "ctd_show_refresh"

/* OBS-side names of a show profile's own collection and profile: "CTD · <name>" (design section 2). The files are
 * basic/scenes/ctd-show-<id>.json and basic/profiles/ctd-show-<id>/, so a rename never moves files. */
#define SHOW_OBS_NAME_PREFIX "CTD \xC2\xB7 "
#define SHOW_OBS_FILE_PREFIX "ctd-show-"
/* The collection's modules entry that ties it to its show profile: {"id": "...", "seq": n, "revision": n} */
#define SHOW_MODULE_KEY "ctd-show-profile"

/* Lifetime (plugin-main.c): init on load, the proc check after OBS finished loading, shutdown on unload. */
void show_init(void);
void show_shutdown(void);

enum show_busy {
	SHOW_IDLE,
	SHOW_SAVING,    /* save as / update: exporting the current setup */
	SHOW_SWITCHING, /* stage -> swap -> verify (-> rollback) */
	SHOW_DELETING,
	SHOW_UPLOADING, /* M3: saving up to CTD Relay (sync.h) */
	SHOW_PULLING,   /* M3: downloading from CTD Relay, or answering a conflict */
};

struct show_entry {
	char id[37];
	char *name;
	int64_t head_seq;       /* local revisions saved (0 = none yet) */
	int64_t revision;       /* Relay revision of the head, 0 = never on Relay */
	bool dirty;             /* "Not backed up": saved here, not on Relay yet */
	bool active;            /* its collection is the one loaded in OBS now */
	char *saved_at;         /* ISO 8601, "" when never */
	char *last_applied_at;  /* ISO 8601, "" when never switched to on this machine */
	char *relay_profile_id; /* "" when local only */
	int needs_reentry;      /* secrets from another machine that have no value here (last switch) */
};

struct show_state {
	bool available;      /* CTD OBS core procs present */
	enum show_busy busy; /* what the worker is doing */
	char *message;       /* the last outcome or the current step, one line for the dock ("" when none) */
	bool message_is_error;
	char *active_id;            /* the show profile whose collection is loaded, "" when none */
	bool live;                  /* an output is active: switching is blocked (design section 6) */
	char *live_reason;          /* why, e.g. "Stop streaming first." ("" when not live) */
	struct show_entry *entries; /* sorted by name */
	size_t count;
	cj *warnings; /* strings from the last operation (missing asset files, encoder fallback, ...) */
	uint64_t seq; /* goes up whenever anything above changes */
};

/* A copy of the current state; free it with show_state_free. Any thread. */
void show_state_get(struct show_state *out);
void show_state_free(struct show_state *state);

/* The output guard (design section 6): false when streaming, recording, the replay buffer, the virtual camera or any
 * other output (vertical canvas, multitrack) is active, with the reason in reason. Any thread. */
bool show_guard_idle(char *reason, size_t size);

/* "Save current setup as": a new show profile named name from OBS's current collection + profile. The current
 * collection and profile become the show profile's own ("CTD · name") only on the next switch to it. */
bool show_save_current_as(const char *name);
/* "Update": saves the current setup as a new revision of the active show profile (id must be the active one). */
bool show_update(const char *id);
/* "Switch": refused while live; seq 0 = the newest local revision. */
bool show_switch(const char *id, int64_t seq);
/* Deletes a show profile's local copy (not the active one). OBS's collection and profile files stay. */
bool show_delete(const char *id);

/* ---- hooks for M3 (sync, T11) and M4 (device mapping, T13) ---- */

struct show_stage_ctx {
	const char *id;      /* show profile */
	int64_t seq;         /* local revision being applied */
	const cj *manifest;  /* the revision's manifest (validated) */
	cj *collection;      /* the collection about to be written: assets and local secrets already filled in */
	cj *service;         /* the service.json about to be written (object; may be edited, e.g. the ingest key) */
	const char *machine; /* this machine's file-safe name (device maps) */
	cj *warnings;        /* append strings for the dock */
};
/* Runs on the worker thread while staging, before anything is written into OBS's folders, in the order added.
 * Nothing in OBS has changed yet, so a hook may block (network) and may fail: returning false aborts the switch with
 * err, and OBS stays exactly as it was. Must not touch OBS sources. */
typedef bool (*show_stage_hook)(void *opaque, struct show_stage_ctx *ctx, char *err, size_t err_size);
void show_add_stage_hook(show_stage_hook hook, void *opaque);

/* Runs on the UI thread after a switch is verified (the new collection is loaded), e.g. to open the fix-mapping
 * dialog when a device slot is below the auto-map score. */
typedef void (*show_applied_hook)(void *opaque, const char *id, int64_t seq, const cj *manifest);
void show_add_applied_hook(show_applied_hook hook, void *opaque);

/* This machine's file-safe name, used for device-map.<machine>.json (out: at least 64 bytes). */
void show_machine_name(char *out, size_t size);

#ifdef __cplusplus
}
#endif

#endif
