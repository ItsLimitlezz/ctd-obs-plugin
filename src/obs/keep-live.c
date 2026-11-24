/*
 * Keep scenes live for CTD apps: the OBS side of KeepScenesLive. See keep-live.h; the lease and reference bookkeeping
 * (and its tests) is keep-live-core.c.
 *
 * Threads:
 *   - requests arrive on obs-websocket's thread, lease expiry is checked from a tick callback (graphics thread), scene
 *     removals and renames come from libobs signals (usually the UI thread): they all only change `leases`, under
 *     `lock`, and then ask for a sync.
 *   - the sync runs on the UI thread only: it reads the wanted set under the lock, then takes or gives back show
 *     references outside it (a source's show/hide callback may do anything), recording what it holds in `ledger`,
 *     which nothing else touches. A sync is idempotent, so running one directly on the UI thread and having another
 *     queued is harmless.
 */

#include "keep-live.h"
#include "keep-live-core.h"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>
#include <util/platform.h>
#include <util/threading.h>

#include <stdio.h>
#include <string.h>

#define SETTING_SECTION "CTDLive"
#define SETTING_KEY "KeepScenesLive"
#define EXPIRY_CHECK_S 0.5f
#define MAX_REQUEST_SCENES 64 /* scene entries read from one request; more are counted as capped */
#define STATUS_LOG_S 60.0f    /* how often the lease and kept-live counts are logged while any are held */

static pthread_mutex_t lock;
static pthread_mutex_t status_lock;  /* GetKeepScenesLive's copy of the leases */
static struct kl_leases status_copy; /* too big for obs-websocket's stack */
static struct kl_leases leases;      /* under lock */
static struct kl_ledger ledger;      /* UI thread only */
static kl_key_t wanted[KL_MAX_KEYS]; /* UI thread only: scratch for sync */
static volatile long enabled = 1;
static volatile long running;   /* between start and stop */
static volatile long accepting; /* new leases are taken: running, and OBS isn't closing */
static volatile long sync_queued;
static bool in_sync;               /* UI thread only */
static float tick_elapsed;         /* graphics thread only */
static float status_elapsed;       /* graphics thread only */
static volatile long live_count;   /* ledger.count, for the status log on the graphics thread */
static volatile long capped_count; /* wanted scenes over KL_MAX_LIVE at the last sync */

static uint64_t now_ms(void)
{
	return os_gettime_ns() / 1000000;
}

/* ---------------------------------------------------- show references -- */

static void *acquire_scene(void *ctx, const char *uuid)
{
	UNUSED_PARAMETER(ctx);
	obs_source_t *source = obs_get_source_by_uuid(uuid);
	if (!source)
		return NULL;
	obs_weak_source_t *weak = NULL;
	if (obs_source_is_scene(source) && !obs_source_removed(source)) {
		obs_source_inc_showing(source);
		weak = obs_source_get_weak_source(source);
		blog(LOG_DEBUG, "[CTD] keep-live: showing \"%s\"", obs_source_get_name(source));
	}
	obs_source_release(source);
	return weak;
}

static void drop_scene(void *ctx, const char *uuid, void *handle)
{
	UNUSED_PARAMETER(ctx);
	UNUSED_PARAMETER(uuid);
	obs_weak_source_t *weak = handle;
	/* a scene that is already destroyed took its show references with it */
	obs_source_t *source = obs_weak_source_get_source(weak);
	if (source) {
		obs_source_dec_showing(source);
		blog(LOG_DEBUG, "[CTD] keep-live: stopped showing \"%s\"", obs_source_get_name(source));
		obs_source_release(source);
	}
	obs_weak_source_release(weak);
}

/* UI thread */
static void sync_now(void)
{
	if (in_sync)
		return;
	in_sync = true;
	pthread_mutex_lock(&lock);
	size_t n = os_atomic_load_long(&running) ? kl_union(&leases, wanted, KL_MAX_KEYS) : 0;
	pthread_mutex_unlock(&lock);
	/* Hard cap for all clients together: every scene kept live keeps its sources (browser sources included)
	 * rendering, so a client asking for many scenes must not keep them all running. The first KL_MAX_LIVE in lease
	 * order win; the rest are not shown (and a scene dropped by the cap gives its reference back below). */
	const size_t over = n > KL_MAX_LIVE ? n - KL_MAX_LIVE : 0;
	if (over)
		n = KL_MAX_LIVE;
	size_t acquired = 0, dropped = 0;
	kl_ledger_sync(&ledger, wanted, n, acquire_scene, drop_scene, NULL, &acquired, &dropped);
	in_sync = false;
	os_atomic_set_long(&live_count, (long)ledger.count);
	if (os_atomic_set_long(&capped_count, (long)over) != (long)over && over)
		blog(LOG_WARNING, "[CTD] keep-live: CTD apps asked for %zu more scene(s) than the %d kept live at once",
		     over, KL_MAX_LIVE);
	if (acquired || dropped)
		blog(LOG_INFO, "[CTD] keep-live: %zu scene(s) kept live for CTD apps (+%zu, -%zu)", ledger.count,
		     acquired, dropped);
}

static void sync_task(void *param)
{
	UNUSED_PARAMETER(param);
	os_atomic_set_long(&sync_queued, 0);
	if (os_atomic_load_long(&running))
		sync_now();
}

/* any thread */
static void request_sync(void)
{
	if (obs_in_task_thread(OBS_TASK_UI) && !in_sync) {
		sync_now();
		return;
	}
	if (os_atomic_compare_swap_long(&sync_queued, 0, 1))
		obs_queue_task(OBS_TASK_UI, sync_task, NULL, false);
}

/* UI thread: drops every lease and every show reference now (collection change, setting off, unload) */
static void release_all(const char *why)
{
	pthread_mutex_lock(&lock);
	const size_t clients = leases.count;
	kl_leases_clear(&leases);
	pthread_mutex_unlock(&lock);
	size_t scenes = 0;
	if (in_sync) {
		/* reached from a show/hide callback inside a sync: the queued sync drops them */
		request_sync();
	} else {
		in_sync = true;
		scenes = kl_ledger_clear(&ledger, drop_scene, NULL);
		in_sync = false;
		os_atomic_set_long(&live_count, 0);
	}
	if (clients || scenes)
		blog(LOG_INFO, "[CTD] keep-live: %s: released %zu scene(s) for %zu client(s)", why, scenes, clients);
}

/* ------------------------------------------------------------ signals -- */

static void scene_gone(void *param, calldata_t *cd)
{
	const char *why = param;
	obs_source_t *source = calldata_ptr(cd, "source");
	if (!source || !obs_source_is_scene(source))
		return;
	const char *uuid = obs_source_get_uuid(source);
	pthread_mutex_lock(&lock);
	const size_t hits = kl_forget_key(&leases, uuid);
	pthread_mutex_unlock(&lock);
	if (hits) {
		blog(LOG_INFO, "[CTD] keep-live: scene \"%s\" %s: no longer kept live", obs_source_get_name(source),
		     why);
		request_sync();
	}
}

static void frontend_event(enum obs_frontend_event event, void *param)
{
	UNUSED_PARAMETER(param);
	switch (event) {
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
		release_all("scene collection changing");
		break;
	case OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN:
	case OBS_FRONTEND_EVENT_EXIT:
		os_atomic_set_long(&accepting, 0);
		release_all("OBS is closing");
		break;
	default:
		break;
	}
}

/* graphics thread */
static void tick(void *param, float seconds)
{
	UNUSED_PARAMETER(param);
	tick_elapsed += seconds;
	status_elapsed += seconds;
	if (tick_elapsed < EXPIRY_CHECK_S)
		return;
	tick_elapsed = 0.0f;
	pthread_mutex_lock(&lock);
	const size_t expired = leases.count ? kl_expire(&leases, now_ms()) : 0;
	const size_t clients = leases.count;
	pthread_mutex_unlock(&lock);
	if (status_elapsed >= STATUS_LOG_S) {
		status_elapsed = 0.0f;
		const long live = os_atomic_load_long(&live_count);
		if (clients || live)
			blog(LOG_INFO, "[CTD] keep-live: status: %zu client lease(s), %ld scene(s) kept live (max %d)",
			     clients, live, KL_MAX_LIVE);
	}
	if (expired) {
		blog(LOG_INFO, "[CTD] keep-live: %zu client lease(s) ran out without a renewal", expired);
		request_sync();
	}
}

/* ------------------------------------------------------------ setting -- */

bool keep_live_enabled(void)
{
	return os_atomic_load_long(&enabled) != 0;
}

void keep_live_set_enabled(bool on)
{
	if (os_atomic_set_long(&enabled, on ? 1 : 0) != (on ? 1 : 0))
		blog(LOG_INFO, "[CTD] keep-live: \"Let CTD apps keep multiview scenes live\" turned %s",
		     on ? "on" : "off");
	config_t *config = obs_frontend_get_user_config();
	if (config) {
		config_set_bool(config, SETTING_SECTION, SETTING_KEY, on);
		config_save_safe(config, "tmp", NULL);
	}
	if (!on)
		release_all("turned off");
}

/* ------------------------------------------------------------ requests -- */

static void fail(obs_data_t *response, const char *error)
{
	obs_data_set_bool(response, "ok", false);
	obs_data_set_bool(response, "enabled", keep_live_enabled());
	obs_data_set_string(response, "error", error);
}

static void push_scene(obs_data_array_t *list, const char *name, const char *uuid)
{
	obs_data_t *item = obs_data_create();
	if (name)
		obs_data_set_string(item, "sceneName", name);
	if (uuid)
		obs_data_set_string(item, "sceneUuid", uuid);
	obs_data_array_push_back(list, item);
	obs_data_release(item);
}

/* KeepScenesLive {clientId, scenes: [{sceneName} | {sceneUuid}], leaseSeconds?} */
void keep_live_request(obs_data_t *request, obs_data_t *response)
{
	obs_data_set_int(response, "maxScenes", KL_MAX_SCENES);
	if (!os_atomic_load_long(&accepting)) {
		fail(response, "OBS is closing");
		return;
	}
	if (!keep_live_enabled()) {
		fail(response, "Turned off in OBS: \"Let CTD apps keep multiview scenes live\"");
		return;
	}
	const char *client = obs_data_get_string(request, "clientId");
	const double lease_s = kl_clamp_lease(obs_data_get_double(request, "leaseSeconds"));

	/* obs-websocket drops plain strings inside arrays on the way to a vendor, so each scene is an object */
	obs_data_array_t *scenes = obs_data_get_array(request, "scenes");
	const size_t count = scenes ? obs_data_array_count(scenes) : 0;
	char uuids[MAX_REQUEST_SCENES][KL_KEY_MAX];
	const char *keys[MAX_REQUEST_SCENES];
	size_t key_count = 0, over = 0;
	obs_data_array_t *held = obs_data_array_create();
	obs_data_array_t *missing = obs_data_array_create();
	for (size_t i = 0; i < count; i++) {
		obs_data_t *item = obs_data_array_item(scenes, i);
		const char *name = obs_data_get_string(item, "sceneName");
		const char *uuid = obs_data_get_string(item, "sceneUuid");
		obs_source_t *source = *uuid   ? obs_get_source_by_uuid(uuid)
				       : *name ? obs_get_source_by_name(name)
					       : NULL;
		if (source && obs_source_is_scene(source) && !obs_source_removed(source)) {
			const char *id = obs_source_get_uuid(source);
			bool dup = false;
			for (size_t k = 0; k < key_count; k++)
				dup = dup || strcmp(keys[k], id) == 0;
			if (dup) {
				/* named twice: kept once */
			} else if (key_count == KL_MAX_SCENES || key_count == MAX_REQUEST_SCENES) {
				over++;
			} else {
				snprintf(uuids[key_count], KL_KEY_MAX, "%s", id);
				keys[key_count] = uuids[key_count];
				key_count++;
				push_scene(held, obs_source_get_name(source), id);
			}
		} else if (*name || *uuid) {
			push_scene(missing, *name ? name : NULL, *uuid ? uuid : NULL);
		}
		obs_source_release(source);
		obs_data_release(item);
	}
	obs_data_array_release(scenes);

	bool changed = false, closing = false;
	enum kl_renew_result result = KL_RELEASED;
	pthread_mutex_lock(&lock);
	/* checked again under the lock, so nothing is added after OBS started closing (release_all clears under it) */
	closing = !os_atomic_load_long(&accepting) || !keep_live_enabled();
	if (!closing)
		result = kl_renew(&leases, client, keys, key_count, now_ms() + (uint64_t)(lease_s * 1000.0), &changed,
				  NULL);
	pthread_mutex_unlock(&lock);
	if (closing) {
		obs_data_array_release(held);
		obs_data_array_release(missing);
		fail(response, keep_live_enabled() ? "OBS is closing"
						   : "Turned off in OBS: \"Let CTD apps keep multiview scenes live\"");
		return;
	}

	switch (result) {
	case KL_BAD_CLIENT:
		fail(response, "clientId is required (1 to 63 characters)");
		break;
	case KL_TOO_MANY_CLIENTS:
		fail(response, "Too many CTD apps are keeping scenes live; try again shortly");
		break;
	case KL_RENEWED:
	case KL_RELEASED:
		obs_data_set_bool(response, "ok", true);
		obs_data_set_bool(response, "enabled", true);
		obs_data_set_string(response, "clientId", client);
		obs_data_set_double(response, "leaseSeconds", lease_s);
		obs_data_set_array(response, "held", held);
		obs_data_set_array(response, "missing", missing);
		obs_data_set_int(response, "capped", (long long)over);
		if (changed)
			blog(LOG_INFO, "[CTD] keep-live: client \"%s\" keeps %zu scene(s) live (lease %.0f s)", client,
			     key_count, lease_s);
		break;
	}
	obs_data_array_release(held);
	obs_data_array_release(missing);
	if (changed)
		request_sync();
}

/* ReleaseScenesLive {clientId} */
void keep_live_release_request(obs_data_t *request, obs_data_t *response)
{
	const char *client = obs_data_get_string(request, "clientId");
	pthread_mutex_lock(&lock);
	const bool released = kl_release(&leases, client);
	pthread_mutex_unlock(&lock);
	obs_data_set_bool(response, "ok", true);
	obs_data_set_bool(response, "released", released);
	if (released) {
		blog(LOG_INFO, "[CTD] keep-live: client \"%s\" released its scenes", client);
		request_sync();
	}
}

/* GetKeepScenesLive: the setting, the limits and who holds what (names resolved now) */
void keep_live_status_request(obs_data_t *request, obs_data_t *response)
{
	UNUSED_PARAMETER(request);
	obs_data_set_bool(response, "ok", true);
	obs_data_set_bool(response, "enabled", keep_live_enabled());
	obs_data_set_int(response, "maxScenes", KL_MAX_SCENES);
	obs_data_set_int(response, "maxLiveScenes", KL_MAX_LIVE);
	obs_data_set_int(response, "liveScenes", os_atomic_load_long(&live_count));
	obs_data_set_int(response, "leaseMinSeconds", KL_LEASE_MIN_S);
	obs_data_set_int(response, "leaseMaxSeconds", KL_LEASE_MAX_S);
	obs_data_set_int(response, "leaseDefaultSeconds", KL_LEASE_DEFAULT_S);

	pthread_mutex_lock(&status_lock);
	pthread_mutex_lock(&lock);
	status_copy = leases;
	pthread_mutex_unlock(&lock);
	const struct kl_leases *copy = &status_copy;
	const uint64_t now = now_ms();
	obs_data_array_t *clients = obs_data_array_create();
	for (size_t i = 0; i < copy->count; i++) {
		const struct kl_lease *lease = &copy->items[i];
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "clientId", lease->client);
		obs_data_set_double(item, "expiresInSeconds",
				    lease->expires_ms > now ? (double)(lease->expires_ms - now) / 1000.0 : 0.0);
		obs_data_array_t *list = obs_data_array_create();
		for (size_t k = 0; k < lease->count; k++) {
			obs_source_t *source = obs_get_source_by_uuid(lease->keys[k]);
			push_scene(list, source ? obs_source_get_name(source) : NULL, lease->keys[k]);
			obs_source_release(source);
		}
		obs_data_set_array(item, "scenes", list);
		obs_data_array_release(list);
		obs_data_array_push_back(clients, item);
		obs_data_release(item);
	}
	pthread_mutex_unlock(&status_lock);
	obs_data_set_array(response, "clients", clients);
	obs_data_array_release(clients);
}

/* ---------------------------------------------------------------- API -- */

/* the signal handlers' param, the same pointer for connect and disconnect */
static const char removed_text[] = "was removed";
static const char renamed_text[] = "was renamed";

/* The locks live until the process ends: a request obs-websocket is still running when this module unloads finds
 * `running` off, never a destroyed lock. */
void keep_live_start(void)
{
	pthread_mutex_init(&lock, NULL);
	pthread_mutex_init(&status_lock, NULL);
	kl_leases_init(&leases);
	kl_ledger_init(&ledger);
	config_t *config = obs_frontend_get_user_config();
	if (config) {
		config_set_default_bool(config, SETTING_SECTION, SETTING_KEY, true);
		os_atomic_set_long(&enabled, config_get_bool(config, SETTING_SECTION, SETTING_KEY) ? 1 : 0);
	}
	signal_handler_t *sh = obs_get_signal_handler();
	signal_handler_connect(sh, "source_remove", scene_gone, (void *)removed_text);
	signal_handler_connect(sh, "source_rename", scene_gone, (void *)renamed_text);
	obs_frontend_add_event_callback(frontend_event, NULL);
	obs_add_tick_callback(tick, NULL);
	os_atomic_set_long(&running, 1);
	os_atomic_set_long(&accepting, 1);
	blog(LOG_INFO, "[CTD] keep-live: CTD apps %s keep multiview scenes live",
	     keep_live_enabled() ? "may" : "may not");
}

void keep_live_stop(void)
{
	os_atomic_set_long(&accepting, 0);
	if (!os_atomic_set_long(&running, 0))
		return;
	obs_remove_tick_callback(tick, NULL);
	obs_frontend_remove_event_callback(frontend_event, NULL);
	signal_handler_t *sh = obs_get_signal_handler();
	signal_handler_disconnect(sh, "source_remove", scene_gone, (void *)removed_text);
	signal_handler_disconnect(sh, "source_rename", scene_gone, (void *)renamed_text);
	/* a sync still queued finds running off and leaves the (now empty) ledger alone */
	release_all("unloading");
}
