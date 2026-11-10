/* Show Profiles sync with CTD Relay (design section 4, ticket T11): the plugin side of sync-flow.h.
 *
 * - "Save to Relay" (show_sync_save_up): never automatic. For the active profile the current setup is saved locally
 *   first (Update), then the newest local revision goes up: POST /blobs/check, PUT the missing blobs (S2 libcurl
 *   transfers), POST the revision with its parent. Offline, it is queued and retried on reconnect (local-first: the
 *   profile stays "Not backed up" and keeps working).
 * - 409 revision_conflict: the dock asks Keep mine / Take theirs / Save as new profile (show_sync_resolve).
 * - Pull: a Relay profile's show profile that isn't on this machine (show_sync_pull), and automatically, when Relay's
 *   head is newer than a local copy that has nothing unsaved: the bundle + blobs are downloaded into the store and the
 *   profile shows "Update ready". Nothing is applied to OBS without the user (switch, or "Update now"), and never while
 *   live (M2 guard).
 * - Progress (step, bytes) and the per-profile sync state for the dock.
 *
 * Every show_sync_* call is made on OBS's UI thread. The work runs on the M2 worker, one action at a time. */
#ifndef CTD_SHOW_SYNC_H
#define CTD_SHOW_SYNC_H

#include "sync-core.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void show_sync_init(void);
void show_sync_shutdown(void);

struct show_sync_entry {
	char id[37];
	enum show_sync_status status;
	int64_t remote_head; /* -1 unknown */
	bool queued;         /* a save-up waiting for CTD Relay */
	bool update_ready;   /* a newer revision was downloaded: switch to it ("Update now") */
};

/* A Relay profile that has a show profile (GET /profiles items with show != null) */
struct show_sync_remote {
	char *relay_profile_id;
	char *name;        /* the Relay profile's name */
	int64_t head;      /* its show profile's head revision */
	char *saved_by;    /* the machine that saved the head ("mac-studio"), "" unknown */
	char local_id[37]; /* the local show profile linked to it, "" when not on this machine */
};

struct show_sync_state {
	bool relay_connected;   /* paired and reachable */
	char *relay_profile_id; /* the profile chosen in the dock ("" none): where an unlinked profile is saved up to */
	char *relay_profile_name; /* its name */
	char *step;               /* "Uploading", "Downloading", ... "" when idle */
	uint64_t done, total;     /* the step's progress (bytes or items); total 0 = unknown */
	struct show_sync_entry *entries;
	size_t count;
	struct show_sync_remote *remotes;
	size_t remote_count;
	/* a save-up that met a newer revision on Relay: ask the user */
	bool conflict;
	char conflict_id[37];
	char *conflict_name;
	int64_t conflict_head;
	char *conflict_by;     /* "mac-studio" or "" */
	uint64_t conflict_seq; /* goes up with each new conflict (the dock opens its dialog once per value) */
	uint64_t seq;          /* goes up whenever anything above changes */
};

void show_sync_state_get(struct show_sync_state *out);
void show_sync_state_free(struct show_sync_state *state);

/* "Save to Relay" for show profile id (linked: to its Relay profile; else to the profile chosen in the dock). */
bool show_sync_save_up(const char *id);
/* Download Relay profile relay_profile_id's head into this machine (a new local profile if needed); switch_after:
 * switch to it once it's here (refused while live, like any switch). */
bool show_sync_pull(const char *relay_profile_id, bool switch_after);
/* The answer to the pending conflict. */
bool show_sync_resolve(enum show_conflict_choice choice);
/* "Decide later": the profile stays "Not backed up" (and in conflict) until the next Save to Relay. */
void show_sync_dismiss_conflict(void);

#ifdef __cplusplus
}
#endif

#endif
