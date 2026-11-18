/*
 * Auto-failover on dead feeds: the OBS side. See failover.h; the rules (and their tests) are failover-core.c.
 *
 * Threads:
 *   - "ctd-failover" checks twice a second: it reads every CTD Live Source's state (and its picture, for the sources
 *     that match pictures), which watched sources each scene shows, and the program scene; then asks the rules what to
 *     do. Only this thread touches `state`.
 *   - a switch is made on the UI thread (obs_queue_task), through the frontend, so it uses the current transition
 *     and, in Studio Mode, moves program and leaves preview alone.
 *   - `config`, the status and the on-program list are shared, under `lock`. The lock is never held while calling
 *     into libobs.
 */

#include "failover.h"
#include "ws-vendor.h"
#include "../multistream.h"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/bmem.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define CHECK_EVERY_MS 500
#define STARTUP_GRACE_MS 15000 /* after OBS starts or the scene collection changes: sources need to connect */
#define MAX_NESTING 8          /* scenes inside scenes */
#define SAVE_KEY "ctd-failover"
#define LIVE_SOURCE_ID "ctd_live_source"

struct sampled {
	char name[FO_NAME_MAX];
	bool has_signal;
	long frozen_ms;
	bool has_grid;
	uint8_t grid[FO_GRID_BYTES];
};

struct scene_view {
	char name[FO_NAME_MAX];
	DARRAY(char *) sources;
};

static struct {
	pthread_mutex_t lock;
	pthread_t thread;
	os_event_t *stop;
	bool running;
	bool lock_ready; /* the lock outlives a failed start: readers may still come */
	volatile long reset;   /* forget what was seen and wait out the start-up grace */
	volatile long closing; /* OBS is exiting: no more switches */

	/* under lock */
	struct fo_config config;
	struct failover_status status;
	size_t on_program_count;
	char on_program[FO_MAX_SOURCES][FO_NAME_MAX];

	/* the thread's own */
	struct fo_state state;
	struct fo_config tick_config; /* too big for a stack */
} fo;

static uint64_t now_ms(void)
{
	return os_gettime_ns() / 1000000;
}

static int64_t epoch_ms(void)
{
	return (int64_t)time(NULL) * 1000;
}

/* ------------------------------------------------------------- settings -- */

static void config_from_data(struct fo_config *c, obs_data_t *data)
{
	fo_config_defaults(c);
	if (!data)
		return;
	obs_data_set_default_int(data, "delayMs", FO_DELAY_DEFAULT_MS);
	obs_data_set_default_int(data, "frozenMs", FO_FROZEN_DEFAULT_MS);
	obs_data_set_default_int(data, "tolerance", FO_TOLERANCE_DEFAULT);
	obs_data_set_default_int(data, "returnAfterMs", FO_RETURN_DEFAULT_MS);
	obs_data_set_default_bool(data, "watchAll", true);
	c->enabled = obs_data_get_bool(data, "enabled");
	c->delay_ms = (int)obs_data_get_int(data, "delayMs");
	c->frozen_ms = (int)obs_data_get_int(data, "frozenMs");
	c->tolerance = (int)obs_data_get_int(data, "tolerance");
	c->return_when_back = obs_data_get_bool(data, "returnWhenBack");
	c->return_after_ms = (int)obs_data_get_int(data, "returnAfterMs");
	c->watch_all = obs_data_get_bool(data, "watchAll");

	obs_data_array_t *scenes = obs_data_get_array(data, "scenes");
	const size_t scene_count = scenes ? obs_data_array_count(scenes) : 0;
	for (size_t i = 0; i < scene_count && c->scene_count < FO_MAX_SCENES; i++) {
		obs_data_t *item = obs_data_array_item(scenes, i);
		snprintf(c->scenes[c->scene_count++], FO_NAME_MAX, "%s", obs_data_get_string(item, "name"));
		obs_data_release(item);
	}
	obs_data_array_release(scenes);

	obs_data_array_t *sources = obs_data_get_array(data, "sources");
	const size_t source_count = sources ? obs_data_array_count(sources) : 0;
	for (size_t i = 0; i < source_count && c->rule_count < FO_MAX_RULES; i++) {
		obs_data_t *item = obs_data_array_item(sources, i);
		struct fo_rule *r = &c->rules[c->rule_count++];
		fo_rule_defaults(r, obs_data_get_string(item, "source"));
		obs_data_set_default_bool(item, "watch", true);
		obs_data_set_default_bool(item, "signalLost", true);
		obs_data_set_default_bool(item, "bars", true);
		r->watch = obs_data_get_bool(item, "watch");
		r->on_signal_lost = obs_data_get_bool(item, "signalLost");
		r->on_bars = obs_data_get_bool(item, "bars");
		r->on_frozen = obs_data_get_bool(item, "frozen");
		r->has_reference = fo_grid_from_hex(obs_data_get_string(item, "picture"), r->reference);
		snprintf(r->target, FO_NAME_MAX, "%s", obs_data_get_string(item, "target"));
		obs_data_release(item);
	}
	obs_data_array_release(sources);
	fo_config_sanitize(c);
}

static obs_data_t *config_to_data(const struct fo_config *c)
{
	obs_data_t *data = obs_data_create();
	obs_data_set_bool(data, "enabled", c->enabled);
	obs_data_set_int(data, "delayMs", c->delay_ms);
	obs_data_set_int(data, "frozenMs", c->frozen_ms);
	obs_data_set_int(data, "tolerance", c->tolerance);
	obs_data_set_bool(data, "returnWhenBack", c->return_when_back);
	obs_data_set_int(data, "returnAfterMs", c->return_after_ms);
	obs_data_set_bool(data, "watchAll", c->watch_all);
	obs_data_array_t *scenes = obs_data_array_create();
	for (size_t i = 0; i < c->scene_count; i++) {
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "name", c->scenes[i]);
		obs_data_array_push_back(scenes, item);
		obs_data_release(item);
	}
	obs_data_set_array(data, "scenes", scenes);
	obs_data_array_release(scenes);
	obs_data_array_t *sources = obs_data_array_create();
	for (size_t i = 0; i < c->rule_count; i++) {
		const struct fo_rule *r = &c->rules[i];
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "source", r->source);
		obs_data_set_bool(item, "watch", r->watch);
		obs_data_set_bool(item, "signalLost", r->on_signal_lost);
		obs_data_set_bool(item, "bars", r->on_bars);
		obs_data_set_bool(item, "frozen", r->on_frozen);
		if (r->has_reference) {
			char hex[FO_GRID_HEX];
			fo_grid_to_hex(r->reference, hex);
			obs_data_set_string(item, "picture", hex);
		}
		obs_data_set_string(item, "target", r->target);
		obs_data_array_push_back(sources, item);
		obs_data_release(item);
	}
	obs_data_set_array(data, "sources", sources);
	obs_data_array_release(sources);
	return data;
}

/* The scene collection is being saved (write ours into it) or was just loaded (read ours, or the defaults). */
static void save_cb(obs_data_t *save_data, bool saving, void *unused)
{
	UNUSED_PARAMETER(unused);
	static struct fo_config scratch; /* UI thread only; too big for a stack */
	if (saving) {
		failover_get_config(&scratch);
		obs_data_t *data = config_to_data(&scratch);
		obs_data_set_obj(save_data, SAVE_KEY, data);
		obs_data_release(data);
		return;
	}
	obs_data_t *data = obs_data_get_obj(save_data, SAVE_KEY);
	config_from_data(&scratch, data);
	obs_data_release(data);
	pthread_mutex_lock(&fo.lock);
	fo.config = scratch;
	pthread_mutex_unlock(&fo.lock);
	os_atomic_set_long(&fo.reset, 1);
}

void failover_get_config(struct fo_config *out)
{
	if (!fo.lock_ready) {
		fo_config_defaults(out);
		return;
	}
	pthread_mutex_lock(&fo.lock);
	*out = fo.config;
	pthread_mutex_unlock(&fo.lock);
}

void failover_set_config(const struct fo_config *in)
{
	if (!fo.lock_ready)
		return;
	pthread_mutex_lock(&fo.lock);
	fo.config = *in;
	fo_config_sanitize(&fo.config);
	pthread_mutex_unlock(&fo.lock);
}

void failover_get_status(struct failover_status *out)
{
	memset(out, 0, sizeof(*out));
	if (!fo.lock_ready)
		return;
	pthread_mutex_lock(&fo.lock);
	*out = fo.status;
	pthread_mutex_unlock(&fo.lock);
}

bool failover_source_on_program(const char *source, char *scene, size_t scene_size)
{
	bool found = false;
	if (!fo.lock_ready)
		return false;
	pthread_mutex_lock(&fo.lock);
	for (size_t i = 0; i < fo.on_program_count && !found; i++)
		found = strcmp(fo.on_program[i], source ? source : "") == 0;
	if (found && scene && scene_size)
		snprintf(scene, scene_size, "%s", fo.status.program);
	pthread_mutex_unlock(&fo.lock);
	return found;
}

/* --------------------------------------------------------------- reading -- */

static bool is_live_source(obs_source_t *source)
{
	const char *id = obs_source_get_unversioned_id(source);
	return id && strcmp(id, LIVE_SOURCE_ID) == 0;
}

static bool read_grid(obs_source_t *source, uint8_t grid[FO_GRID_BYTES])
{
	calldata_t cd = {0};
	calldata_set_ptr(&cd, "grid", grid);
	const bool ok = proc_handler_call(obs_source_get_proc_handler(source), "get_picture_grid", &cd) &&
			calldata_bool(&cd, "ok");
	calldata_free(&cd);
	return ok;
}

struct collected {
	DARRAY(struct sampled) samples;
	DARRAY(struct scene_view) scenes;
	const struct fo_config *config;
};

static bool collect_source(void *param, obs_source_t *source)
{
	struct collected *all = param;
	if (!is_live_source(source))
		return true;
	const char *name = obs_source_get_name(source);
	if (!name || !*name)
		return true;
	calldata_t cd = {0};
	if (proc_handler_call(obs_source_get_proc_handler(source), "get_feed", &cd)) {
		obs_data_t *json = obs_data_create_from_json(calldata_string(&cd, "json"));
		if (json) {
			const char *state = obs_data_get_string(json, "state");
			/* H17: disconnected on purpose (or not started): not a dead feed, and not watched */
			const bool off = strcmp(state, "DISCONNECTED") == 0 || strcmp(state, "STOPPED") == 0;
			if (!off) {
				struct sampled *s = da_push_back_new(all->samples);
				snprintf(s->name, sizeof(s->name), "%s", name);
				/* STALLED is a gap that may still close; the engine calls it NO_SIGNAL soon after */
				s->has_signal = strcmp(state, "LIVE") == 0 || strcmp(state, "STALLED") == 0;
				s->frozen_ms = (long)obs_data_get_int(json, "frozenMs");
				/* its picture only when a rule looks at pictures (most never do) */
				struct fo_rule rule;
				if (s->has_signal && fo_rule_for(all->config, s->name, &rule) &&
				    (rule.on_bars || rule.has_reference))
					s->has_grid = read_grid(source, s->grid);
			}
			obs_data_release(json);
		}
	}
	calldata_free(&cd);
	return true;
}

struct walk {
	struct scene_view *view;
	int depth;
};

/* the watched sources visible in a scene, through groups and nested scenes */
static bool walk_item(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	UNUSED_PARAMETER(scene);
	struct walk *walk = param;
	if (!obs_sceneitem_visible(item))
		return true;
	obs_source_t *source = obs_sceneitem_get_source(item);
	if (!source)
		return true;
	if (obs_sceneitem_is_group(item)) {
		obs_sceneitem_group_enum_items(item, walk_item, walk);
		return true;
	}
	obs_scene_t *nested = obs_scene_from_source(source);
	if (nested) {
		if (walk->depth < MAX_NESTING) {
			walk->depth++;
			obs_scene_enum_items(nested, walk_item, walk);
			walk->depth--;
		}
		return true;
	}
	if (!is_live_source(source))
		return true;
	const char *name = obs_source_get_name(source);
	if (!name || !*name)
		return true;
	for (size_t i = 0; i < walk->view->sources.num; i++) {
		if (strcmp(walk->view->sources.array[i], name) == 0)
			return true;
	}
	char *copy = bstrdup(name);
	da_push_back(walk->view->sources, &copy);
	return true;
}

static bool collect_scene(void *param, obs_source_t *source)
{
	struct collected *all = param;
	obs_scene_t *scene = obs_scene_from_source(source);
	const char *name = obs_source_get_name(source);
	if (!scene || !name || !*name)
		return true;
	struct scene_view *view = da_push_back_new(all->scenes);
	snprintf(view->name, sizeof(view->name), "%s", name);
	da_init(view->sources);
	struct walk walk = {view, 0};
	obs_scene_enum_items(scene, walk_item, &walk);
	return true;
}

/* the main canvas's program scene: channel 0, through its transition (a transition counts from its start) */
static void program_scene(char *out, size_t size)
{
	out[0] = '\0';
	obs_source_t *source = obs_get_output_source(0);
	if (source && obs_source_get_type(source) == OBS_SOURCE_TYPE_TRANSITION) {
		obs_source_t *active = obs_transition_get_active_source(source);
		obs_source_release(source);
		source = active;
	}
	if (source && obs_scene_from_source(source))
		snprintf(out, size, "%s", obs_source_get_name(source));
	obs_source_release(source);
}

bool failover_capture_picture(const char *source_name, uint8_t out[FO_GRID_BYTES])
{
	obs_source_t *source = obs_get_source_by_name(source_name ? source_name : "");
	const bool ok = source && is_live_source(source) && read_grid(source, out);
	obs_source_release(source);
	return ok;
}

/* ------------------------------------------------------------- switching -- */

struct switch_request {
	char to[FO_NAME_MAX];
};

/* UI thread: through the frontend, so it is the operator's own kind of cut (current transition; in Studio Mode
 * program moves and preview stays) */
static void switch_task(void *param)
{
	struct switch_request *request = param;
	if (!os_atomic_load_long(&fo.closing)) {
		obs_source_t *scene = obs_get_source_by_name(request->to);
		if (scene && obs_source_is_scene(scene))
			obs_frontend_set_current_scene(scene);
		obs_source_release(scene);
	}
	bfree(request);
}

static const char *title_key(const struct fo_decision *d)
{
	if (d->action == FO_RETURN)
		return "Failover.Event.Return";
	switch (d->reason) {
	case FO_BARS:
		return "Failover.Event.Bars";
	case FO_IMAGE_MATCH:
		return "Failover.Event.Picture";
	case FO_FROZEN:
		return "Failover.Event.Frozen";
	case FO_SIGNAL_LOST:
	case FO_OK:
		break;
	}
	return "Failover.Event.SignalLost";
}

/* the log, obs-websocket and CTD Relay hear about every switch (Relay API 1.11: obs.failover) */
static void announce(const struct fo_decision *d)
{
	struct dstr title = {0};
	dstr_printf(&title, obs_module_text(title_key(d)), d->to, d->source);
	blog(d->action == FO_RETURN ? LOG_INFO : LOG_WARNING, "[CTD] failover: %s (from \"%s\")", title.array, d->from);
	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "canvas", "landscape");
	obs_data_set_string(data, "from", d->from);
	obs_data_set_string(data, "to", d->to);
	obs_data_set_string(data, "source", d->source);
	obs_data_set_string(data, "reason", fo_reason_name(d->reason));
	obs_data_set_string(data, "mode", fo_decision_mode(d));
	obs_data_set_string(data, "title", title.array);
	ws_vendor_emit("FailoverTriggered", data);
	ms_post_event("obs.failover", data);
	obs_data_release(data);
	pthread_mutex_lock(&fo.lock);
	snprintf(fo.status.last, sizeof(fo.status.last), "%s", title.array);
	fo.status.last_at_ms = epoch_ms();
	pthread_mutex_unlock(&fo.lock);
	dstr_free(&title);
}

/* ---------------------------------------------------------------- thread -- */

static void check(void)
{
	const uint64_t now = now_ms();
	pthread_mutex_lock(&fo.lock);
	fo.tick_config = fo.config;
	pthread_mutex_unlock(&fo.lock);
	if (os_atomic_set_long(&fo.reset, 0)) {
		fo_state_init(&fo.state);
		fo_state_quiet(&fo.state, now, STARTUP_GRACE_MS);
	}

	struct collected all;
	da_init(all.samples);
	da_init(all.scenes);
	all.config = &fo.tick_config;
	obs_enum_sources(collect_source, &all);
	obs_enum_scenes(collect_scene, &all);
	char program[FO_NAME_MAX];
	program_scene(program, sizeof(program));

	DARRAY(struct fo_sample) samples;
	DARRAY(struct fo_scene) scenes;
	da_init(samples);
	da_init(scenes);
	for (size_t i = 0; i < all.samples.num; i++) {
		const struct sampled *s = &all.samples.array[i];
		const struct fo_sample sample = {s->name, s->has_signal, s->frozen_ms, s->has_grid ? s->grid : NULL};
		da_push_back(samples, &sample);
	}
	const struct scene_view *on_air = NULL;
	for (size_t i = 0; i < all.scenes.num; i++) {
		const struct scene_view *v = &all.scenes.array[i];
		const struct fo_scene scene = {v->name, (const char *const *)v->sources.array, v->sources.num};
		da_push_back(scenes, &scene);
		if (strcmp(v->name, program) == 0)
			on_air = v;
	}

	/* what is on program, for the issues the plugin reports and for the settings window */
	pthread_mutex_lock(&fo.lock);
	snprintf(fo.status.program, sizeof(fo.status.program), "%s", program);
	fo.on_program_count = 0;
	for (size_t i = 0; on_air && i < on_air->sources.num && fo.on_program_count < FO_MAX_SOURCES; i++)
		snprintf(fo.on_program[fo.on_program_count++], FO_NAME_MAX, "%s", on_air->sources.array[i]);
	pthread_mutex_unlock(&fo.lock);

	fo_observe(&fo.state, &fo.tick_config, samples.array, samples.num, now);
	const struct fo_world world = {program, scenes.array, scenes.num};
	struct fo_decision decision;
	fo_decide(&fo.state, &fo.tick_config, &world, now, &decision);
	if (decision.action != FO_NONE && !os_atomic_load_long(&fo.closing)) {
		struct switch_request *request = bzalloc(sizeof(*request));
		snprintf(request->to, sizeof(request->to), "%s", decision.to);
		obs_queue_task(OBS_TASK_UI, switch_task, request, false);
		fo_committed(&fo.state, &decision, now);
		announce(&decision);
	}

	da_free(samples);
	da_free(scenes);
	for (size_t i = 0; i < all.scenes.num; i++) {
		struct scene_view *v = &all.scenes.array[i];
		for (size_t k = 0; k < v->sources.num; k++)
			bfree(v->sources.array[k]);
		da_free(v->sources);
	}
	da_free(all.scenes);
	da_free(all.samples);
}

static void *failover_thread(void *unused)
{
	UNUSED_PARAMETER(unused);
	os_set_thread_name("ctd-failover");
	while (os_event_timedwait(fo.stop, CHECK_EVERY_MS) == ETIMEDOUT) {
		if (!os_atomic_load_long(&fo.closing))
			check();
	}
	return NULL;
}

static void frontend_event(enum obs_frontend_event event, void *unused)
{
	UNUSED_PARAMETER(unused);
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		os_atomic_set_long(&fo.closing, 0);
		os_atomic_set_long(&fo.reset, 1);
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
		os_atomic_set_long(&fo.closing, 1); /* scenes are going away: hands off until the new ones are in */
		break;
	case OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN:
	case OBS_FRONTEND_EVENT_EXIT:
		os_atomic_set_long(&fo.closing, 1);
		break;
	default:
		break;
	}
}

void failover_start(void)
{
	if (fo.running)
		return;
	if (!fo.lock_ready) {
		pthread_mutex_init(&fo.lock, NULL);
		fo.lock_ready = true;
	}
	fo_config_defaults(&fo.config);
	fo_state_init(&fo.state);
	os_atomic_set_long(&fo.reset, 1);
	os_atomic_set_long(&fo.closing, 0);
	if (os_event_init(&fo.stop, OS_EVENT_TYPE_MANUAL) != 0)
		return;
	obs_frontend_add_event_callback(frontend_event, NULL);
	obs_frontend_add_save_callback(save_cb, NULL);
	fo.running = pthread_create(&fo.thread, NULL, failover_thread, NULL) == 0;
	if (!fo.running) {
		obs_frontend_remove_save_callback(save_cb, NULL);
		obs_frontend_remove_event_callback(frontend_event, NULL);
		os_event_destroy(fo.stop);
	}
}

void failover_stop(void)
{
	if (!fo.running)
		return;
	os_atomic_set_long(&fo.closing, 1);
	os_event_signal(fo.stop);
	pthread_join(fo.thread, NULL);
	fo.running = false;
	obs_frontend_remove_save_callback(save_cb, NULL);
	obs_frontend_remove_event_callback(frontend_event, NULL);
	os_event_destroy(fo.stop);
	/* the lock stays: a late reader (an issue being reported while the module unloads) must not find it gone */
}
