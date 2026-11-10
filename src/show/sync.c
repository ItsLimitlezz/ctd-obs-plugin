/* Show Profiles sync with CTD Relay: HTTP operations, jobs on the M2 worker, automatic pulls and retries, dock
 * state. See sync.h; the flows themselves are in sync-flow.c (tested without OBS). */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* recursive mutexes in util/threading.h under -std=c17 */
#endif
#include "sync.h"
#include "show-fs.h"
#include "show-internal.h"
#include "store.h"
#include "sync-flow.h"
#include "../http-client.h"
#include "../multistream.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/bmem.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define TICK_MS 2000
#define PULL_BACKOFF_S 60
#define APP_ID_PREFIX "ctd-obs-plugin-"
#define PATH_LEN 2048

enum job_kind { JOB_SAVE_UP, JOB_PULL, JOB_RESOLVE };

struct job {
	enum job_kind kind;
	char id[37];    /* save up, resolve: the local show profile */
	char relay[72]; /* pull: the Relay profile; save up: where an unlinked profile goes */
	char name[96];  /* for messages */
	char *base;     /* API base, ".../api/v1" */
	char *token;    /* never logged */
	bool automatic; /* a retry or an automatic pull: quieter messages */
	bool switch_after;
	enum show_conflict_choice choice;
	int64_t head;
	char scratch[PATH_LEN];
};

static struct {
	pthread_mutex_t mutex;
	bool mutex_ok;
	bool initialized;
	volatile bool stopping;
	volatile bool tick_queued;
	pthread_t thread;
	bool thread_running;
	os_event_t *wake;

	/* guarded by mutex */
	bool connected;
	char *relay_profile_id;
	char *relay_profile_name;
	char *step;
	uint64_t done, total;
	struct show_sync_entry *entries;
	size_t count;
	struct show_sync_remote *remotes;
	size_t remote_count;
	bool conflict;
	char conflict_id[37];
	char *conflict_name;
	int64_t conflict_head;
	char *conflict_by;
	uint64_t conflict_seq;
	uint64_t seq;

	/* UI thread only */
	bool job_running;
	cj *update_ready;        /* {id: true}: pulled while active, not switched to yet */
	cj *pull_backoff;        /* {relay profile: epoch s} after a failed automatic pull */
	char pending_upload[37]; /* Save to Relay of the active profile: the local Update runs first */
} sy;

static void lock(void)
{
	if (sy.mutex_ok)
		pthread_mutex_lock(&sy.mutex);
}

static void unlock(void)
{
	if (sy.mutex_ok)
		pthread_mutex_unlock(&sy.mutex);
}

static void set_str(char **field, const char *value)
{
	bfree(*field);
	*field = bstrdup(value ? value : "");
}

static int64_t now_s(void)
{
	return (int64_t)time(NULL);
}

static bool safe_relay_id(const char *id)
{
	if (!id || !*id || strlen(id) > 64)
		return false;
	for (const char *c = id; *c; c++)
		if (!isalnum((unsigned char)*c) && *c != '-' && *c != '_')
			return false;
	return true;
}

/* -------------------------------------------------------- Relay ops --- */

static int interrupt_cb(void *opaque)
{
	(void)opaque;
	return sy.stopping ? 1 : 0;
}

static enum show_relay_result map_status(enum http_status s)
{
	switch (s) {
	case HTTP_OK:
		return SHOW_RELAY_OK;
	case HTTP_UNAUTHORIZED:
	case HTTP_FORBIDDEN:
		return SHOW_RELAY_DENIED;
	case HTTP_NOT_FOUND:
		return SHOW_RELAY_NOT_FOUND;
	case HTTP_BAD_REQUEST:
		return SHOW_RELAY_BAD;
	case HTTP_OTHER_4XX:
		return SHOW_RELAY_REFUSED;
	case HTTP_ABORTED:
		return SHOW_RELAY_ABORTED;
	case HTTP_LOCAL_FILE:
		return SHOW_RELAY_LOCAL;
	case HTTP_UNREACHABLE:
	case HTTP_SERVER_ERROR:
	case HTTP_BAD_RESPONSE:
	case HTTP_TLS_FAILED:
		break;
	}
	return sy.stopping ? SHOW_RELAY_ABORTED : SHOW_RELAY_OFFLINE;
}

static enum show_relay_result json_call(struct job *job, const char *method, const char *path, const cj *body,
					cj **reply)
{
	struct dstr url = {0};
	dstr_printf(&url, "%s%s", job->base, path);
	char *text = body ? cj_print(body, false) : NULL;
	char *answer = NULL;
	struct http_call call = {
		.method = method,
		.url = url.array,
		.bearer = job->token,
		.body = text,
		.interrupt = interrupt_cb,
	};
	enum http_status status = http_call_text(&call, reply ? &answer : NULL);
	dstr_free(&url);
	if (text) {
		memset(text, 0, strlen(text)); /* the save body carries secrets */
		cj_free_str(text);
	}
	enum show_relay_result r = map_status(status);
	if (r == SHOW_RELAY_OK && reply) {
		*reply = answer ? cj_parse(answer, strlen(answer), NULL, 0) : NULL;
		if (answer && !*reply)
			r = SHOW_RELAY_OFFLINE; /* not JSON: a proxy page, not Relay */
	}
	if (answer) {
		memset(answer, 0, strlen(answer));
		bfree(answer);
	}
	return r;
}

static void progress_cb(void *opaque, uint64_t done, uint64_t total)
{
	(void)opaque;
	lock();
	sy.done = done;
	sy.total = total;
	sy.seq++;
	unlock();
}

static void step_cb(void *opaque, const char *step, uint64_t done, uint64_t total)
{
	(void)opaque;
	lock();
	set_str(&sy.step, step);
	sy.done = done;
	sy.total = total;
	sy.seq++;
	unlock();
}

static enum show_relay_result op_check(void *opaque, const cj *body, cj **missing)
{
	cj *reply = NULL;
	*missing = NULL;
	enum show_relay_result r = json_call(opaque, "POST", "/blobs/check", body, &reply);
	if (r == SHOW_RELAY_OK) {
		cj *list = cj_take(reply, "missing");
		*missing = cj_is_array(list) ? list : cj_new_array();
		if (!cj_is_array(list))
			cj_free(list);
	}
	cj_free(reply);
	return r;
}

static enum show_relay_result op_put_blob(void *opaque, const char *sha256, const char *kind, const char *path)
{
	struct job *job = opaque;
	struct dstr url = {0};
	char sent[65] = "";
	dstr_printf(&url, "%s/blobs/%s?kind=%s", job->base, sha256, kind);
	enum http_status status = http_put_file(url.array, job->token, path, sent, interrupt_cb, progress_cb, job);
	dstr_free(&url);
	if (status == HTTP_OK && strcmp(sent, sha256) != 0)
		return SHOW_RELAY_LOCAL; /* the file changed while it was sent */
	return map_status(status);
}

static enum show_relay_result op_post_revision(void *opaque, const char *profile, const cj *body, cj **reply)
{
	struct dstr path = {0};
	dstr_printf(&path, "/profiles/%s/show/revisions", profile);
	enum show_relay_result r = json_call(opaque, "POST", path.array, body, reply);
	dstr_free(&path);
	return r;
}

static enum show_relay_result op_head(void *opaque, const char *profile, int64_t *head)
{
	struct dstr path = {0};
	cj *reply = NULL;
	*head = 0;
	dstr_printf(&path, "/profiles/%s/show", profile);
	enum show_relay_result r = json_call(opaque, "GET", path.array, NULL, &reply);
	dstr_free(&path);
	if (r == SHOW_RELAY_OK)
		*head = cj_get_int(reply, "headRevision", 0);
	else if (r == SHOW_RELAY_NOT_FOUND)
		r = SHOW_RELAY_OK; /* 404 not_found: nothing saved yet */
	cj_free(reply);
	return r;
}

static enum show_relay_result op_get_revision(void *opaque, const char *profile, int64_t revision, cj **item)
{
	struct dstr path = {0};
	dstr_printf(&path, "/profiles/%s/show/revisions/%lld", profile, (long long)revision);
	enum show_relay_result r = json_call(opaque, "GET", path.array, NULL, item);
	dstr_free(&path);
	return r;
}

static enum show_relay_result get_file(struct job *job, const char *path, const char *dest, char sha256_out[65])
{
	struct dstr url = {0};
	dstr_printf(&url, "%s%s", job->base, path);
	enum http_status status =
		http_get_file(url.array, job->token, dest, sha256_out, interrupt_cb, progress_cb, job);
	dstr_free(&url);
	return map_status(status);
}

static enum show_relay_result op_get_bundle(void *opaque, const char *profile, int64_t revision, const char *dest,
					    char sha256_out[65])
{
	struct dstr path = {0};
	dstr_printf(&path, "/profiles/%s/show/revisions/%lld/bundle", profile, (long long)revision);
	enum show_relay_result r = get_file(opaque, path.array, dest, sha256_out);
	dstr_free(&path);
	return r;
}

static enum show_relay_result op_get_blob(void *opaque, const char *sha256, const char *dest, char sha256_out[65])
{
	struct dstr path = {0};
	dstr_printf(&path, "/blobs/%s", sha256);
	enum show_relay_result r = get_file(opaque, path.array, dest, sha256_out);
	dstr_free(&path);
	return r;
}

static enum show_relay_result op_get_secrets(void *opaque, const char *profile, int64_t revision, cj **items)
{
	struct dstr path = {0};
	cj *reply = NULL;
	*items = NULL;
	dstr_printf(&path, "/profiles/%s/show/revisions/%lld/secrets", profile, (long long)revision);
	enum show_relay_result r = json_call(opaque, "GET", path.array, NULL, &reply);
	dstr_free(&path);
	if (r == SHOW_RELAY_OK)
		*items = cj_take(reply, "items");
	cj_free(reply);
	return r;
}

static struct show_relay_ops relay_ops(struct job *job)
{
	struct show_relay_ops ops = {
		.opaque = job,
		.check = op_check,
		.put_blob = op_put_blob,
		.post_revision = op_post_revision,
		.head = op_head,
		.get_revision = op_get_revision,
		.get_bundle = op_get_bundle,
		.get_blob = op_get_blob,
		.get_secrets = op_get_secrets,
		.progress = step_cb,
	};
	return ops;
}

/* ------------------------------------------------------------- jobs --- */

static void job_free(struct job *job)
{
	if (!job)
		return;
	if (job->token) {
		memset(job->token, 0, strlen(job->token));
		bfree(job->token);
	}
	bfree(job->base);
	bfree(job);
}

static const char *remote_saved_by(const char *relay)
{
	/* called with the mutex held */
	for (size_t i = 0; relay && i < sy.remote_count; i++)
		if (strcmp(sy.remotes[i].relay_profile_id, relay) == 0)
			return sy.remotes[i].saved_by;
	return "";
}

static void set_conflict(const struct job *job, int64_t head)
{
	cj *meta = show_store_meta(job->id);
	const char *relay = cj_get_str(meta, "relayProfileId");
	lock();
	sy.conflict = true;
	snprintf(sy.conflict_id, sizeof(sy.conflict_id), "%s", job->id);
	set_str(&sy.conflict_name, job->name);
	sy.conflict_head = head;
	set_str(&sy.conflict_by, remote_saved_by(relay && *relay ? relay : job->relay));
	sy.conflict_seq++;
	sy.seq++;
	unlock();
	cj_free(meta);
}

static void clear_conflict(void)
{
	lock();
	sy.conflict = false;
	sy.conflict_id[0] = '\0';
	sy.seq++;
	unlock();
}

struct after_job {
	struct job *job;
	char id[37]; /* the profile to switch to ("" none) */
	bool update_ready;
	bool ok;
};

/* UI thread, queued by the worker as its last step */
static void after_job_ui(void *param)
{
	struct after_job *a = param;
	show_join_worker();
	sy.job_running = false;
	if (a->update_ready && a->id[0])
		cj_set_bool(sy.update_ready, a->id, true);
	if (a->job->kind == JOB_PULL && !a->ok && a->job->automatic && a->job->relay[0])
		cj_set_int(sy.pull_backoff, a->job->relay, now_s() + PULL_BACKOFF_S);
	if (a->job->switch_after && a->ok && a->id[0])
		show_switch(a->id, 0);
	job_free(a->job);
	bfree(a);
	if (sy.wake)
		os_event_signal(sy.wake);
}

static void message_for(const struct job *job, const struct show_sync_outcome *out, bool ok)
{
	if (job->kind == JOB_SAVE_UP || (job->kind == JOB_RESOLVE && job->choice == SHOW_CONFLICT_KEEP_MINE)) {
		if (ok && out->no_changes)
			show_set_message(false, "%s is already on CTD Relay (revision %lld).", job->name,
					 (long long)out->revision);
		else if (ok && out->secrets_local)
			show_set_message(false,
					 "Saved %s to CTD Relay as revision %lld (%d value(s) stay on this machine).",
					 job->name, (long long)out->revision, out->secrets_local);
		else if (ok)
			show_set_message(false, "Saved %s to CTD Relay as revision %lld.", job->name,
					 (long long)out->revision);
		else if (out->conflict)
			show_set_message(true, "%s Choose which version to keep.", out->error);
		else if (out->result == SHOW_RELAY_OFFLINE)
			show_set_message(
				true,
				"CTD Relay can't be reached: %s stays on this machine and is saved up when Relay is "
				"back.",
				job->name);
		else if (out->result == SHOW_RELAY_DENIED)
			show_set_message(true,
					 "CTD Relay refused: pair this machine again so it gets Show Profiles access "
					 "(show:read, show:write).");
		else
			show_set_message(true, "%s", out->error);
		return;
	}
	if (!ok) {
		if (out->result == SHOW_RELAY_DENIED)
			show_set_message(true,
					 "CTD Relay refused: pair this machine again so it gets Show Profiles access.");
		else if (!job->automatic || out->result != SHOW_RELAY_OFFLINE)
			show_set_message(true, "%s", out->error);
		return;
	}
	if (job->kind == JOB_RESOLVE && job->choice == SHOW_CONFLICT_SAVE_AS_NEW)
		show_set_message(
			false,
			"Kept your version as a new local show profile; %s now has revision %lld from CTD Relay.",
			job->name, (long long)out->revision);
	else if (job->kind == JOB_RESOLVE)
		show_set_message(
			false,
			"%s now has revision %lld from CTD Relay; your version is kept as an older local revision.",
			job->name, (long long)out->revision);
	else if (out->no_changes)
		show_set_message(false, "This machine already has revision %lld.", (long long)out->revision);
	else if (job->switch_after)
		show_set_message(false, "Downloaded revision %lld; switching…", (long long)out->revision);
	else
		show_set_message(false,
				 "Downloaded revision %lld from CTD Relay: switch to it to apply it (Update ready).",
				 (long long)out->revision);
}

static void *job_thread(void *param)
{
	struct job *job = param;
	struct show_sync_outcome out;
	const struct show_relay_ops ops = relay_ops(job);
	bool ok = false;
	os_set_thread_name("ctd-show-sync");
	memset(&out, 0, sizeof(out));

	switch (job->kind) {
	case JOB_SAVE_UP:
		ok = show_flow_save_up(&ops, job->id, job->relay[0] ? job->relay : NULL, -1, job->scratch, NULL, &out);
		if (ok || out.conflict || out.result == SHOW_RELAY_DENIED)
			show_sync_queue_remove(job->id);
		else if (out.result == SHOW_RELAY_OFFLINE)
			show_sync_queue_add(job->id, now_s(), out.error);
		if (out.conflict)
			set_conflict(job, out.head);
		break;
	case JOB_PULL:
		ok = show_flow_pull(&ops, job->relay, 0, NULL, job->scratch, &out);
		break;
	case JOB_RESOLVE:
		ok = show_flow_resolve(&ops, job->id, job->relay[0] ? job->relay : NULL, job->choice, job->head,
				       job->scratch, &out);
		if (ok)
			clear_conflict();
		else if (out.conflict)
			set_conflict(job, out.head); /* Keep mine met yet another revision */
		snprintf(out.id, sizeof(out.id), "%s", job->id);
		break;
	}
	if (!ok && out.result != SHOW_RELAY_ABORTED)
		blog(LOG_WARNING, SHOW_LOG_PREFIX "sync: %s", out.error);
	message_for(job, &out, ok);

	char active[37];
	struct after_job *a = bzalloc(sizeof(*a));
	a->job = job;
	a->ok = ok;
	snprintf(a->id, sizeof(a->id), "%s", out.id);
	a->update_ready = ok && !out.no_changes && !job->switch_after && job->kind != JOB_SAVE_UP &&
			  strcmp(show_active_id_copy(active), out.id) == 0;
	if (job->kind == JOB_RESOLVE && job->choice == SHOW_CONFLICT_KEEP_MINE)
		a->update_ready = false;

	lock();
	set_str(&sy.step, "");
	sy.done = sy.total = 0;
	sy.seq++;
	unlock();
	show_reload_entries();
	show_set_busy(SHOW_IDLE);
	obs_queue_task(OBS_TASK_UI, after_job_ui, a, false);
	return NULL;
}

/* UI thread */
static bool start_job(struct job *job)
{
	char *base = NULL, *token = NULL, *profile = NULL;
	if (!ms_show_access(&base, &token, &profile)) {
		show_set_message(true, "Connect the dock to CTD Relay first.");
		job_free(job);
		return false;
	}
	bfree(profile);
	job->base = base;
	job->token = token;
	const char *root = show_store_root();
	if (!root || !*root || !sfs_join(job->scratch, sizeof(job->scratch), root, "sync-tmp")) {
		job_free(job);
		return false;
	}
	cj *meta = job->id[0] ? show_store_meta(job->id) : NULL;
	if (meta && cj_get_str(meta, "name"))
		snprintf(job->name, sizeof(job->name), "%s", cj_get_str(meta, "name"));
	cj_free(meta);
	const enum show_busy busy = job->kind == JOB_SAVE_UP ? SHOW_UPLOADING : SHOW_PULLING;
	lock();
	set_str(&sy.step, job->kind == JOB_SAVE_UP ? "Uploading" : "Downloading");
	sy.done = sy.total = 0;
	sy.seq++;
	unlock();
	sy.job_running = true;
	if (!show_begin_job(busy, job_thread, job)) {
		sy.job_running = false;
		lock();
		set_str(&sy.step, "");
		sy.seq++;
		unlock();
		job_free(job);
		return false;
	}
	return true;
}

static struct job *new_job(enum job_kind kind)
{
	struct job *job = bzalloc(sizeof(*job));
	job->kind = kind;
	return job;
}

static bool start_save_up(const char *id, bool automatic)
{
	struct job *job = new_job(JOB_SAVE_UP);
	snprintf(job->id, sizeof(job->id), "%s", id);
	job->automatic = automatic;
	lock();
	if (sy.relay_profile_id)
		snprintf(job->relay, sizeof(job->relay), "%s", sy.relay_profile_id);
	unlock();
	if (job->relay[0] && !safe_relay_id(job->relay))
		job->relay[0] = '\0';
	return start_job(job);
}

static bool start_pull(const char *relay, bool switch_after, bool automatic)
{
	if (!safe_relay_id(relay)) {
		show_set_message(true, "That CTD Relay profile can't be pulled.");
		return false;
	}
	struct job *job = new_job(JOB_PULL);
	snprintf(job->relay, sizeof(job->relay), "%s", relay);
	job->switch_after = switch_after;
	job->automatic = automatic;
	return start_job(job);
}

/* --------------------------------------------------------- dock API --- */

bool show_sync_save_up(const char *id)
{
	char active[37];
	cj *meta = show_store_meta(id);
	const bool exists = meta != NULL;
	const char *relay = cj_get_str(meta, "relayProfileId");
	bool linked = relay && *relay;
	cj_free(meta);
	if (!exists) {
		show_set_message(true, "That show profile is gone.");
		return false;
	}
	lock();
	const bool have_target = linked || (sy.relay_profile_id && *sy.relay_profile_id);
	unlock();
	if (!have_target) {
		show_set_message(true,
				 "Pick a CTD Relay profile in the dock first: the show profile is saved up to it.");
		return false;
	}
	if (strcmp(show_active_id_copy(active), id) == 0) {
		/* the setup loaded now: save it here first (Update), then upload what that saved */
		if (!show_update(id))
			return false;
		snprintf(sy.pending_upload, sizeof(sy.pending_upload), "%s", id);
		os_event_signal(sy.wake);
		return true;
	}
	return start_save_up(id, false);
}

bool show_sync_pull(const char *relay_profile_id, bool switch_after)
{
	return start_pull(relay_profile_id, switch_after, false);
}

bool show_sync_resolve(enum show_conflict_choice choice)
{
	lock();
	const bool pending = sy.conflict;
	struct job *job = pending ? new_job(JOB_RESOLVE) : NULL;
	if (job) {
		snprintf(job->id, sizeof(job->id), "%s", sy.conflict_id);
		job->head = sy.conflict_head;
		if (sy.relay_profile_id && safe_relay_id(sy.relay_profile_id))
			snprintf(job->relay, sizeof(job->relay), "%s", sy.relay_profile_id);
	}
	unlock();
	if (!job) {
		show_set_message(true, "There is nothing to decide any more.");
		return false;
	}
	job->choice = choice;
	return start_job(job);
}

void show_sync_dismiss_conflict(void)
{
	clear_conflict();
}

/* -------------------------------------------------------- the tick --- */

static const struct show_sync_remote *find_remote(const struct show_sync_remote *remotes, size_t n, const char *relay)
{
	for (size_t i = 0; relay && *relay && i < n; i++)
		if (strcmp(remotes[i].relay_profile_id, relay) == 0)
			return &remotes[i];
	return NULL;
}

static void free_remotes(struct show_sync_remote *remotes, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		bfree(remotes[i].relay_profile_id);
		bfree(remotes[i].name);
		bfree(remotes[i].saved_by);
	}
	bfree(remotes);
}

/* UI thread: refresh the dock's view, then start one automatic action when idle */
static void tick_ui(void *unused)
{
	(void)unused;
	sy.tick_queued = false;
	if (sy.stopping || !sy.initialized)
		return;

	struct ms_snapshot ms;
	ms_snapshot_get(&ms);
	struct show_state st;
	show_state_get(&st);
	const bool connected = ms.link == MS_CONNECTED;

	/* Relay profiles and their show heads (GET /profiles items: show = {headRevision, savedBy}) */
	const size_t pn = ms.profiles ? obs_data_array_count(ms.profiles) : 0;
	struct show_sync_remote *remotes = pn ? bzalloc(pn * sizeof(*remotes)) : NULL;
	size_t rn = 0;
	const char *chosen_name = "";
	obs_data_t *chosen = NULL;
	for (size_t i = 0; i < pn; i++) {
		obs_data_t *item = obs_data_array_item(ms.profiles, i);
		const char *pid = obs_data_get_string(item, "id");
		if (ms.profile_id && strcmp(pid, ms.profile_id) == 0) {
			obs_data_release(chosen);
			chosen = item;
			obs_data_addref(chosen);
		}
		obs_data_t *show = obs_data_get_obj(item, "show");
		if (show && safe_relay_id(pid)) {
			struct show_sync_remote *r = &remotes[rn++];
			r->relay_profile_id = bstrdup(pid);
			r->name = bstrdup(obs_data_get_string(item, "name"));
			r->head = obs_data_get_int(show, "headRevision");
			obs_data_t *by = obs_data_get_obj(show, "savedBy");
			const char *app = by ? obs_data_get_string(by, "appId") : "";
			r->saved_by = bstrdup(strncmp(app, APP_ID_PREFIX, strlen(APP_ID_PREFIX)) == 0
						      ? app + strlen(APP_ID_PREFIX)
						      : app);
			obs_data_release(by);
		}
		obs_data_release(show);
		obs_data_release(item);
	}
	if (chosen)
		chosen_name = obs_data_get_string(chosen, "name");

	/* each local profile against Relay */
	struct show_sync_entry *entries = st.count ? bzalloc(st.count * sizeof(*entries)) : NULL;
	for (size_t i = 0; i < st.count; i++) {
		const struct show_entry *e = &st.entries[i];
		struct show_sync_entry *s = &entries[i];
		snprintf(s->id, sizeof(s->id), "%s", e->id);
		const bool linked = e->relay_profile_id && *e->relay_profile_id;
		const struct show_sync_remote *r = find_remote(remotes, rn, e->relay_profile_id);
		int64_t head = -1;
		if (r)
			head = r->head;
		else if (linked && connected && pn > 0)
			head = 0; /* listed without a show profile (or not listed for this token) */
		s->remote_head = head;
		s->status = show_sync_classify(linked, e->revision, e->dirty, connected ? head : -1);
		s->queued = show_sync_queue_has(e->id);
		s->update_ready = cj_get_bool(sy.update_ready, e->id, false) && e->active;
		for (size_t k = 0; linked && k < rn; k++)
			if (strcmp(remotes[k].relay_profile_id, e->relay_profile_id) == 0)
				snprintf(remotes[k].local_id, sizeof(remotes[k].local_id), "%s", e->id);
	}

	lock();
	sy.connected = connected;
	set_str(&sy.relay_profile_id, ms.profile_id);
	set_str(&sy.relay_profile_name, chosen_name);
	bfree(sy.entries);
	sy.entries = entries;
	sy.count = st.count;
	free_remotes(sy.remotes, sy.remote_count);
	sy.remotes = remotes;
	sy.remote_count = rn;
	sy.seq++;
	unlock();
	obs_data_release(chosen);

	/* one automatic action at a time, only when nothing else runs */
	if (st.available && st.busy == SHOW_IDLE && !sy.job_running && !sy.stopping) {
		if (sy.pending_upload[0]) {
			char id[37];
			snprintf(id, sizeof(id), "%s", sy.pending_upload);
			sy.pending_upload[0] = '\0';
			if (!st.message_is_error) /* the Update before it worked (or found no changes) */
				start_save_up(id, false);
		} else if (connected) {
			bool started = false;
			cj *due = show_sync_queue_due(now_s());
			if (cj_size(due))
				started = start_save_up(cj_str(cj_at(due, 0)), true);
			cj_free(due);
			for (size_t i = 0; !started && i < st.count; i++) {
				const struct show_entry *e = &st.entries[i];
				const struct show_sync_entry *s = &entries[i];
				if (s->status != SHOW_SYNC_UPDATE_READY || !safe_relay_id(e->relay_profile_id))
					continue;
				if (cj_get_int(sy.pull_backoff, e->relay_profile_id, 0) > now_s())
					continue;
				started = start_pull(e->relay_profile_id, false, true);
			}
		}
	}

	show_state_free(&st);
	ms_snapshot_free(&ms);
}

static void applied_hook(void *opaque, const char *id, int64_t seq, const cj *manifest)
{
	(void)opaque;
	(void)seq;
	(void)manifest;
	cj_remove(sy.update_ready, id); /* switched to the newest: nothing ready any more */
}

static void *tick_thread(void *unused)
{
	(void)unused;
	os_set_thread_name("ctd-show-sync-tick");
	while (!sy.stopping) {
		os_event_timedwait(sy.wake, TICK_MS);
		if (sy.stopping)
			break;
		if (!sy.tick_queued) {
			sy.tick_queued = true;
			obs_queue_task(OBS_TASK_UI, tick_ui, NULL, false);
		}
	}
	return NULL;
}

static void event_cb(enum obs_frontend_event event, void *unused)
{
	(void)unused;
	if (event == OBS_FRONTEND_EVENT_EXIT) {
		sy.stopping = true; /* the UI thread stops running tasks: no new ticks, transfers stop */
		if (sy.wake)
			os_event_signal(sy.wake);
	}
}

/* ---------------------------------------------------------- lifetime --- */

void show_sync_init(void)
{
	if (sy.initialized)
		return;
	sy.mutex_ok = pthread_mutex_init(&sy.mutex, NULL) == 0;
	sy.update_ready = cj_new_object();
	sy.pull_backoff = cj_new_object();
	sy.step = bstrdup("");
	sy.relay_profile_id = bstrdup("");
	sy.relay_profile_name = bstrdup("");
	sy.conflict_name = bstrdup("");
	sy.conflict_by = bstrdup("");
	/* a crash mid-transfer can leave a bundle tar behind */
	char tmp[PATH_LEN];
	const char *root = show_store_root();
	if (root && *root && sfs_join(tmp, sizeof(tmp), root, "sync-tmp"))
		sfs_remove_tree(tmp);
	show_add_applied_hook(applied_hook, NULL);
	obs_frontend_add_event_callback(event_cb, NULL);
	sy.stopping = false;
	if (os_event_init(&sy.wake, OS_EVENT_TYPE_AUTO) == 0 &&
	    pthread_create(&sy.thread, NULL, tick_thread, NULL) == 0)
		sy.thread_running = true;
	sy.initialized = true;
}

void show_sync_shutdown(void)
{
	if (!sy.initialized)
		return;
	sy.stopping = true;
	if (sy.wake)
		os_event_signal(sy.wake);
	if (sy.thread_running) {
		pthread_join(sy.thread, NULL);
		sy.thread_running = false;
	}
	obs_frontend_remove_event_callback(event_cb, NULL);
	if (sy.wake)
		os_event_destroy(sy.wake);
	sy.wake = NULL;
	lock();
	bfree(sy.step);
	bfree(sy.relay_profile_id);
	bfree(sy.relay_profile_name);
	bfree(sy.conflict_name);
	bfree(sy.conflict_by);
	bfree(sy.entries);
	free_remotes(sy.remotes, sy.remote_count);
	sy.step = sy.relay_profile_id = sy.relay_profile_name = sy.conflict_name = sy.conflict_by = NULL;
	sy.entries = NULL;
	sy.remotes = NULL;
	sy.count = sy.remote_count = 0;
	unlock();
	cj_free(sy.update_ready);
	cj_free(sy.pull_backoff);
	sy.update_ready = sy.pull_backoff = NULL;
	if (sy.mutex_ok)
		pthread_mutex_destroy(&sy.mutex);
	sy.mutex_ok = false;
	sy.initialized = false;
}

/* ------------------------------------------------------------- state --- */

void show_sync_state_get(struct show_sync_state *out)
{
	memset(out, 0, sizeof(*out));
	lock();
	out->relay_connected = sy.connected;
	out->relay_profile_id = bstrdup(sy.relay_profile_id ? sy.relay_profile_id : "");
	out->relay_profile_name = bstrdup(sy.relay_profile_name ? sy.relay_profile_name : "");
	out->step = bstrdup(sy.step ? sy.step : "");
	out->done = sy.done;
	out->total = sy.total;
	out->count = sy.count;
	if (sy.count) {
		out->entries = bmalloc(sy.count * sizeof(*out->entries));
		memcpy(out->entries, sy.entries, sy.count * sizeof(*out->entries));
	}
	out->remote_count = sy.remote_count;
	out->remotes = sy.remote_count ? bzalloc(sy.remote_count * sizeof(*out->remotes)) : NULL;
	for (size_t i = 0; i < sy.remote_count; i++) {
		out->remotes[i] = sy.remotes[i];
		out->remotes[i].relay_profile_id = bstrdup(sy.remotes[i].relay_profile_id);
		out->remotes[i].name = bstrdup(sy.remotes[i].name);
		out->remotes[i].saved_by = bstrdup(sy.remotes[i].saved_by);
	}
	out->conflict = sy.conflict;
	snprintf(out->conflict_id, sizeof(out->conflict_id), "%s", sy.conflict_id);
	out->conflict_name = bstrdup(sy.conflict_name ? sy.conflict_name : "");
	out->conflict_head = sy.conflict_head;
	out->conflict_by = bstrdup(sy.conflict_by ? sy.conflict_by : "");
	out->conflict_seq = sy.conflict_seq;
	out->seq = sy.seq;
	unlock();
}

void show_sync_state_free(struct show_sync_state *s)
{
	if (!s)
		return;
	bfree(s->relay_profile_id);
	bfree(s->relay_profile_name);
	bfree(s->step);
	bfree(s->entries);
	free_remotes(s->remotes, s->remote_count);
	bfree(s->conflict_name);
	bfree(s->conflict_by);
	memset(s, 0, sizeof(*s));
}
