/* Switch guard + staged apply (ticket T7, design section 6).
 *
 *  1. Stage (worker thread; OBS untouched): load the revision (checksums + manifest checked), resolve asset
 *     placeholders to the cache (every file must be there with its size), put this machine's secrets back, run the
 *     stage hooks (M3 ingest key/overlays, M4 device map), and write the collection + OBS profile into
 *     <store>/staging/<id>/ -- not into OBS's own folders, so OBS's lists never see a half-made profile.
 *  2. Rollback point + swap (UI thread, guard checked again): the target's current files move to
 *     <store>/staging/<id>/backup, the staged ones move into basic/scenes and basic/profiles, and the core's
 *     ctd_show_apply proc loads them without saving the active collection first.
 *  3. Verify: the active collection and profile are the target's, every source in the bundle exists, every asset
 *     file exists, a stream service is set. Checked right after the proc returns and, if OBS is still loading,
 *     again every 250 ms for up to 10 s.
 *  4. Failure at 2 or 3: the backup goes back, the previous collection + profile are loaded through the same proc,
 *     and the error is shown and logged. Success: the backup is deleted and the applied hooks run. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* gethostname, gmtime_r, recursive mutexes in util/threading.h under -std=c17 */
#endif
#include "show-internal.h"
#include "assets.h"
#include "bundle.h"
#include "ini.h"
#include "manifest.h"
#include "scrub.h"
#include "show-fs.h"
#include "store.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <obs.h>
#include <util/bmem.h>
#include <util/platform.h>
#include <util/threading.h>

#include <stdio.h>
#include <string.h>

#define PATH_LEN 2048
#define VERIFY_TRIES 40
#define VERIFY_STEP_MS 250

/* ---------------------------------------------------------------- guard --- */

struct output_check {
	char *reason;
	size_t size;
	bool active;
};

static bool check_output(void *param, obs_output_t *output)
{
	struct output_check *c = param;
	if (!obs_output_active(output))
		return true;
	c->active = true;
	snprintf(c->reason, c->size, "Stop the \"%s\" output first.", obs_output_get_name(output));
	return false;
}

bool show_guard_idle(char *reason, size_t size)
{
	const char *why = NULL;
	if (obs_frontend_streaming_active())
		why = "Stop streaming first.";
	else if (obs_frontend_recording_active())
		why = "Stop recording first.";
	else if (obs_frontend_replay_buffer_active())
		why = "Stop the replay buffer first.";
	else if (obs_frontend_virtualcam_active())
		why = "Stop the virtual camera first.";
	if (why) {
		if (reason && size)
			snprintf(reason, size, "%s", why);
		return false;
	}
	/* everything else: the vertical canvas's outputs, multitrack, other plugins' outputs */
	char buf[256] = "";
	struct output_check c = {buf, sizeof(buf), false};
	obs_enum_outputs(check_output, &c);
	if (c.active) {
		if (reason && size)
			snprintf(reason, size, "%s", buf);
		return false;
	}
	if (reason && size)
		reason[0] = '\0';
	return true;
}

/* ----------------------------------------------------------------- core --- */

bool show_core_available(void)
{
	proc_handler_t *ph = obs_get_proc_handler();
	if (!ph)
		return false;
	calldata_t cd;
	calldata_init(&cd);
	bool found = proc_handler_call(ph, SHOW_PROC_REFRESH, &cd);
	calldata_free(&cd);
	return found;
}

/* C1: false with err when the proc is missing or reports a failure */
static bool core_apply(const char *collection_path, const char *profile_path, char *err, size_t err_size)
{
	proc_handler_t *ph = obs_get_proc_handler();
	calldata_t cd;
	calldata_init(&cd);
	calldata_set_string(&cd, "collection_path", collection_path);
	calldata_set_string(&cd, "profile_path", profile_path);
	bool found = ph && proc_handler_call(ph, SHOW_PROC_APPLY, &cd);
	bool success = true; /* a core that doesn't report success is judged by the verify step */
	const char *error = NULL;
	if (found) {
		calldata_get_bool(&cd, "success", &success);
		calldata_get_string(&cd, "error", &error);
	}
	if (!found)
		snprintf(err, err_size, "This OBS can't switch show profiles (the CTD OBS core is missing).");
	else if (!success)
		snprintf(err, err_size, "OBS refused to load it: %s", error && *error ? error : "no reason given");
	calldata_free(&cd);
	return found && success;
}

/* ------------------------------------------------------------- the job --- */

struct apply_job {
	char id[37];
	int64_t seq;
	char name[SHOW_NAME_MAX + 1];
	char obs_name[SHOW_NAME_MAX + 16]; /* "CTD · <name>" */

	/* OBS paths */
	char basic[PATH_LEN];
	char target_collection[PATH_LEN]; /* basic/scenes/ctd-show-<id>.json */
	char target_profile[PATH_LEN];    /* basic/profiles/ctd-show-<id> */
	char staging[PATH_LEN];           /* <store>/staging/<id> */
	char staged_collection[PATH_LEN];
	char staged_profile[PATH_LEN];
	char backup_collection[PATH_LEN];
	char backup_profile[PATH_LEN];
	bool had_collection, had_profile;

	/* rollback point (UI thread) */
	char prev_collection_name[256];
	char prev_profile_name[256];
	char prev_collection_path[PATH_LEN];
	char prev_profile_path[PATH_LEN];

	/* what verify checks */
	cj *manifest;
	cj *uuids;       /* array of the bundle's top-level source uuids */
	cj *asset_paths; /* array of cache paths */
	cj *warnings;
	int needs_reentry;

	int verify_tries;
	char error[512];
};

static void job_free(struct apply_job *job)
{
	if (!job)
		return;
	cj_free(job->manifest);
	cj_free(job->uuids);
	cj_free(job->asset_paths);
	cj_free(job->warnings);
	bfree(job);
}

static void warn(struct apply_job *job, const char *text)
{
	cj_append(job->warnings, cj_new_string(text));
}

struct resolve_ctx {
	struct apply_job *job;
	char err[256];
};

static bool asset_path_for(void *opaque, const cj *asset, char *out, size_t size)
{
	struct resolve_ctx *r = opaque;
	const char *sha = cj_get_str(asset, "sha256"), *name = cj_get_str(asset, "name");
	if (!show_store_has_asset(sha, name, cj_get_int(asset, "size", -1))) {
		snprintf(r->err, sizeof(r->err), "the file %s isn't on this machine yet", name ? name : sha);
		return false;
	}
	if (!show_store_asset_path(sha, name, out, size))
		return false;
	/* OBS settings use '/' on every platform */
	cj_append(r->job->asset_paths, cj_new_string(out));
	return true;
}

static void collect_uuids(struct apply_job *job, const cj *collection)
{
	static const char *const lists[] = {"sources", "groups", NULL};
	for (int l = 0; lists[l]; l++) {
		const cj *arr = cj_get(collection, lists[l]);
		for (size_t i = 0; i < cj_size(arr); i++) {
			const char *uuid = cj_get_str(cj_at(arr, i), "uuid");
			if (uuid && *uuid)
				cj_append(job->uuids, cj_new_string(uuid));
		}
	}
}

static bool write_file(const char *dir, const char *name, const char *data, size_t size)
{
	char path[PATH_LEN];
	return sfs_join(path, sizeof(path), dir, name) && sfs_write_atomic(path, data, size);
}

/* the service.json to write: this machine's own (key included) when it is for the same kind of service, else the
 * bundle's reduced one with the key to re-enter */
static cj *pick_service(struct apply_job *job, const struct show_bundle *b)
{
	const struct show_file *f = show_bundle_file(b, SHOW_FILE_SERVICE);
	cj *bundle_service = f ? cj_parse(f->data, f->size, NULL, 0) : NULL;
	const cj *local = cj_get(b->local, "service");
	cj *out = NULL;
	if (cj_is_object(local) && (!bundle_service || !cj_get_str(bundle_service, "type") ||
				    (cj_get_str(local, "type") &&
				     strcmp(cj_get_str(local, "type"), cj_get_str(bundle_service, "type")) == 0))) {
		out = cj_clone(local);
		/* the bundle's server wins (it may have moved); the key stays this machine's */
		const char *server = cj_get_str(cj_get(bundle_service, "settings"), "server");
		if (server && !strstr(server, SHOW_SECRET_PLACEHOLDER))
			cj_set_str(cj_get_or_add_object(out, "settings"), "server", server);
	} else if (bundle_service) {
		out = cj_clone(bundle_service);
		warn(job, "The stream key isn't saved on this machine: enter it in Settings > Stream (or link the show "
			  "profile to CTD Relay).");
	}
	cj_free(bundle_service);
	return out;
}

static bool stage(struct apply_job *job, char *err, size_t err_size)
{
	struct show_bundle b;
	if (!show_store_load(job->id, job->seq, &b, err, err_size))
		return false;
	bool ok = false;
	cj *service = NULL;
	char *text = NULL;

	job->manifest = cj_clone(b.manifest);
	snprintf(job->name, sizeof(job->name), "%s", cj_get_str(b.manifest, "name"));
	snprintf(job->obs_name, sizeof(job->obs_name), SHOW_OBS_NAME_PREFIX "%s", job->name);

	/* a bundle from a newer OBS: OBS loads older collections fine, not always newer ones */
	const char *from = cj_get_str(cj_get(b.manifest, "versions"), "obs");
	if (from && *from) {
		int a = 0, bb = 0, c = 0, x = 0, y = 0, z = 0;
		sscanf(from, "%d.%d.%d", &a, &bb, &c);
		sscanf(obs_get_version_string(), "%d.%d.%d", &x, &y, &z);
		if (a > x || (a == x && (bb > y || (bb == y && c > z)))) {
			char line[200];
			snprintf(line, sizeof(line), "Saved with OBS %s, newer than this OBS (%s).", from,
				 obs_get_version_string());
			warn(job, line);
		}
	}

	/* assets -> cache paths */
	struct resolve_ctx r = {job, ""};
	if (!show_assets_resolve(b.collection, b.manifest, asset_path_for, &r, err, err_size)) {
		if (r.err[0])
			snprintf(err, err_size, "Can't switch: %s.", r.err);
		goto done;
	}

	/* this machine's secrets */
	cj *missing = cj_new_array();
	show_secrets_restore(b.collection, cj_get(b.local, "secrets"), missing);
	for (size_t i = 0; i < cj_size(missing); i++)
		if (!cj_get_bool(cj_at(missing, i), "overlay", false))
			job->needs_reentry++;
	if (job->needs_reentry) {
		char line[160];
		snprintf(line, sizeof(line), "%d setting(s) hold a password or key that isn't saved on this machine.",
			 job->needs_reentry);
		warn(job, line);
	}
	cj_free(missing);

	/* name it and tie it to its show profile (design section 2) */
	cj_set_str(b.collection, "name", job->obs_name);
	cj *modules = cj_get_or_add_object(b.collection, "modules");
	cj *tie = cj_set(modules, SHOW_MODULE_KEY, cj_new_object());
	cj_set_str(tie, "id", job->id);
	cj_set_int(tie, "seq", job->seq);
	cj_set_int(tie, "revision", cj_get_int(b.manifest, "revision", 0));

	service = pick_service(job, &b);
	if (!service)
		service = cj_new_object();

	/* M3 (ingest, overlays) and M4 (devices) */
	char machine[64];
	show_machine_name(machine, sizeof(machine));
	struct show_stage_ctx ctx = {job->id, job->seq, b.manifest, b.collection, service, machine, job->warnings};
	if (!show_run_stage_hooks(&ctx, err, err_size))
		goto done;
	collect_uuids(job, b.collection);

	/* write the staged files */
	sfs_remove_tree(job->staging);
	if (!sfs_mkdirs(job->staged_profile)) {
		snprintf(err, err_size, "Can't write in %s.", job->staging);
		goto done;
	}
	text = cj_print(b.collection, true);
	if (!text || !sfs_write_atomic(job->staged_collection, text, strlen(text)))
		goto write_failed;
	cj_free_str(text);
	text = NULL;

	const struct show_file *ini = show_bundle_file(&b, SHOW_FILE_BASIC_INI);
	char existing_ini_path[PATH_LEN];
	char *existing = NULL;
	size_t existing_size = 0;
	if (sfs_join(existing_ini_path, sizeof(existing_ini_path), job->target_profile, "basic.ini"))
		sfs_read(existing_ini_path, &existing, &existing_size);
	char *merged = show_ini_merge(ini ? ini->data : "", existing, job->obs_name);
	free(existing);
	bool ini_ok = merged && write_file(job->staged_profile, "basic.ini", merged, strlen(merged));
	free(merged);
	if (!ini_ok)
		goto write_failed;

	static const struct {
		const char *bundle, *obs;
	} encoders[] = {{SHOW_FILE_STREAM_ENCODER, "streamEncoder.json"},
			{SHOW_FILE_RECORD_ENCODER, "recordEncoder.json"}};
	for (size_t i = 0; i < sizeof(encoders) / sizeof(encoders[0]); i++) {
		const struct show_file *f = show_bundle_file(&b, encoders[i].bundle);
		if (!f)
			continue;
		cj *settings = cj_parse(f->data, f->size, NULL, 0);
		if (!settings)
			continue;
		show_secrets_restore_settings(settings, encoders[i].bundle, cj_get(b.local, "secrets"));
		text = cj_print(settings, true);
		cj_free(settings);
		if (!text || !write_file(job->staged_profile, encoders[i].obs, text, strlen(text)))
			goto write_failed;
		cj_free_str(text);
		text = NULL;
	}
	if (cj_size(service)) {
		text = cj_print(service, true);
		if (!text || !write_file(job->staged_profile, "service.json", text, strlen(text)))
			goto write_failed;
		cj_free_str(text);
		text = NULL;
	} else {
		/* keep this machine's existing service.json when the bundle has none */
		char src[PATH_LEN], dst[PATH_LEN];
		if (sfs_join(src, sizeof(src), job->target_profile, "service.json") && sfs_exists(src) &&
		    sfs_join(dst, sizeof(dst), job->staged_profile, "service.json"))
			sfs_copy_file(src, dst);
	}
	ok = true;
	goto done;

write_failed:
	snprintf(err, err_size, "Can't write the staged files (disk full?).");
done:
	cj_free_str(text);
	cj_free(service);
	show_bundle_free(&b);
	if (!ok)
		sfs_remove_tree(job->staging);
	return ok;
}

/* ----------------------------------------------------- swap and verify --- */

static void fail_job(struct apply_job *job, const char *message)
{
	blog(LOG_WARNING, SHOW_LOG_PREFIX "switch to %s failed: %s", job->id, message);
	show_set_message(true, "%s", message);
	show_set_warnings(job->warnings);
	job->warnings = NULL;
	show_set_busy(SHOW_IDLE);
	show_reload_entries();
	job_free(job);
}

/* puts the previous files and the previous collection/profile back */
static void rollback(struct apply_job *job, const char *why, bool reload)
{
	char err[256] = "";
	blog(LOG_WARNING, SHOW_LOG_PREFIX "rolling back: %s", why);
	sfs_remove(job->target_collection);
	sfs_remove_tree(job->target_profile);
	if (job->had_collection)
		sfs_rename(job->backup_collection, job->target_collection);
	if (job->had_profile)
		sfs_rename(job->backup_profile, job->target_profile);
	bool restored = !reload || (job->prev_collection_path[0] && job->prev_profile_path[0] &&
				    core_apply(job->prev_collection_path, job->prev_profile_path, err, sizeof(err)));
	char message[700];
	if (restored)
		snprintf(message, sizeof(message), "Couldn't switch to %s: %s Your previous setup is back.", job->name,
			 why);
	else
		snprintf(message, sizeof(message),
			 "Couldn't switch to %s: %s Then the previous setup couldn't be reloaded (%s): pick \"%s\" in "
			 "Scene Collection and \"%s\" in Profile.",
			 job->name, why, err[0] ? err : "not found", job->prev_collection_name, job->prev_profile_name);
	sfs_remove_tree(job->staging);
	fail_job(job, message);
}

static bool verify(struct apply_job *job, char *why, size_t size)
{
	char *collection = obs_frontend_get_current_scene_collection();
	char *profile = obs_frontend_get_current_profile();
	bool names = collection && profile && strcmp(collection, job->obs_name) == 0 &&
		     strcmp(profile, job->obs_name) == 0;
	bfree(collection);
	bfree(profile);
	if (!names) {
		snprintf(why, size, "OBS didn't load it.");
		return false;
	}
	for (size_t i = 0; i < cj_size(job->uuids); i++) {
		obs_source_t *source = obs_get_source_by_uuid(cj_str(cj_at(job->uuids, i)));
		if (!source) {
			snprintf(why, size, "a source is missing after loading (%s).", cj_str(cj_at(job->uuids, i)));
			return false;
		}
		obs_source_release(source);
	}
	for (size_t i = 0; i < cj_size(job->asset_paths); i++) {
		if (!sfs_exists(cj_str(cj_at(job->asset_paths, i)))) {
			snprintf(why, size, "a file it uses is gone (%s).", cj_str(cj_at(job->asset_paths, i)));
			return false;
		}
	}
	if (!obs_frontend_get_streaming_service()) {
		snprintf(why, size, "no stream service is set.");
		return false;
	}
	return true;
}

static void succeed(struct apply_job *job)
{
	char now[32];
	show_now_iso(now, sizeof(now));
	if (job->had_collection)
		sfs_remove(job->backup_collection);
	if (job->had_profile)
		sfs_remove_tree(job->backup_profile);
	sfs_remove_tree(job->staging);

	cj *meta = show_store_meta(job->id);
	if (meta) {
		cj_set_int(meta, "appliedSeq", job->seq);
		cj_set_str(meta, "lastAppliedAt", now);
		show_store_write_meta(job->id, meta);
		cj_free(meta);
	}
	show_set_active(job->id);
	show_set_reentry(job->id, job->needs_reentry);
	blog(LOG_INFO, SHOW_LOG_PREFIX "switched to %s (local revision %lld)", job->id, (long long)job->seq);
	show_set_message(false, "Switched to %s.", job->name);
	show_set_warnings(job->warnings);
	job->warnings = NULL;

	/* OBS's menus list the new collection/profile */
	proc_handler_t *ph = obs_get_proc_handler();
	calldata_t cd;
	calldata_init(&cd);
	proc_handler_call(ph, SHOW_PROC_REFRESH, &cd);
	calldata_free(&cd);

	show_set_busy(SHOW_IDLE);
	show_reload_entries();
	show_run_applied_hooks(job->id, job->seq, job->manifest);
	job_free(job);
}

static void verify_ui(void *param);

static void *verify_later(void *param)
{
	os_set_thread_name("ctd-show-verify");
	os_sleep_ms(VERIFY_STEP_MS);
	obs_queue_task(OBS_TASK_UI, verify_ui, param, false);
	return NULL;
}

static void verify_ui(void *param)
{
	struct apply_job *job = param;
	char why[400];
	if (verify(job, why, sizeof(why))) {
		succeed(job);
		return;
	}
	if (++job->verify_tries < VERIFY_TRIES) {
		pthread_t thread;
		if (pthread_create(&thread, NULL, verify_later, job) == 0) {
			pthread_detach(thread);
			return;
		}
	}
	rollback(job, why, true);
}

static bool move_aside(const char *path, const char *backup, bool dir)
{
	if (dir)
		sfs_remove_tree(backup);
	else
		sfs_remove(backup);
	return sfs_rename(path, backup);
}

static void swap_ui(void *param)
{
	struct apply_job *job = param;
	char reason[256], err[512];

	/* the guard again, on the UI thread, right before anything changes (design section 6) */
	if (!show_guard_idle(reason, sizeof(reason))) {
		sfs_remove_tree(job->staging);
		snprintf(err, sizeof(err), "Didn't switch: %s", reason);
		fail_job(job, err);
		return;
	}

	/* rollback point */
	char *collection = obs_frontend_get_current_scene_collection();
	char *profile = obs_frontend_get_current_profile();
	char *profile_path = obs_frontend_get_current_profile_path();
	snprintf(job->prev_collection_name, sizeof(job->prev_collection_name), "%s", collection ? collection : "");
	snprintf(job->prev_profile_name, sizeof(job->prev_profile_name), "%s", profile ? profile : "");
	snprintf(job->prev_profile_path, sizeof(job->prev_profile_path), "%s", profile_path ? profile_path : "");
	if (collection)
		show_find_collection_file(job->basic, collection, job->prev_collection_path,
					  sizeof(job->prev_collection_path));
	bfree(collection);
	bfree(profile);
	bfree(profile_path);

	/* a re-apply of the active show profile: rollback puts its backup back in place and reloads that same path */
	job->had_collection = sfs_exists(job->target_collection);
	job->had_profile = sfs_is_dir(job->target_profile);
	if ((job->had_collection && !move_aside(job->target_collection, job->backup_collection, false)) ||
	    (job->had_profile && !move_aside(job->target_profile, job->backup_profile, true))) {
		if (job->had_collection && sfs_exists(job->backup_collection) && !sfs_exists(job->target_collection))
			sfs_rename(job->backup_collection, job->target_collection);
		sfs_remove_tree(job->staging);
		fail_job(job, "Didn't switch: OBS's files for it are in use. Nothing changed.");
		return;
	}
	if (!sfs_rename(job->staged_collection, job->target_collection) ||
	    !sfs_rename(job->staged_profile, job->target_profile)) {
		/* OBS still has the old one loaded: nothing to reload */
		rollback(job, "the staged files couldn't be moved into place.", false);
		return;
	}

	blog(LOG_INFO, SHOW_LOG_PREFIX "applying %s (local revision %lld)", job->id, (long long)job->seq);
	if (!core_apply(job->target_collection, job->target_profile, err, sizeof(err))) {
		rollback(job, err, true);
		return;
	}
	verify_ui(job);
}

static void *stage_thread(void *param)
{
	struct apply_job *job = param;
	char err[512] = "";
	os_set_thread_name("ctd-show-stage");
	show_set_message(false, "Preparing the switch…");
	if (!stage(job, err, sizeof(err))) {
		fail_job(job, err[0] ? err : "Couldn't prepare the switch.");
		return NULL;
	}
	show_set_message(false, "Switching…");
	obs_queue_task(OBS_TASK_UI, swap_ui, job, false);
	return NULL;
}

static bool job_paths(struct apply_job *job)
{
	char file[64], dir[64];
	if (!show_basic_dir(job->basic, sizeof(job->basic)))
		return false;
	snprintf(file, sizeof(file), SHOW_OBS_FILE_PREFIX "%s.json", job->id);
	snprintf(dir, sizeof(dir), SHOW_OBS_FILE_PREFIX "%s", job->id);
	char scenes[PATH_LEN], profiles[PATH_LEN], staging_root[PATH_LEN];
	return sfs_join(scenes, sizeof(scenes), job->basic, "scenes") &&
	       sfs_join(job->target_collection, PATH_LEN, scenes, file) &&
	       sfs_join(profiles, sizeof(profiles), job->basic, "profiles") &&
	       sfs_join(job->target_profile, PATH_LEN, profiles, dir) &&
	       sfs_join(staging_root, sizeof(staging_root), show_store_root(), "staging") &&
	       sfs_join(job->staging, PATH_LEN, staging_root, job->id) &&
	       sfs_join(job->staged_collection, PATH_LEN, job->staging, "collection.json") &&
	       sfs_join(job->staged_profile, PATH_LEN, job->staging, "profile") &&
	       sfs_join(job->backup_collection, PATH_LEN, staging_root, "backup-collection.json") &&
	       sfs_join(job->backup_profile, PATH_LEN, staging_root, "backup-profile");
}

void show_apply_start(const char *id, int64_t seq)
{
	struct apply_job *job = bzalloc(sizeof(*job));
	snprintf(job->id, sizeof(job->id), "%s", id);
	job->seq = seq;
	job->uuids = cj_new_array();
	job->asset_paths = cj_new_array();
	job->warnings = cj_new_array();
	if (seq <= 0) {
		cj *meta = show_store_meta(id);
		job->seq = cj_get_int(meta, "headSeq", 0);
		cj_free(meta);
	}
	if (job->seq <= 0) {
		fail_job(job, "This show profile has nothing saved yet.");
		return;
	}
	if (!job_paths(job)) {
		fail_job(job, "Can't find OBS's settings folder.");
		return;
	}
	if (!show_run_worker(stage_thread, job)) {
		job_free(job);
		show_set_message(true, "Busy: wait for the current Show Profiles action to finish.");
	}
}

void show_apply_shutdown(void) {}
