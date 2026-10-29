/*
 * Sync groups in OBS. See sync-groups.h.
 */

#include "sync-groups.h"
#include "ws-vendor.h"
#include "../multistream.h"

#include <obs-module.h>
#include <util/bmem.h>
#include <util/darray.h>
#include <util/platform.h>
#include <util/threading.h>

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define TICK_MS 100
#define MAX_GROUPS 16

struct running_group {
	char name[64];
	struct sync_group g;
	char members[SYNC_MAX_MEMBERS][256]; /* source names, by slot */
	enum sync_state last_state[SYNC_MAX_MEMBERS];
	bool announced[SYNC_MAX_MEMBERS];
	char timecode[SYNC_MAX_MEMBERS][16];
};

/* one source this round */
struct gathered {
	obs_source_t *source; /* a reference, released at the end of the round */
	char name[256];
	struct ctd_sync_info info;
};

static struct {
	pthread_t thread;
	os_event_t *stop;
	bool running;
	pthread_mutex_t mutex; /* groups (the thread writes, the snapshot and configure read) and settings */
	struct running_group groups[MAX_GROUPS];
	int group_count;
	obs_data_t *settings; /* sync-groups.json: {"groups": {name: {mode, maxDelayMs, fixedLatencyMs}}} */
} manager;

/* ---------------------------------------------------------------- storage -- */

static char *config_file(void)
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	return obs_module_config_path("sync-groups.json");
}

/* call with the mutex held: a group's settings into its controller */
static void apply_settings(struct running_group *r)
{
	obs_data_t *groups = obs_data_get_obj(manager.settings, "groups");
	obs_data_t *mine = groups ? obs_data_get_obj(groups, r->name) : NULL;
	r->g.mode = SYNC_ALIGN_TO_SLOWEST;
	r->g.max_delay_ms = 1000;
	r->g.fixed_latency_ms = 0;
	if (mine) {
		r->g.mode = strcmp(obs_data_get_string(mine, "mode"), "fixed") == 0 ? SYNC_FIXED_LATENCY
										    : SYNC_ALIGN_TO_SLOWEST;
		const long long max = obs_data_get_int(mine, "maxDelayMs");
		if (max > 0)
			r->g.max_delay_ms = max > 5000 ? 5000 : (int)max;
		r->g.fixed_latency_ms = (int)obs_data_get_int(mine, "fixedLatencyMs");
	}
	obs_data_release(mine);
	obs_data_release(groups);
}

bool sync_groups_configure(const char *name, int mode, int max_delay_ms, int fixed_latency_ms)
{
	if (!name || !*name || strlen(name) >= 64)
		return false;
	pthread_mutex_lock(&manager.mutex);
	obs_data_t *groups = obs_data_get_obj(manager.settings, "groups");
	if (!groups) {
		groups = obs_data_create();
		obs_data_set_obj(manager.settings, "groups", groups);
	}
	obs_data_t *mine = obs_data_get_obj(groups, name);
	if (!mine) {
		mine = obs_data_create();
		obs_data_set_string(mine, "mode", "align");
		obs_data_set_int(mine, "maxDelayMs", 1000);
		obs_data_set_obj(groups, name, mine);
	}
	if (mode >= 0)
		obs_data_set_string(mine, "mode", mode == SYNC_FIXED_LATENCY ? "fixed" : "align");
	if (max_delay_ms >= 0)
		obs_data_set_int(mine, "maxDelayMs", max_delay_ms < 1 ? 1 : max_delay_ms > 5000 ? 5000 : max_delay_ms);
	if (fixed_latency_ms >= 0)
		obs_data_set_int(mine, "fixedLatencyMs", fixed_latency_ms);
	obs_data_release(mine);
	obs_data_release(groups);
	char *file = config_file();
	if (file && !obs_data_save_json_safe(manager.settings, file, "tmp", "bak"))
		blog(LOG_WARNING, "[ctd-live] couldn't save the sync group settings");
	bfree(file);
	for (int i = 0; i < manager.group_count; i++)
		if (strcmp(manager.groups[i].name, name) == 0)
			apply_settings(&manager.groups[i]);
	pthread_mutex_unlock(&manager.mutex);
	blog(LOG_INFO, "[ctd-live] sync group \"%s\" settings changed", name);
	return true;
}

/* ------------------------------------------------------------------ round -- */

static bool is_ctd_source(obs_source_t *source)
{
	const char *id = obs_source_get_unversioned_id(source);
	return id && (strcmp(id, "ctd_live_source") == 0 || strcmp(id, "ctd_live_audio_source") == 0);
}

static bool gather(void *param, obs_source_t *source)
{
	if (!is_ctd_source(source))
		return true;
	struct gathered g = {0};
	calldata_t cd = {0};
	calldata_set_ptr(&cd, "info", &g.info);
	const bool ok = proc_handler_call(obs_source_get_proc_handler(source), "get_sync", &cd);
	calldata_free(&cd);
	if (!ok)
		return true;
	g.source = obs_source_get_ref(source);
	if (!g.source)
		return true;
	snprintf(g.name, sizeof(g.name), "%s", obs_source_get_name(source));
	DARRAY(struct gathered) *all = param;
	da_push_back(*all, &g);
	return true;
}

static void tell_source(obs_source_t *source, int delay_ms, bool apply, bool jump, int state, double offset_ms,
			bool reference)
{
	calldata_t cd = {0};
	calldata_set_int(&cd, "delay_ms", delay_ms);
	calldata_set_bool(&cd, "apply", apply);
	calldata_set_bool(&cd, "jump", jump);
	calldata_set_int(&cd, "state", state);
	calldata_set_float(&cd, "offset_ms", offset_ms);
	calldata_set_bool(&cd, "reference", reference);
	proc_handler_call(obs_source_get_proc_handler(source), "set_sync", &cd);
	calldata_free(&cd);
}

/* SyncStatusChanged (I3), to obs-websocket and CTD Relay */
static void announce(const char *group, const char *source, enum sync_state state, enum sync_state previous,
		     double offset_ms, double delay_ms)
{
	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "group", group);
	obs_data_set_string(data, "sourceName", source);
	obs_data_set_string(data, "state", sync_state_name(state));
	obs_data_set_string(data, "previous", sync_state_name(previous));
	obs_data_set_bool(data, "lost", sync_state_lost(state));
	obs_data_set_double(data, "offsetMs", round(offset_ms * 10) / 10);
	obs_data_set_int(data, "delayMs", (long long)lround(delay_ms));
	ws_vendor_emit("SyncStatusChanged", data);
	ms_post_event("obs.sync.status", data);
	obs_data_release(data);
	blog(sync_state_lost(state) ? LOG_WARNING : LOG_INFO,
	     "[ctd-live] sync group \"%s\": %s is %s (offset %.0f ms, delay %.0f ms)", group, source,
	     sync_state_name(state), offset_ms, delay_ms);
}

/* the local time of day, ns since midnight (fixed latency mode) */
static int64_t time_of_day_ns(void)
{
	struct timespec ts;
	timespec_get(&ts, TIME_UTC);
	struct tm local;
#ifdef _WIN32
	localtime_s(&local, &ts.tv_sec);
#else
	localtime_r(&ts.tv_sec, &local);
#endif
	return ((int64_t)local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec) * 1000000000ll + ts.tv_nsec;
}

static struct running_group *find_group(const char *name, bool create)
{
	for (int i = 0; i < manager.group_count; i++)
		if (strcmp(manager.groups[i].name, name) == 0)
			return &manager.groups[i];
	if (!create || manager.group_count >= MAX_GROUPS)
		return NULL;
	struct running_group *r = &manager.groups[manager.group_count++];
	memset(r, 0, sizeof(*r));
	snprintf(r->name, sizeof(r->name), "%s", name);
	sync_group_init(&r->g);
	apply_settings(r);
	return r;
}

static void tick(void)
{
	DARRAY(struct gathered) all;
	da_init(all);
	obs_enum_sources(gather, &all);
	const uint64_t now = os_gettime_ns();
	const int64_t tod = time_of_day_ns();

	/* what to tell each source, worked out under the lock and sent after it */
	struct order {
		obs_source_t *source;
		int delay_ms;
		bool apply, jump;
		int state;
		double offset_ms;
		bool reference;
	};
	DARRAY(struct order) orders;
	da_init(orders);
	struct event {
		char group[64];
		char source[256];
		enum sync_state state, previous;
		double offset_ms, delay_ms;
	};
	DARRAY(struct event) events;
	da_init(events);

	pthread_mutex_lock(&manager.mutex);
	bool seen[MAX_GROUPS][SYNC_MAX_MEMBERS] = {{false}};
	struct sync_input inputs[MAX_GROUPS][SYNC_MAX_MEMBERS];
	memset(inputs, 0, sizeof(inputs));
	obs_source_t *sources[MAX_GROUPS][SYNC_MAX_MEMBERS] = {{NULL}};
	for (size_t i = 0; i < all.num; i++) {
		struct gathered *s = &all.array[i];
		if (!*s->info.group) {
			/* in no group: drop a group delay it still holds */
			if (s->info.delay_ms)
				da_push_back(orders, (&(struct order){s->source, 0, true, false, -1, 0, false}));
			continue;
		}
		struct running_group *r = find_group(s->info.group, true);
		if (!r)
			continue;
		const int gi = (int)(r - manager.groups);
		int slot = -1;
		for (int m = 0; m < SYNC_MAX_MEMBERS && slot < 0; m++)
			if (r->g.members[m].used && strcmp(r->members[m], s->name) == 0)
				slot = m;
		for (int m = 0; m < SYNC_MAX_MEMBERS && slot < 0; m++) {
			if (!r->g.members[m].used) {
				slot = m;
				/* S13: a member that comes back keeps its delay as the starting guess */
				sync_member_join(&r->g, m, s->info.delay_ms);
				snprintf(r->members[m], sizeof(r->members[m]), "%s", s->name);
				r->announced[m] = false;
				blog(LOG_INFO, "[ctd-live] sync group \"%s\": %s joined", r->name, s->name);
			}
		}
		if (slot < 0)
			continue; /* a full group */
		seen[gi][slot] = true;
		sources[gi][slot] = s->source;
		snprintf(r->timecode[slot], sizeof(r->timecode[slot]), "%s", s->info.timecode_text);
		struct sync_input *in = &inputs[gi][slot];
		in->timecode = s->info.engine && s->info.timecode;
		in->pinned = s->info.pinned;
		in->tc_seconds = s->info.tc_seconds;
		in->pin_ns = s->info.pin_ns;
		in->fps = s->info.fps;
		in->settled = s->info.settled;
	}
	for (int gi = 0; gi < manager.group_count; gi++) {
		struct running_group *r = &manager.groups[gi];
		/* members that left (renamed, removed, or moved to another group) */
		for (int m = 0; m < SYNC_MAX_MEMBERS; m++) {
			if (r->g.members[m].used && !seen[gi][m]) {
				blog(LOG_INFO, "[ctd-live] sync group \"%s\": %s left", r->name, r->members[m]);
				r->g.members[m].used = false;
			}
		}
		struct sync_output out[SYNC_MAX_MEMBERS];
		sync_group_update(&r->g, inputs[gi], now, tod, out);
		for (int m = 0; m < SYNC_MAX_MEMBERS; m++) {
			const struct sync_member *member = &r->g.members[m];
			if (!member->used)
				continue;
			da_push_back(orders, (&(struct order){sources[gi][m], (int)lround(out[m].delay_ms),
							      out[m].changed, out[m].jump, (int)member->state,
							      member->offset_ms, member->reference}));
			if (!r->announced[m] || member->state != r->last_state[m]) {
				struct event ev = {0};
				snprintf(ev.group, sizeof(ev.group), "%s", r->name);
				snprintf(ev.source, sizeof(ev.source), "%s", r->members[m]);
				ev.state = member->state;
				ev.previous = r->announced[m] ? r->last_state[m] : SYNC_NO_TIMECODE;
				ev.offset_ms = member->offset_ms;
				ev.delay_ms = member->delay_ms;
				da_push_back(events, &ev);
				r->last_state[m] = member->state;
				r->announced[m] = true;
			}
		}
	}
	/* groups with nobody left */
	for (int gi = manager.group_count - 1; gi >= 0; gi--) {
		bool any = false;
		for (int m = 0; m < SYNC_MAX_MEMBERS; m++)
			any |= manager.groups[gi].g.members[m].used;
		if (!any) {
			memmove(&manager.groups[gi], &manager.groups[gi + 1],
				sizeof(manager.groups[0]) * (size_t)(manager.group_count - gi - 1));
			manager.group_count--;
		}
	}
	pthread_mutex_unlock(&manager.mutex);

	for (size_t i = 0; i < orders.num; i++) {
		const struct order *o = &orders.array[i];
		tell_source(o->source, o->delay_ms, o->apply, o->jump, o->state, o->offset_ms, o->reference);
	}
	for (size_t i = 0; i < events.num; i++) {
		const struct event *ev = &events.array[i];
		announce(ev->group, ev->source, ev->state, ev->previous, ev->offset_ms, ev->delay_ms);
	}
	da_free(orders);
	da_free(events);
	for (size_t i = 0; i < all.num; i++)
		obs_source_release(all.array[i].source);
	da_free(all);
}

static void *sync_thread(void *unused)
{
	UNUSED_PARAMETER(unused);
	os_set_thread_name("ctd-sync");
	while (os_event_timedwait(manager.stop, TICK_MS) == ETIMEDOUT)
		tick();
	return NULL;
}

/* ------------------------------------------------------------------ API -- */

void sync_groups_start(void)
{
	if (manager.running)
		return;
	pthread_mutex_init(&manager.mutex, NULL);
	char *file = config_file();
	manager.settings = file ? obs_data_create_from_json_file_safe(file, "bak") : NULL;
	bfree(file);
	if (!manager.settings)
		manager.settings = obs_data_create();
	if (os_event_init(&manager.stop, OS_EVENT_TYPE_MANUAL) != 0)
		return;
	manager.running = pthread_create(&manager.thread, NULL, sync_thread, NULL) == 0;
}

void sync_groups_stop(void)
{
	if (!manager.running)
		return;
	os_event_signal(manager.stop);
	pthread_join(manager.thread, NULL);
	os_event_destroy(manager.stop);
	manager.running = false;
	obs_data_release(manager.settings);
	manager.settings = NULL;
	manager.group_count = 0;
	pthread_mutex_destroy(&manager.mutex);
}

size_t sync_groups_snapshot(struct ctd_sync_group_view *out, size_t max)
{
	if (!manager.running)
		return 0;
	size_t n = 0;
	pthread_mutex_lock(&manager.mutex);
	for (int gi = 0; gi < manager.group_count && n < max; gi++) {
		const struct running_group *r = &manager.groups[gi];
		struct ctd_sync_group_view *v = &out[n++];
		memset(v, 0, sizeof(*v));
		snprintf(v->name, sizeof(v->name), "%s", r->name);
		v->mode = r->g.mode;
		v->max_delay_ms = r->g.max_delay_ms;
		v->fixed_latency_ms = r->g.fixed_latency_ms;
		for (int m = 0; m < SYNC_MAX_MEMBERS; m++) {
			const struct sync_member *member = &r->g.members[m];
			if (!member->used)
				continue;
			struct ctd_sync_member_view *mv = &v->members[v->count++];
			snprintf(mv->source, sizeof(mv->source), "%s", r->members[m]);
			mv->state = member->state;
			snprintf(mv->timecode, sizeof(mv->timecode), "%s", r->timecode[m]);
			mv->offset_ms = member->offset_ms;
			mv->delay_ms = member->delay_ms;
			mv->reference = member->reference;
		}
	}
	pthread_mutex_unlock(&manager.mutex);
	return n;
}
