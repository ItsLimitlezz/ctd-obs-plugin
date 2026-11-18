/* ctd-keep-live-tests: lease and show-reference bookkeeping for KeepScenesLive (src/obs/keep-live-core.c). No libobs.
 * The fake scenes below count their show references, so every test also checks that nothing is ever dropped twice or
 * left behind. Run with ctest (test/keep-live/CMakeLists.txt). */
#include "keep-live-core.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                            \
	do {                                                                   \
		if (!(cond)) {                                                 \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                            \
		}                                                              \
	} while (0)

/* --------------------------------------------------------- fake scenes -- */

#define SCENES 40

struct scene {
	char key[KL_KEY_MAX];
	bool exists;
	int show_refs;  /* what obs_source_inc_showing / dec_showing would count */
	int underflows; /* a drop with no reference to drop */
	int acquires, drops;
};

static struct scene scenes[SCENES];
static struct kl_ledger ledger;
static struct kl_leases leases;
static kl_key_t want[KL_MAX_KEYS];

static struct scene *find_scene(const char *key)
{
	for (int i = 0; i < SCENES; i++) {
		if (scenes[i].exists && strcmp(scenes[i].key, key) == 0)
			return &scenes[i];
	}
	return NULL;
}

static void *acquire(void *ctx, const char *key)
{
	(void)ctx;
	struct scene *s = find_scene(key);
	if (!s)
		return NULL;
	s->show_refs++;
	s->acquires++;
	return s;
}

static void drop(void *ctx, const char *key, void *handle)
{
	(void)ctx;
	struct scene *s = handle;
	if (strcmp(s->key, key) != 0) {
		printf("FAIL drop: handle for %s dropped as %s\n", s->key, key);
		failures++;
	}
	s->drops++;
	if (s->show_refs <= 0)
		s->underflows++;
	else
		s->show_refs--;
}

static void reset(void)
{
	memset(scenes, 0, sizeof(scenes));
	for (int i = 0; i < SCENES; i++) {
		snprintf(scenes[i].key, sizeof(scenes[i].key), "00000000-0000-0000-0000-%012d", i);
		scenes[i].exists = true;
	}
	kl_leases_init(&leases);
	kl_ledger_init(&ledger);
}

static const char *K(int i)
{
	return scenes[i].key;
}

/* what the plugin does after every change: move the ledger to the leases */
static void sync(void)
{
	const size_t n = kl_union(&leases, want, KL_MAX_KEYS);
	kl_ledger_sync(&ledger, want, n, acquire, drop, NULL, NULL, NULL);
}

/* every scene holds exactly one reference if some lease wants it and it exists, none otherwise; never underflows */
static void check_consistent(const char *where)
{
	for (int i = 0; i < SCENES; i++) {
		bool wanted = false;
		for (size_t c = 0; c < leases.count; c++) {
			for (size_t k = 0; k < leases.items[c].count; k++)
				wanted = wanted || strcmp(leases.items[c].keys[k], scenes[i].key) == 0;
		}
		const int expect = wanted && scenes[i].exists ? 1 : 0;
		if (scenes[i].show_refs != expect || scenes[i].underflows) {
			printf("FAIL %s: scene %d has %d show refs (want %d), %d underflows\n", where, i,
			       scenes[i].show_refs, expect, scenes[i].underflows);
			failures++;
		}
	}
}

static enum kl_renew_result renew(const char *client, const int *ids, size_t n, uint64_t expires)
{
	const char *keys[64];
	for (size_t i = 0; i < n; i++)
		keys[i] = K(ids[i]);
	return kl_renew(&leases, client, keys, n, expires, NULL, NULL);
}

/* --------------------------------------------------------------- tests -- */

static void test_clamp(void)
{
	CHECK(kl_clamp_lease(0) == KL_LEASE_DEFAULT_S);
	CHECK(kl_clamp_lease(-5) == KL_LEASE_DEFAULT_S);
	CHECK(kl_clamp_lease(1) == KL_LEASE_MIN_S);
	CHECK(kl_clamp_lease(3) == 3);
	CHECK(kl_clamp_lease(12.5) == 12.5);
	CHECK(kl_clamp_lease(30) == 30);
	CHECK(kl_clamp_lease(1000) == KL_LEASE_MAX_S);
}

static void test_renew_replaces_set(void)
{
	reset();
	const int first[] = {1, 2, 3};
	CHECK(renew("studio", first, 3, 10000) == KL_RENEWED);
	sync();
	check_consistent("first set");
	CHECK(ledger.count == 3);

	/* renewing with 2, 3, 4: 1 is dropped once, 4 taken once, 2 and 3 untouched */
	bool changed = false;
	const char *keys[] = {K(2), K(3), K(4)};
	CHECK(kl_renew(&leases, "studio", keys, 3, 20000, &changed, NULL) == KL_RENEWED);
	CHECK(changed);
	sync();
	check_consistent("second set");
	CHECK(scenes[1].drops == 1);
	CHECK(scenes[2].acquires == 1 && scenes[2].drops == 0);
	CHECK(scenes[4].acquires == 1);

	/* the same set again (other order) is no change, and renews the expiry */
	const char *same[] = {K(4), K(2), K(3)};
	CHECK(kl_renew(&leases, "studio", same, 3, 30000, &changed, NULL) == KL_RENEWED);
	CHECK(!changed);
	CHECK(kl_find(&leases, "studio")->expires_ms == 30000);
	sync();
	CHECK(scenes[2].acquires == 1 && scenes[3].acquires == 1 && scenes[4].acquires == 1);
	check_consistent("same set");
}

static void test_dedupe_and_bad_keys(void)
{
	reset();
	char longkey[KL_KEY_MAX + 8];
	memset(longkey, 'x', sizeof(longkey) - 1);
	longkey[sizeof(longkey) - 1] = 0;
	const char *keys[] = {K(5), K(5), "", NULL, longkey, K(6)};
	CHECK(kl_renew(&leases, "a", keys, 6, 1000, NULL, NULL) == KL_RENEWED);
	CHECK(kl_find(&leases, "a")->count == 2);
	sync();
	CHECK(scenes[5].show_refs == 1);
	check_consistent("dedupe");
}

static void test_bad_client(void)
{
	reset();
	const int ids[] = {1};
	CHECK(renew("", ids, 1, 1000) == KL_BAD_CLIENT);
	CHECK(renew(NULL, ids, 1, 1000) == KL_BAD_CLIENT);
	char longclient[KL_CLIENT_MAX + 1];
	memset(longclient, 'c', KL_CLIENT_MAX);
	longclient[KL_CLIENT_MAX] = 0;
	CHECK(renew(longclient, ids, 1, 1000) == KL_BAD_CLIENT);
	CHECK(leases.count == 0);
}

static void test_cap(void)
{
	reset();
	int ids[20];
	for (int i = 0; i < 20; i++)
		ids[i] = i;
	const char *keys[20];
	for (int i = 0; i < 20; i++)
		keys[i] = K(ids[i]);
	size_t capped = 0;
	CHECK(kl_renew(&leases, "big", keys, 20, 1000, NULL, &capped) == KL_RENEWED);
	CHECK(capped == 20 - KL_MAX_SCENES);
	CHECK(kl_find(&leases, "big")->count == KL_MAX_SCENES);
	sync();
	CHECK(ledger.count == KL_MAX_SCENES);
	check_consistent("cap");
}

static void test_too_many_clients(void)
{
	reset();
	const int ids[] = {0};
	char name[16];
	for (int i = 0; i < KL_MAX_CLIENTS; i++) {
		snprintf(name, sizeof(name), "c%d", i);
		CHECK(renew(name, ids, 1, 1000) == KL_RENEWED);
	}
	CHECK(renew("one-more", ids, 1, 1000) == KL_TOO_MANY_CLIENTS);
	/* an existing client can still renew */
	CHECK(renew("c3", ids, 1, 2000) == KL_RENEWED);
	sync();
	/* 32 clients want scene 0: one show reference */
	CHECK(scenes[0].show_refs == 1 && scenes[0].acquires == 1);
	check_consistent("many clients");
}

static void test_shared_scene_two_clients(void)
{
	reset();
	const int a[] = {1, 2};
	const int b[] = {2, 3};
	renew("mac", a, 2, 10000);
	renew("phone", b, 2, 15000);
	sync();
	CHECK(scenes[2].show_refs == 1);
	check_consistent("two clients");

	/* mac goes away: 1 dropped, 2 still held for phone */
	CHECK(kl_release(&leases, "mac"));
	CHECK(!kl_release(&leases, "mac")); /* twice is a no-op */
	sync();
	CHECK(scenes[1].show_refs == 0 && scenes[1].drops == 1);
	CHECK(scenes[2].show_refs == 1 && scenes[2].drops == 0);
	check_consistent("after release");
}

static void test_expiry(void)
{
	reset();
	const int a[] = {1};
	const int b[] = {2};
	renew("a", a, 1, 10000);
	renew("b", b, 1, 20000);
	sync();
	CHECK(kl_expire(&leases, 9999) == 0);
	CHECK(kl_expire(&leases, 10000) == 1);
	sync();
	CHECK(scenes[1].show_refs == 0 && scenes[2].show_refs == 1);
	CHECK(kl_expire(&leases, 50000) == 1);
	sync();
	check_consistent("expiry");
	CHECK(ledger.count == 0);
	/* syncing again with nothing wanted drops nothing more */
	sync();
	CHECK(scenes[1].drops == 1 && scenes[2].drops == 1);
}

static void test_empty_renew_releases(void)
{
	reset();
	const int a[] = {7};
	renew("a", a, 1, 1000);
	sync();
	bool changed = false;
	CHECK(kl_renew(&leases, "a", NULL, 0, 2000, &changed, NULL) == KL_RELEASED);
	CHECK(changed);
	CHECK(kl_find(&leases, "a") == NULL);
	sync();
	check_consistent("empty renew");
	CHECK(scenes[7].drops == 1);
}

static void test_scene_removed(void)
{
	reset();
	const int a[] = {1, 2};
	const int b[] = {2};
	renew("a", a, 2, 1000);
	renew("b", b, 1, 1000);
	sync();
	/* scene 2 is removed in OBS: forgotten from both leases; b is left empty and goes */
	CHECK(kl_forget_key(&leases, K(2)) == 2);
	CHECK(kl_find(&leases, "b") == NULL);
	CHECK(kl_find(&leases, "a")->count == 1);
	sync();
	CHECK(scenes[2].show_refs == 0 && scenes[2].drops == 1);
	check_consistent("removed");
}

static void test_scene_destroyed_before_sync(void)
{
	reset();
	const int a[] = {1, 2};
	renew("a", a, 2, 1000);
	/* scene 2 vanishes before the first sync: it just isn't held, and nothing is dropped for it later */
	scenes[2].exists = false;
	sync();
	CHECK(ledger.count == 1);
	CHECK(scenes[2].acquires == 0);
	kl_leases_clear(&leases);
	sync();
	CHECK(scenes[2].drops == 0 && scenes[1].drops == 1);
	CHECK(ledger.count == 0);
}

static void test_clear_and_unload(void)
{
	reset();
	const int a[] = {1, 2, 3};
	renew("a", a, 3, 1000);
	sync();
	/* scene collection change / module unload: leases cleared and the ledger emptied in one go */
	kl_leases_clear(&leases);
	CHECK(kl_ledger_clear(&ledger, drop, NULL) == 3);
	CHECK(kl_ledger_clear(&ledger, drop, NULL) == 0);
	sync();
	for (int i = 1; i <= 3; i++)
		CHECK(scenes[i].drops == 1 && scenes[i].show_refs == 0 && !scenes[i].underflows);
}

/* a drop that leads straight back into a sync (as an OBS hide callback might) sees the entry already gone */
static int reentrant_depth;
static void drop_reentrant(void *ctx, const char *key, void *handle)
{
	drop(ctx, key, handle);
	if (reentrant_depth++ == 0)
		kl_ledger_sync(&ledger, want, 0, acquire, drop, NULL, NULL, NULL);
}

static void test_reentrant_drop(void)
{
	reset();
	const int a[] = {1, 2, 3};
	renew("a", a, 3, 1000);
	sync();
	reentrant_depth = 0;
	CHECK(kl_ledger_clear(&ledger, drop_reentrant, NULL) >= 1);
	CHECK(ledger.count == 0);
	for (int i = 1; i <= 3; i++)
		CHECK(scenes[i].drops == 1 && !scenes[i].underflows);
}

/* a long random run of renewals, releases, expiries, removals and clears never leaves the counts wrong */
static unsigned rng = 12345;
static unsigned next_rand(void)
{
	rng = rng * 1103515245u + 12345u;
	return (rng >> 16) & 0x7fff;
}

static void test_random(void)
{
	reset();
	uint64_t now = 0;
	char name[8];
	for (int step = 0; step < 20000; step++) {
		now += next_rand() % 500;
		snprintf(name, sizeof(name), "c%u", next_rand() % 6);
		switch (next_rand() % 10) {
		case 0:
			kl_release(&leases, name);
			break;
		case 1:
			kl_expire(&leases, now);
			break;
		case 2: {
			const int victim = (int)(next_rand() % SCENES);
			kl_forget_key(&leases, K(victim));
			break;
		}
		case 3:
			if (next_rand() % 50 == 0) {
				kl_leases_clear(&leases);
				kl_ledger_clear(&ledger, drop, NULL);
			}
			break;
		default: {
			int ids[24];
			const size_t n = next_rand() % 24;
			for (size_t i = 0; i < n; i++)
				ids[i] = (int)(next_rand() % SCENES);
			renew(name, ids, n, now + 3000 + next_rand() % 27000);
			break;
		}
		}
		if (next_rand() % 3 == 0)
			sync();
	}
	sync();
	check_consistent("random");
	kl_leases_clear(&leases);
	kl_ledger_clear(&ledger, drop, NULL);
	for (int i = 0; i < SCENES; i++)
		CHECK(scenes[i].show_refs == 0 && scenes[i].underflows == 0 && scenes[i].acquires == scenes[i].drops);
}

int main(void)
{
	test_clamp();
	test_renew_replaces_set();
	test_dedupe_and_bad_keys();
	test_bad_client();
	test_cap();
	test_too_many_clients();
	test_shared_scene_two_clients();
	test_expiry();
	test_empty_renew_releases();
	test_scene_removed();
	test_scene_destroyed_before_sync();
	test_clear_and_unload();
	test_reentrant_drop();
	test_random();
	if (failures) {
		printf("%d failure(s)\n", failures);
		return 1;
	}
	printf("ctd-keep-live-tests: all passed\n");
	return 0;
}
