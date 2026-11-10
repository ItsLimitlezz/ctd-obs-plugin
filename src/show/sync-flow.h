/* Show Profiles sync flows (design section 4, ticket T11): save up, pull and the three conflict answers, written
 * against a table of Relay operations so the whole flow runs in ctd-show-unit against an in-memory Relay and in the
 * plugin against the real one (sync.c: HTTP JSON + S2 file transfers). Pure C, no libobs, blocking: run on a worker.
 *
 * Every call works on the local store (store.h), which must be open, from one thread at a time. */
#ifndef CTD_SHOW_SYNC_FLOW_H
#define CTD_SHOW_SYNC_FLOW_H

#include "json.h"
#include "sync-core.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum show_relay_result {
	SHOW_RELAY_OK,
	SHOW_RELAY_OFFLINE,   /* unreachable, timeout, 5xx, 429: try again later (the save-up is queued) */
	SHOW_RELAY_DENIED,    /* 401/403: not paired, or the token lacks a show scope ("re-pair") */
	SHOW_RELAY_NOT_FOUND, /* 404 */
	SHOW_RELAY_BAD,       /* 400: invalid, missing_blobs, hash_mismatch */
	SHOW_RELAY_REFUSED,   /* any other 4xx: 409 revision_conflict, 413 limit/quota (told apart by the flow) */
	SHOW_RELAY_LOCAL,     /* a local file couldn't be read or written */
	SHOW_RELAY_ABORTED,   /* stopped (shutdown) */
};
const char *show_relay_result_text(enum show_relay_result r);

/* Relay operations. Paths are UTF-8 local files; replies are cj (caller frees). profile is the Relay profile id. */
struct show_relay_ops {
	void *opaque;
	/* POST /blobs/check: *missing = the reply's "missing" array */
	enum show_relay_result (*check)(void *opaque, const cj *body, cj **missing);
	/* PUT /blobs/{sha}?kind=: the file at path */
	enum show_relay_result (*put_blob)(void *opaque, const char *sha256, const char *kind, const char *path);
	/* POST /profiles/{profile}/show/revisions: *reply = the new revision item */
	enum show_relay_result (*post_revision)(void *opaque, const char *profile, const cj *body, cj **reply);
	/* GET /profiles/{profile}/show -> headRevision (0 and OK when it has no show profile yet: 404 not_found) */
	enum show_relay_result (*head)(void *opaque, const char *profile, int64_t *head);
	/* GET /profiles/{profile}/show/revisions/{rev}: *item = the revision with its manifest */
	enum show_relay_result (*get_revision)(void *opaque, const char *profile, int64_t revision, cj **item);
	/* GET .../revisions/{rev}/bundle and GET /blobs/{sha} into path (atomically); sha256_out = what arrived */
	enum show_relay_result (*get_bundle)(void *opaque, const char *profile, int64_t revision, const char *path,
					     char sha256_out[65]);
	enum show_relay_result (*get_blob)(void *opaque, const char *sha256, const char *path, char sha256_out[65]);
	/* GET .../revisions/{rev}/secrets: *items = the reply's "items" (DENIED without show:secrets) */
	enum show_relay_result (*get_secrets)(void *opaque, const char *profile, int64_t revision, cj **items);
	/* progress for the dock (may be NULL): step is "Uploading", "Downloading", ...; bytes done/total of the step */
	void (*progress)(void *opaque, const char *step, uint64_t done, uint64_t total);
};

struct show_sync_outcome {
	enum show_relay_result result;
	bool conflict;      /* save up: Relay's head moved on (409) -> ask Keep mine / Take theirs / Save as new */
	bool no_changes;    /* save up: the head is already this revision on Relay; pull: already have it */
	int64_t revision;   /* save up: Relay's number for the saved revision; pull: the revision pulled */
	int64_t head;       /* Relay's head as last seen */
	int64_t seq;        /* pull: the local revision written */
	char id[37];        /* pull: the local show profile it went into */
	char forked_id[37]; /* save as new: the new local-only profile that holds "mine" */
	int uploaded;       /* blobs sent */
	int downloaded;     /* blobs fetched */
	int secrets_local;  /* save up: secrets that stay on this machine (Relay can't hold them) */
	int secrets_pulled; /* pull: secret values received (show:secrets) */
	char error[512];    /* one line for the dock when result isn't OK */
};

/* Save up the local head of show profile id (design section 4). relay_profile links an unlinked profile on its first
 * save (ignored when the profile is linked already). parent < 0: the profile's baseRevision. scratch is a folder
 * for the bundle's tar (created; the file is removed after). */
bool show_flow_save_up(const struct show_relay_ops *ops, const char *id, const char *relay_profile, int64_t parent,
		       const char *scratch, const char *note, struct show_sync_outcome *out);

/* Pull a Relay revision (0 = the head) of relay_profile into the local store, then the caller switches to it (M2
 * apply). local_id: the profile to pull into, or NULL to find it (the local profile linked to relay_profile, else the
 * manifest's showProfileId, else a new one). Never touches OBS. */
bool show_flow_pull(const struct show_relay_ops *ops, const char *relay_profile, int64_t revision, const char *local_id,
		    const char *scratch, struct show_sync_outcome *out);

/* The answer to a conflict on show profile id whose Relay head is head. relay_profile: where an unlinked profile's
 * first save met the conflict (ignored once the profile is linked). */
bool show_flow_resolve(const struct show_relay_ops *ops, const char *id, const char *relay_profile,
		       enum show_conflict_choice choice, int64_t head, const char *scratch,
		       struct show_sync_outcome *out);

/* The local profile linked to relay_profile (id_out 37 bytes). */
bool show_flow_find_linked(const char *relay_profile, char id_out[37]);

/* A random v4 uuid (37 bytes). */
void show_flow_new_uuid(char out[37]);

#ifdef __cplusplus
}
#endif

#endif
