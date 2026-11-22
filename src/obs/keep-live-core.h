/*
 * Keep scenes live: the bookkeeping behind the "ctd" vendor requests KeepScenesLive and ReleaseScenesLive
 * (src/obs/keep-live.h). Pure C, no libobs, so it is unit-tested on its own (test/keep-live).
 *
 * OBS only runs a scene's sources while something shows it (Program, studio-mode Preview, the Multiview, a
 * projector). CTD Live Studio's multiview reads the other scenes as screenshots, which show a scene for one frame only,
 * so browser, capture and "deactivate when not showing" sources hold their last frame. The plugin keeps those scenes
 * showing for as long as a client keeps renewing its lease.
 *
 * Two halves, so the show references can never be dropped twice:
 *
 *   kl_leases   what clients asked for: per client id, a set of scene keys (scene UUIDs) and when the lease ends.
 *               Renewing replaces the client's set. Any thread, under the caller's lock.
 *   kl_ledger   what is actually held: one show reference per key, however many clients want it. kl_ledger_sync
 *               moves it to the union of the leases; each acquire that succeeds is dropped exactly once, by a later
 *               sync or kl_ledger_clear. Owned by one thread (OBS's UI thread).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KL_KEY_MAX 48     /* a scene key (OBS scene UUID, 36 characters) plus room; longer keys are refused */
#define KL_CLIENT_MAX 64  /* a client id, with its terminating NUL */
#define KL_MAX_SCENES 16  /* scenes one client may keep live */
#define KL_MAX_CLIENTS 32 /* clients holding leases at once */
#define KL_MAX_KEYS (KL_MAX_SCENES * KL_MAX_CLIENTS)
#define KL_MAX_LIVE 12   /* scenes kept live at once, all clients together (the rest are not shown) */
#define KL_LEASE_MIN_S 3 /* leaseSeconds is clamped to [3, 30], default 10 */
#define KL_LEASE_MAX_S 30
#define KL_LEASE_DEFAULT_S 10

typedef char kl_key_t[KL_KEY_MAX];

struct kl_lease {
	char client[KL_CLIENT_MAX];
	uint64_t expires_ms;
	size_t count;
	kl_key_t keys[KL_MAX_SCENES];
};

struct kl_leases {
	size_t count;
	struct kl_lease items[KL_MAX_CLIENTS];
};

enum kl_renew_result {
	KL_RENEWED,          /* the client holds the (possibly new) set until expires_ms */
	KL_RELEASED,         /* an empty set: the client holds nothing now */
	KL_BAD_CLIENT,       /* no client id, or one too long */
	KL_TOO_MANY_CLIENTS, /* KL_MAX_CLIENTS other clients hold leases */
};

/* leaseSeconds as asked (<= 0 or not a number: the default), clamped to [KL_LEASE_MIN_S, KL_LEASE_MAX_S] */
double kl_clamp_lease(double seconds);

void kl_leases_init(struct kl_leases *l);
/* Replaces the client's set with keys (duplicates, empty and over-long keys are skipped; at most KL_MAX_SCENES are
 * kept, the rest are counted in *capped). *changed says whether the client's set differs from before. Either out
 * pointer may be NULL. */
enum kl_renew_result kl_renew(struct kl_leases *l, const char *client, const char *const *keys, size_t count,
			      uint64_t expires_ms, bool *changed, size_t *capped);
/* true if the client held anything */
bool kl_release(struct kl_leases *l, const char *client);
/* Drops every lease that ended at or before now_ms; returns how many. */
size_t kl_expire(struct kl_leases *l, uint64_t now_ms);
/* A scene went away or was renamed: drops the key from every lease (a lease left empty is dropped). Returns how many
 * leases had it. */
size_t kl_forget_key(struct kl_leases *l, const char *key);
void kl_leases_clear(struct kl_leases *l);
const struct kl_lease *kl_find(const struct kl_leases *l, const char *client);
/* The union of every lease's keys, each once, into out (at most max). Returns how many. */
size_t kl_union(const struct kl_leases *l, kl_key_t *out, size_t max);

/* Takes a show reference on the scene with this key; returns a handle, or NULL if there is no such scene. */
typedef void *(*kl_acquire_fn)(void *ctx, const char *key);
/* Gives back the reference taken by the acquire that returned handle. Called exactly once per handle. */
typedef void (*kl_drop_fn)(void *ctx, const char *key, void *handle);

struct kl_ledger {
	size_t count;
	kl_key_t keys[KL_MAX_KEYS];
	void *handles[KL_MAX_KEYS];
};

void kl_ledger_init(struct kl_ledger *g);
/* Drops the held keys that aren't in want, then acquires the wanted keys not held yet. A failed acquire (no such
 * scene) is simply not held, and is tried again by the next sync. *acquired and *dropped (may be NULL) count the
 * changes. */
void kl_ledger_sync(struct kl_ledger *g, const kl_key_t *want, size_t want_count, kl_acquire_fn acquire,
		    kl_drop_fn drop, void *ctx, size_t *acquired, size_t *dropped);
/* Drops everything held; returns how many. */
size_t kl_ledger_clear(struct kl_ledger *g, kl_drop_fn drop, void *ctx);
bool kl_ledger_holds(const struct kl_ledger *g, const char *key);

#ifdef __cplusplus
}
#endif
