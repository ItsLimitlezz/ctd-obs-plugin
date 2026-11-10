/* Show Profiles in the plugin: state, save/update/delete, lifetime and hooks (design sections 2, 4, 7).
 * The switch itself (guard, staged apply, verify, rollback) is in apply.c. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* gethostname, gmtime_r, recursive mutexes in util/threading.h under -std=c17 */
#endif
#include "show-internal.h"
#include "export.h"
#include "manifest.h"
#include "sha256.h"
#include "show-fs.h"
#include "store.h"
#include "device-map.h"
#include "sync.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>
#include <util/bmem.h>
#include <util/platform.h>
#include <util/threading.h>

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef _WIN32
#include <unistd.h>
#endif

#define PATH_LEN 2048
#define MAX_HOOKS 8

static struct {
	pthread_mutex_t mutex;
	bool mutex_ok;
	bool initialized;
	bool available;
	enum show_busy busy;
	char *message;
	bool message_is_error;
	char active_id[37];
	cj *entries;  /* store metas */
	cj *reentry;  /* {id: count} from the last switch to each */
	cj *warnings; /* strings */
	uint64_t seq;

	pthread_t worker;
	bool worker_running; /* a joinable worker exists */
	volatile bool worker_done;

	struct {
		show_stage_hook fn;
		void *opaque;
	} stage_hooks[MAX_HOOKS];
	size_t stage_hook_count;
	struct {
		show_applied_hook fn;
		void *opaque;
	} applied_hooks[MAX_HOOKS];
	size_t applied_hook_count;
} sp;

static void lock(void)
{
	if (sp.mutex_ok)
		pthread_mutex_lock(&sp.mutex);
}

static void unlock(void)
{
	if (sp.mutex_ok)
		pthread_mutex_unlock(&sp.mutex);
}

/* ------------------------------------------------------------ helpers --- */

void show_now_iso(char *out, size_t size)
{
	time_t now = time(NULL);
	struct tm tm;
#ifdef _WIN32
	gmtime_s(&tm, &now);
#else
	gmtime_r(&now, &tm);
#endif
	strftime(out, size, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

void show_machine_name(char *out, size_t size)
{
	char host[128] = "";
#ifdef _WIN32
	const char *env = getenv("COMPUTERNAME");
	if (env)
		snprintf(host, sizeof(host), "%s", env);
#else
	if (gethostname(host, sizeof(host) - 1) != 0)
		host[0] = '\0';
#endif
	size_t n = 0;
	for (const char *c = host; *c && n + 1 < size && n < 63; c++) {
		char lower = (char)tolower((unsigned char)*c);
		if (isalnum((unsigned char)lower))
			out[n++] = lower;
		else if (n && out[n - 1] != '-')
			out[n++] = '-';
	}
	while (n && out[n - 1] == '-')
		n--;
	if (n == 0 && size > 5) {
		snprintf(out, size, "local");
		return;
	}
	out[n] = '\0';
}

bool show_basic_dir(char *out, size_t size)
{
	char *profile = obs_frontend_get_current_profile_path(); /* .../basic/profiles/<dir> */
	if (!profile)
		return false;
	snprintf(out, size, "%s", profile);
	bfree(profile);
	for (int up = 0; up < 2; up++) {
		size_t n = strlen(out);
		while (n && (out[n - 1] == '/' || out[n - 1] == '\\'))
			out[--n] = '\0';
		char *slash = strrchr(out, '/');
		char *back = strrchr(out, '\\');
		if (back > slash)
			slash = back;
		if (!slash)
			return false;
		*slash = '\0';
	}
	return out[0] != '\0';
}

struct find_ctx {
	const char *dir, *name;
	char *out;
	size_t size;
	bool found;
};

/* the collection's top-level "name": parse only files that mention it, newest OBS writes it near the top or end */
static bool find_entry(void *opaque, const char *file, bool is_dir)
{
	struct find_ctx *f = opaque;
	size_t n = strlen(file);
	if (is_dir || n < 6 || strcmp(file + n - 5, ".json") != 0)
		return true;
	char path[PATH_LEN];
	if (!sfs_join(path, sizeof(path), f->dir, file))
		return true;
	cj *doc = cj_parse_file(path, NULL, 0);
	const char *name = cj_get_str(doc, "name");
	if (name && strcmp(name, f->name) == 0) {
		snprintf(f->out, f->size, "%s", path);
		f->found = true;
	}
	cj_free(doc);
	return !f->found;
}

bool show_find_collection_file(const char *basic_dir, const char *name, char *out, size_t size)
{
	char scenes[PATH_LEN];
	if (!sfs_join(scenes, sizeof(scenes), basic_dir, "scenes"))
		return false;
	struct find_ctx f = {scenes, name, out, size, false};
	sfs_list(scenes, find_entry, &f);
	return f.found;
}

static bool show_store_meta_exists(const char *id)
{
	if (!id || !show_is_uuid(id))
		return false;
	cj *meta = show_store_meta(id);
	bool exists = meta != NULL;
	cj_free(meta);
	return exists;
}

/* -------------------------------------------------------------- state --- */

static void bump(void)
{
	sp.seq++;
}

void show_set_busy(enum show_busy busy)
{
	lock();
	sp.busy = busy;
	bump();
	unlock();
}

void show_set_message(bool error, const char *fmt, ...)
{
	char text[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);
	lock();
	bfree(sp.message);
	sp.message = bstrdup(text);
	sp.message_is_error = error;
	bump();
	unlock();
}

void show_set_warnings(cj *warnings)
{
	lock();
	cj_free(sp.warnings);
	sp.warnings = warnings;
	bump();
	unlock();
}

void show_set_active(const char *id)
{
	lock();
	snprintf(sp.active_id, sizeof(sp.active_id), "%s", id && show_is_uuid(id) ? id : "");
	bump();
	unlock();
}

void show_set_reentry(const char *id, int count)
{
	lock();
	if (!sp.reentry)
		sp.reentry = cj_new_object();
	cj_set_int(sp.reentry, id, count);
	bump();
	unlock();
}

void show_reload_entries(void)
{
	cj *list = show_store_list();
	lock();
	cj_free(sp.entries);
	sp.entries = list;
	bump();
	unlock();
}

static char *dup_or_empty(const char *s)
{
	return bstrdup(s ? s : "");
}

void show_state_get(struct show_state *out)
{
	char reason[256];
	memset(out, 0, sizeof(*out));
	bool idle = show_guard_idle(reason, sizeof(reason));
	lock();
	out->available = sp.available;
	out->busy = sp.busy;
	out->message = dup_or_empty(sp.message);
	out->message_is_error = sp.message_is_error;
	out->active_id = dup_or_empty(sp.active_id);
	out->live = !idle;
	out->live_reason = dup_or_empty(idle ? "" : reason);
	out->warnings = sp.warnings ? cj_clone(sp.warnings) : cj_new_array();
	out->seq = sp.seq;
	size_t n = cj_size(sp.entries);
	out->entries = n ? bzalloc(n * sizeof(*out->entries)) : NULL;
	for (size_t i = 0; i < n; i++) {
		const cj *meta = cj_at(sp.entries, i);
		struct show_entry *e = &out->entries[i];
		snprintf(e->id, sizeof(e->id), "%s", cj_get_str(meta, "id"));
		e->name = dup_or_empty(cj_get_str(meta, "name"));
		e->head_seq = cj_get_int(meta, "headSeq", 0);
		e->revision = cj_get_int(meta, "baseRevision", 0);
		e->dirty = cj_get_bool(meta, "dirty", false);
		e->active = strcmp(e->id, sp.active_id) == 0;
		e->saved_at = dup_or_empty(cj_get_str(meta, "savedAt"));
		e->last_applied_at = dup_or_empty(cj_get_str(meta, "lastAppliedAt"));
		e->relay_profile_id = dup_or_empty(cj_get_str(meta, "relayProfileId"));
		e->needs_reentry = (int)cj_get_int(sp.reentry, e->id, 0);
	}
	out->count = n;
	unlock();
	/* the guard can change without anything else changing: fold it into seq so a dock that only redraws on seq
	 * still sees it */
	out->seq = out->seq * 2 + (out->live ? 1 : 0);
}

void show_state_free(struct show_state *s)
{
	if (!s)
		return;
	bfree(s->message);
	bfree(s->active_id);
	bfree(s->live_reason);
	for (size_t i = 0; i < s->count; i++) {
		bfree(s->entries[i].name);
		bfree(s->entries[i].saved_at);
		bfree(s->entries[i].last_applied_at);
		bfree(s->entries[i].relay_profile_id);
	}
	bfree(s->entries);
	cj_free(s->warnings);
	memset(s, 0, sizeof(*s));
}

/* ------------------------------------------------------------- worker --- */

static void join_finished_worker(void)
{
	if (sp.worker_running && sp.worker_done) {
		pthread_join(sp.worker, NULL);
		sp.worker_running = false;
	}
}

struct worker_start {
	void *(*fn)(void *);
	void *param;
};

static void *worker_main(void *opaque)
{
	struct worker_start start = *(struct worker_start *)opaque;
	bfree(opaque);
	start.fn(start.param);
	sp.worker_done = true;
	return NULL;
}

/* UI thread */
bool show_run_worker(void *(*fn)(void *), void *param)
{
	join_finished_worker();
	if (sp.worker_running)
		return false;
	struct worker_start *start = bzalloc(sizeof(*start));
	start->fn = fn;
	start->param = param;
	sp.worker_done = false;
	if (pthread_create(&sp.worker, NULL, worker_main, start) != 0) {
		bfree(start);
		return false;
	}
	sp.worker_running = true;
	return true;
}

/* -------------------------------------------------------------- hooks --- */

void show_add_stage_hook(show_stage_hook hook, void *opaque)
{
	lock();
	if (sp.stage_hook_count < MAX_HOOKS) {
		sp.stage_hooks[sp.stage_hook_count].fn = hook;
		sp.stage_hooks[sp.stage_hook_count].opaque = opaque;
		sp.stage_hook_count++;
	}
	unlock();
}

void show_add_applied_hook(show_applied_hook hook, void *opaque)
{
	lock();
	if (sp.applied_hook_count < MAX_HOOKS) {
		sp.applied_hooks[sp.applied_hook_count].fn = hook;
		sp.applied_hooks[sp.applied_hook_count].opaque = opaque;
		sp.applied_hook_count++;
	}
	unlock();
}

bool show_run_stage_hooks(struct show_stage_ctx *ctx, char *err, size_t err_size)
{
	for (size_t i = 0; i < sp.stage_hook_count; i++)
		if (!sp.stage_hooks[i].fn(sp.stage_hooks[i].opaque, ctx, err, err_size))
			return false;
	return true;
}

void show_run_applied_hooks(const char *id, int64_t seq, const cj *manifest)
{
	for (size_t i = 0; i < sp.applied_hook_count; i++)
		sp.applied_hooks[i].fn(sp.applied_hooks[i].opaque, id, seq, manifest);
}

/* --------------------------------------------------------------- save --- */

struct save_job {
	char id[37];
	char name[SHOW_NAME_MAX + 1];
	bool created; /* a new show profile: delete it again if the save fails */
	char collection_name[256];
	char profile_dir[PATH_LEN];
	char basic[PATH_LEN];
};

static const char *os_name(void)
{
#if defined(_WIN32)
	return "windows";
#elif defined(__APPLE__)
	return "macos";
#else
	return "linux";
#endif
}

/* Files already in the asset cache are named by their sha256: no need to hash a 2 GB video again on every save */
static bool cached_hash(void *opaque, const char *path, char sha256[65], int64_t *size)
{
	(void)opaque;
	char assets[PATH_LEN];
	const char *root = show_store_root();
	size_t n = 0;
	if (root[0] && sfs_join(assets, sizeof(assets), root, "assets"))
		n = strlen(assets);
	if (n && strncmp(path, assets, n) == 0 && (path[n] == '/' || path[n] == '\\')) {
		const char *file = path + strlen(path);
		while (file > path && file[-1] != '/' && file[-1] != '\\')
			file--;
		char sha[65];
		snprintf(sha, sizeof(sha), "%.64s", file);
		int64_t have = sfs_size(path);
		if (show_is_sha256(sha) && have >= 0 && (file[64] == '\0' || file[64] == '.')) {
			memcpy(sha256, sha, 65);
			if (size)
				*size = have;
			return true;
		}
	}
	return sha256_file(path, sha256, size, NULL, NULL);
}

static void *save_thread(void *param)
{
	struct save_job *job = param;
	char err[512] = "", collection_path[PATH_LEN], now[32], machine[64], app_id[96];
	struct show_bundle b;
	cj *warnings = cj_new_array();
	cj *previous = NULL;
	bool ok = false;
	int64_t seq = 0, before = 0;
	show_bundle_init(&b);
	os_set_thread_name("ctd-show-save");

	if (!show_find_collection_file(job->basic, job->collection_name, collection_path, sizeof(collection_path))) {
		snprintf(err, sizeof(err), "Can't find the file of the scene collection \"%s\".", job->collection_name);
		goto done;
	}
	if (job->created && !show_store_create(job->id, job->name, err, sizeof(err))) {
		job->created = false;
		goto done;
	}
	cj *meta = show_store_meta(job->id);
	before = cj_get_int(meta, "headSeq", 0);
	cj_free(meta);
	if (before > 0) {
		struct show_bundle head;
		if (show_store_load(job->id, before, &head, NULL, 0)) {
			previous = cj_clone(head.manifest);
			show_bundle_free(&head);
		}
	}

	show_now_iso(now, sizeof(now));
	show_machine_name(machine, sizeof(machine));
	snprintf(app_id, sizeof(app_id), "ctd-obs-plugin-%s", machine);
	struct show_export_input in = {
		.id = job->id,
		.name = job->name,
		.collection_path = collection_path,
		.profile_dir = job->profile_dir,
		.app_id = app_id,
		.os = os_name(),
		.obs_version = obs_get_version_string(),
		.ctd_obs_version = "",
		.plugin_version = PLUGIN_VERSION,
		.saved_at = now,
		.previous_manifest = previous,
		.hash = cached_hash,
	};
	if (!show_export(&in, &b, warnings, err, sizeof(err)))
		goto done;
	if (!show_store_save(job->id, &b, SHOW_SAVE_LOCAL, &seq, err, sizeof(err)))
		goto done;
	ok = true;
done:
	show_bundle_free(&b);
	cj_free(previous);
	if (!ok && job->created)
		show_store_delete(job->id);
	if (ok && seq == before)
		show_set_message(false, "No changes since the last save of %s.", job->name);
	else if (ok)
		show_set_message(false, "Saved %s on this machine.", job->name);
	else
		show_set_message(true, "Couldn't save %s: %s", job->name, err[0] ? err : "unknown error");
	if (!ok)
		blog(LOG_WARNING, SHOW_LOG_PREFIX "save failed: %s", err);
	show_set_warnings(warnings);
	show_reload_entries();
	show_set_busy(SHOW_IDLE);
	bfree(job);
	return NULL;
}

/* UI thread: OBS writes the collection file after its event loop runs (obs_frontend_save defers), so the worker
 * starts from a queued task, after that write */
static void save_start_ui(void *param)
{
	struct save_job *job = param;
	if (!show_run_worker(save_thread, job)) {
		show_set_message(true, "Busy: wait for the current Show Profiles action to finish.");
		show_set_busy(SHOW_IDLE);
		bfree(job);
	}
}

static bool begin(enum show_busy busy)
{
	if (!sp.available) {
		show_set_message(true, "Show Profiles needs CTD OBS.");
		return false;
	}
	join_finished_worker();
	lock();
	bool free_now = sp.busy == SHOW_IDLE && !sp.worker_running;
	if (free_now) {
		sp.busy = busy;
		bump();
	}
	unlock();
	if (!free_now)
		show_set_message(true, "Busy: wait for the current Show Profiles action to finish.");
	return free_now;
}

bool show_begin_job(enum show_busy busy, void *(*fn)(void *), void *param)
{
	if (!begin(busy))
		return false;
	if (!show_run_worker(fn, param)) {
		show_set_message(true, "Busy: wait for the current Show Profiles action to finish.");
		show_set_busy(SHOW_IDLE);
		return false;
	}
	return true;
}

void show_join_worker(void)
{
	if (sp.worker_running) {
		pthread_join(sp.worker, NULL);
		sp.worker_running = false;
	}
}

const char *show_active_id_copy(char out[37])
{
	lock();
	snprintf(out, 37, "%s", sp.active_id);
	unlock();
	return out;
}

static bool start_save(const char *id, const char *name, bool create)
{
	if (!begin(SHOW_SAVING))
		return false;
	struct save_job *job = bzalloc(sizeof(*job));
	snprintf(job->id, sizeof(job->id), "%s", id);
	snprintf(job->name, sizeof(job->name), "%s", name);
	job->created = create;
	char *collection = obs_frontend_get_current_scene_collection();
	char *profile = obs_frontend_get_current_profile_path();
	bool ok = collection && profile && show_basic_dir(job->basic, sizeof(job->basic));
	snprintf(job->collection_name, sizeof(job->collection_name), "%s", collection ? collection : "");
	snprintf(job->profile_dir, sizeof(job->profile_dir), "%s", profile ? profile : "");
	bfree(collection);
	bfree(profile);
	if (!ok) {
		bfree(job);
		show_set_message(true, "Can't find OBS's current scene collection and profile.");
		show_set_busy(SHOW_IDLE);
		return false;
	}
	show_set_message(false, "Saving %s…", name);
	obs_frontend_save();
	obs_queue_task(OBS_TASK_UI, save_start_ui, job, false);
	return true;
}

bool show_save_current_as(const char *name)
{
	char other[37];
	if (!show_name_is_valid(name)) {
		show_set_message(true, "\"%s\" can't be a show profile name.", name ? name : "");
		return false;
	}
	if (show_store_find_by_name(name, other)) {
		show_set_message(true, "A show profile named \"%s\" already exists: pick it and use Update.", name);
		return false;
	}
	char *uuid = os_generate_uuid();
	bool ok = start_save(uuid, name, true);
	bfree(uuid);
	return ok;
}

bool show_update(const char *id)
{
	cj *meta = show_store_meta(id);
	char name[SHOW_NAME_MAX + 1];
	snprintf(name, sizeof(name), "%s", meta ? cj_get_str(meta, "name") : "");
	cj_free(meta);
	if (!name[0]) {
		show_set_message(true, "That show profile is gone.");
		return false;
	}
	lock();
	bool active = strcmp(sp.active_id, id) == 0;
	unlock();
	if (!active) {
		show_set_message(true, "Switch to %s first: Update saves the setup that is loaded now.", name);
		return false;
	}
	return start_save(id, name, false);
}

/* ------------------------------------------------------------- switch --- */

bool show_switch(const char *id, int64_t seq)
{
	char reason[256];
	if (!show_store_meta_exists(id)) {
		show_set_message(true, "That show profile is gone.");
		return false;
	}
	if (!show_guard_idle(reason, sizeof(reason))) {
		show_set_message(true, "Switching is off while live: %s", reason);
		return false;
	}
	if (!begin(SHOW_SWITCHING))
		return false;
	show_set_warnings(NULL);
	show_apply_start(id, seq);
	return true;
}

/* ------------------------------------------------------------- delete --- */

static void *delete_thread(void *param)
{
	char *id = param;
	os_set_thread_name("ctd-show-delete");
	cj *meta = show_store_meta(id);
	char name[SHOW_NAME_MAX + 1];
	snprintf(name, sizeof(name), "%s", meta ? cj_get_str(meta, "name") : id);
	cj_free(meta);
	if (show_store_delete(id))
		show_set_message(false, "Deleted %s from this machine.", name);
	else
		show_set_message(true, "Couldn't delete all of %s's files.", name);
	show_reload_entries();
	show_set_busy(SHOW_IDLE);
	bfree(id);
	return NULL;
}

bool show_delete(const char *id)
{
	lock();
	bool active = strcmp(sp.active_id, id) == 0;
	unlock();
	if (active) {
		show_set_message(true, "That show profile is loaded now: switch to another one first.");
		return false;
	}
	if (!begin(SHOW_DELETING))
		return false;
	if (!show_run_worker(delete_thread, bstrdup(id))) {
		show_set_busy(SHOW_IDLE);
		return false;
	}
	return true;
}

/* ----------------------------------------------------------- lifetime --- */

/* modules.ctd-show-profile in the collection: which show profile it belongs to (design section 2) */
static void save_cb(obs_data_t *save_data, bool saving, void *unused)
{
	(void)unused;
	if (saving) {
		lock();
		char id[37];
		snprintf(id, sizeof(id), "%s", sp.active_id);
		unlock();
		char *collection = obs_frontend_get_current_scene_collection();
		bool ours = collection && strncmp(collection, SHOW_OBS_NAME_PREFIX, strlen(SHOW_OBS_NAME_PREFIX)) == 0;
		bfree(collection);
		if (id[0] && ours) {
			obs_data_t *tie = obs_data_create();
			obs_data_set_string(tie, "id", id);
			cj *meta = show_store_meta(id);
			obs_data_set_int(tie, "seq", cj_get_int(meta, "appliedSeq", 0));
			cj_free(meta);
			obs_data_set_obj(save_data, SHOW_MODULE_KEY, tie);
			obs_data_release(tie);
		}
		return;
	}
	obs_data_t *tie = obs_data_get_obj(save_data, SHOW_MODULE_KEY);
	const char *id = tie ? obs_data_get_string(tie, "id") : "";
	show_set_active(show_store_meta_exists(id) ? id : "");
	obs_data_release(tie);
}

static void event_cb(enum obs_frontend_event event, void *unused)
{
	(void)unused;
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		bool available = show_core_available();
		lock();
		sp.available = available;
		bump();
		unlock();
		blog(LOG_INFO, SHOW_LOG_PREFIX "%s",
		     available ? "CTD OBS core found: Show Profiles on" : "no CTD OBS core procs: Show Profiles off");
	}
}

void show_init(void)
{
	if (sp.initialized)
		return;
	sp.mutex_ok = pthread_mutex_init(&sp.mutex, NULL) == 0;
	char *root = obs_module_config_path("");
	if (root) {
		size_t n = strlen(root);
		while (n && (root[n - 1] == '/' || root[n - 1] == '\\'))
			root[--n] = '\0';
		if (!show_store_open(root))
			blog(LOG_WARNING, SHOW_LOG_PREFIX "can't open the show profile store");
		bfree(root);
	}
	show_reload_entries();
	obs_frontend_add_event_callback(event_cb, NULL);
	obs_frontend_add_save_callback(save_cb, NULL);
	show_devices_init(); /* M4: stage + applied hooks */
	show_sync_init();    /* M3: Relay sync */
	sp.initialized = true;
}

void show_shutdown(void)
{
	if (!sp.initialized)
		return;
	show_sync_shutdown(); /* stops transfers, so a running job ends quickly */
	obs_frontend_remove_save_callback(save_cb, NULL);
	obs_frontend_remove_event_callback(event_cb, NULL);
	if (sp.worker_running) {
		pthread_join(sp.worker, NULL);
		sp.worker_running = false;
	}
	show_apply_shutdown();
	show_devices_shutdown();
	show_store_close();
	lock();
	bfree(sp.message);
	sp.message = NULL;
	cj_free(sp.entries);
	sp.entries = NULL;
	cj_free(sp.reentry);
	sp.reentry = NULL;
	cj_free(sp.warnings);
	sp.warnings = NULL;
	unlock();
	if (sp.mutex_ok)
		pthread_mutex_destroy(&sp.mutex);
	sp.mutex_ok = false;
	sp.initialized = false;
}
