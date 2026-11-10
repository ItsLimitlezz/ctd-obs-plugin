/* Show Profiles sync, the pure part (design section 4, ticket T11). C only, no libobs, no network: what to send, how
 * to read what comes back, the conflict choices and the offline queue. sync.c does the HTTP and the threading.
 *
 * Relay API v1.9 (ctd-relay docs/API.md section 14):
 *   save up  POST /blobs/check {items:[{sha256,size}]} -> {missing:[...]}; PUT /blobs/{sha}?kind=...;
 *            POST /profiles/{id}/show/revisions {parentRevision, bundleSha256, manifest, secrets?, note?}
 *            -> 201 revision | 409 revision_conflict {head} | 400 missing_blobs {missing}
 *   pull     GET /profiles/{id}/show/revisions/{rev} (item + manifest), GET .../bundle (the tar), GET /blobs/{sha},
 *            GET .../secrets (show:secrets) -> {items:[{source,key,value}]} */
#ifndef CTD_SHOW_SYNC_CORE_H
#define CTD_SHOW_SYNC_CORE_H

#include "bundle.h"
#include "json.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Where a local show profile stands against Relay (the dock's chip). */
enum show_sync_status {
	SHOW_SYNC_LOCAL_ONLY,    /* never saved up: no Relay profile linked */
	SHOW_SYNC_UNKNOWN,       /* linked, but Relay's head isn't known (offline, not paired, no show:read) */
	SHOW_SYNC_UP_TO_DATE,    /* base == head, nothing new here */
	SHOW_SYNC_NOT_BACKED_UP, /* saved here since the base; Relay has nothing newer */
	SHOW_SYNC_UPDATE_READY,  /* Relay has a newer revision; nothing new here */
	SHOW_SYNC_CONFLICT,      /* both: saved here AND Relay moved on (design section 4: no automatic merge) */
};

/* remote_head < 0: unknown. */
enum show_sync_status show_sync_classify(bool linked, int64_t base, bool dirty, int64_t remote_head);
const char *show_sync_status_name(enum show_sync_status status);

/* The three answers to a conflict (Juan via the Head, 2026-11-10). Nothing is ever lost: every revision stays in Relay's
 * history and the local store keeps the last ones. */
enum show_conflict_choice {
	SHOW_CONFLICT_KEEP_MINE,   /* mine becomes Relay's next revision, on top of theirs (theirs stays in history) */
	SHOW_CONFLICT_TAKE_THEIRS, /* theirs is pulled into this profile; mine stays here as an older local revision */
	SHOW_CONFLICT_SAVE_AS_NEW, /* mine is copied into a new local-only show profile, then theirs is pulled here */
};
struct show_conflict_plan {
	bool fork_mine;   /* first: copy the local head into a new, unlinked show profile */
	bool pull_theirs; /* download Relay's head into this profile */
	bool upload_mine; /* POST the local head with parentRevision = parent */
	int64_t parent;
};
void show_conflict_plan(enum show_conflict_choice choice, int64_t head, struct show_conflict_plan *out);

/* POST /blobs/check body: the bundle first, then every manifest asset (sizes from the manifest). */
cj *show_sync_check_body(const cj *manifest, const char *bundle_sha256, int64_t bundle_size);
/* The Relay kind of a blob ("lut", "image", ..., "bundle"); "other" when unknown. */
const char *show_sync_blob_kind(const cj *manifest, const char *sha256, const char *bundle_sha256);

/* POST /profiles/{id}/show/revisions body. The manifest is copied with revision = parent + 1 (Relay numbers it; this is
 * only a hint), parentRevision = parent (null for the first save) and relayProfileId set. secrets[] holds the value of
 * every manifest.secrets[] entry found in local_secrets ("<source>|<key>"). Entries Relay can't hold (no source:
 * top-level values) stay on this machine; *kept_local counts them (may be NULL). */
cj *show_sync_revision_body(const cj *manifest, int64_t parent, const char *relay_profile_id, const char *bundle_sha256,
			    const cj *local_secrets, const char *note, int *kept_local);

/* GET .../secrets reply items -> local secrets ("<source>|<key>" -> value). Returns how many were taken. */
int show_sync_secrets_to_local(const cj *items, cj *local_secrets);

/* A manifest from Relay, made ready for show_store_save(SHOW_SAVE_RELAY) into local profile local_id: showProfileId,
 * relayProfileId, revision and parentRevision are set from Relay's revision item (Relay's numbers win over whatever the
 * saving machine wrote). */
bool show_sync_prepare_pulled(cj *manifest, const char *local_id, const char *relay_profile_id, int64_t revision,
			      int64_t parent);

/* "Save as new profile": turns a loaded bundle into one of a new, unlinked show profile. */
bool show_sync_fork(struct show_bundle *b, const char *new_id, const char *new_name);
/* "<name> (mine)", "<name> (mine 2)", ... the first free one (exists() says whether a name is taken), within
 * SHOW_NAME_MAX bytes. */
void show_sync_fork_name(const char *name, bool (*exists)(void *opaque, const char *name), void *opaque, char *out,
			 size_t size);

/* The offline queue (design section 4: local-first; a save-up that couldn't reach Relay is retried on reconnect).
 * Kept in <store root>/show-sync.json: {"queue": {"<id>": {"queuedAt": epoch s, "attempts": n, "lastError": "..."}}} */
bool show_sync_queue_add(const char *id, int64_t now_s, const char *error);
bool show_sync_queue_remove(const char *id);
bool show_sync_queue_has(const char *id);
/* The ids due for a retry at now_s, oldest first (caller frees). */
cj *show_sync_queue_due(int64_t now_s);
/* Seconds to wait after the n-th failed attempt: 15, 30, 60 ... at most 600. */
int64_t show_sync_retry_delay_s(int attempts);

#ifdef __cplusplus
}
#endif

#endif
