/*
 * Keep scenes live: lease and show-reference bookkeeping. Pure C, no libobs. See keep-live-core.h.
 */

#include "keep-live-core.h"

#include <string.h>

double kl_clamp_lease(double seconds)
{
	if (!(seconds > 0)) /* also NaN */
		return KL_LEASE_DEFAULT_S;
	if (seconds < KL_LEASE_MIN_S)
		return KL_LEASE_MIN_S;
	if (seconds > KL_LEASE_MAX_S)
		return KL_LEASE_MAX_S;
	return seconds;
}

/* src is known to fit (checked by the caller) */
static void copy_str(char *dst, const char *src)
{
	memcpy(dst, src, strlen(src) + 1);
}

static bool key_ok(const char *key)
{
	return key && *key && strlen(key) < KL_KEY_MAX;
}

static bool lease_has(const struct kl_lease *lease, const char *key)
{
	for (size_t i = 0; i < lease->count; i++) {
		if (strcmp(lease->keys[i], key) == 0)
			return true;
	}
	return false;
}

/* same keys, whatever the order (both sets hold no duplicates) */
static bool same_set(const struct kl_lease *a, const struct kl_lease *b)
{
	if (a->count != b->count)
		return false;
	for (size_t i = 0; i < a->count; i++) {
		if (!lease_has(b, a->keys[i]))
			return false;
	}
	return true;
}

static void remove_lease(struct kl_leases *l, size_t index)
{
	l->count--;
	if (index != l->count)
		l->items[index] = l->items[l->count];
	memset(&l->items[l->count], 0, sizeof(l->items[l->count]));
}

static long find_index(const struct kl_leases *l, const char *client)
{
	for (size_t i = 0; i < l->count; i++) {
		if (strcmp(l->items[i].client, client) == 0)
			return (long)i;
	}
	return -1;
}

void kl_leases_init(struct kl_leases *l)
{
	memset(l, 0, sizeof(*l));
}

enum kl_renew_result kl_renew(struct kl_leases *l, const char *client, const char *const *keys, size_t count,
			      uint64_t expires_ms, bool *changed, size_t *capped)
{
	if (changed)
		*changed = false;
	if (capped)
		*capped = 0;
	if (!client || !*client || strlen(client) >= KL_CLIENT_MAX)
		return KL_BAD_CLIENT;

	struct kl_lease next;
	memset(&next, 0, sizeof(next));
	copy_str(next.client, client);
	next.expires_ms = expires_ms;
	for (size_t i = 0; i < count; i++) {
		if (!key_ok(keys[i]) || lease_has(&next, keys[i]))
			continue;
		if (next.count == KL_MAX_SCENES) {
			if (capped)
				(*capped)++;
			continue;
		}
		copy_str(next.keys[next.count++], keys[i]);
	}

	const long index = find_index(l, client);
	if (next.count == 0) {
		if (index >= 0) {
			remove_lease(l, (size_t)index);
			if (changed)
				*changed = true;
		}
		return KL_RELEASED;
	}
	if (index < 0) {
		if (l->count == KL_MAX_CLIENTS)
			return KL_TOO_MANY_CLIENTS;
		l->items[l->count++] = next;
		if (changed)
			*changed = true;
		return KL_RENEWED;
	}
	if (changed)
		*changed = !same_set(&l->items[index], &next);
	l->items[index] = next;
	return KL_RENEWED;
}

bool kl_release(struct kl_leases *l, const char *client)
{
	if (!client)
		return false;
	const long index = find_index(l, client);
	if (index < 0)
		return false;
	remove_lease(l, (size_t)index);
	return true;
}

size_t kl_expire(struct kl_leases *l, uint64_t now_ms)
{
	size_t dropped = 0;
	for (size_t i = 0; i < l->count;) {
		if (l->items[i].expires_ms <= now_ms) {
			remove_lease(l, i);
			dropped++;
		} else {
			i++;
		}
	}
	return dropped;
}

size_t kl_forget_key(struct kl_leases *l, const char *key)
{
	if (!key_ok(key))
		return 0;
	size_t hits = 0;
	for (size_t i = 0; i < l->count;) {
		struct kl_lease *lease = &l->items[i];
		for (size_t k = 0; k < lease->count; k++) {
			if (strcmp(lease->keys[k], key) == 0) {
				lease->count--;
				if (k != lease->count)
					memcpy(lease->keys[k], lease->keys[lease->count], KL_KEY_MAX);
				memset(lease->keys[lease->count], 0, KL_KEY_MAX);
				hits++;
				break;
			}
		}
		if (lease->count == 0)
			remove_lease(l, i);
		else
			i++;
	}
	return hits;
}

void kl_leases_clear(struct kl_leases *l)
{
	kl_leases_init(l);
}

const struct kl_lease *kl_find(const struct kl_leases *l, const char *client)
{
	if (!client)
		return NULL;
	const long index = find_index(l, client);
	return index < 0 ? NULL : &l->items[index];
}

static bool keys_have(const kl_key_t *keys, size_t count, const char *key)
{
	for (size_t i = 0; i < count; i++) {
		if (strcmp(keys[i], key) == 0)
			return true;
	}
	return false;
}

size_t kl_union(const struct kl_leases *l, kl_key_t *out, size_t max)
{
	size_t n = 0;
	for (size_t i = 0; i < l->count; i++) {
		const struct kl_lease *lease = &l->items[i];
		for (size_t k = 0; k < lease->count && n < max; k++) {
			if (!keys_have(out, n, lease->keys[k]))
				memcpy(out[n++], lease->keys[k], KL_KEY_MAX);
		}
	}
	return n;
}

/* ------------------------------------------------------------- ledger -- */

void kl_ledger_init(struct kl_ledger *g)
{
	memset(g, 0, sizeof(*g));
}

bool kl_ledger_holds(const struct kl_ledger *g, const char *key)
{
	return key && keys_have(g->keys, g->count, key);
}

/* takes the entry out of the ledger before dropping it, so a drop that ends up back in here can't drop it again */
static void drop_index(struct kl_ledger *g, size_t index, kl_drop_fn drop, void *ctx)
{
	kl_key_t key;
	memcpy(key, g->keys[index], KL_KEY_MAX);
	void *handle = g->handles[index];
	g->count--;
	if (index != g->count) {
		memcpy(g->keys[index], g->keys[g->count], KL_KEY_MAX);
		g->handles[index] = g->handles[g->count];
	}
	memset(g->keys[g->count], 0, KL_KEY_MAX);
	g->handles[g->count] = NULL;
	drop(ctx, key, handle);
}

void kl_ledger_sync(struct kl_ledger *g, const kl_key_t *want, size_t want_count, kl_acquire_fn acquire,
		    kl_drop_fn drop, void *ctx, size_t *acquired, size_t *dropped)
{
	size_t a = 0, d = 0;
	for (size_t i = 0; i < g->count;) {
		if (keys_have(want, want_count, g->keys[i])) {
			i++;
		} else {
			drop_index(g, i, drop, ctx);
			d++;
		}
	}
	for (size_t i = 0; i < want_count && g->count < KL_MAX_KEYS; i++) {
		if (!key_ok(want[i]) || kl_ledger_holds(g, want[i]))
			continue;
		void *handle = acquire(ctx, want[i]);
		if (!handle)
			continue;
		memcpy(g->keys[g->count], want[i], KL_KEY_MAX);
		g->handles[g->count] = handle;
		g->count++;
		a++;
	}
	if (acquired)
		*acquired = a;
	if (dropped)
		*dropped = d;
}

size_t kl_ledger_clear(struct kl_ledger *g, kl_drop_fn drop, void *ctx)
{
	size_t n = 0;
	while (g->count > 0) {
		drop_index(g, g->count - 1, drop, ctx);
		n++;
	}
	return n;
}
