/*
 * Mirror of the profile's CTD Relay asset library. See library-mirror.h.
 */

#include "library-mirror.h"
#include "library-plan.h"
#include "../http-client.h"

#include <obs-module.h>
#include <util/bmem.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define LIBRARY_DIR "library"
#define INDEX_FILE "index.json"
#define MAX_ENTRIES 2000

struct entry {
	char name[128];
	char sha256[LIBRARY_SHA256_LEN + 1]; /* lowercase */
	char file[LIBRARY_SHA256_LEN + 8];   /* "<sha256>.cube" */
	int64_t size;                        /* from Relay; 0 when it didn't say */
	bool have;                           /* the file is in the cache */
};

struct entry_list {
	struct entry *items;
	size_t count;
	size_t capacity;
};

static void list_push(struct entry_list *list, const struct entry *e)
{
	if (list->count == list->capacity) {
		list->capacity = list->capacity ? list->capacity * 2 : 32;
		list->items = brealloc(list->items, list->capacity * sizeof(*list->items));
	}
	list->items[list->count++] = *e;
}

static void list_free(struct entry_list *list)
{
	bfree(list->items);
	memset(list, 0, sizeof(*list));
}

static struct {
	pthread_mutex_t mutex;
	pthread_t thread;
	bool running;
	os_event_t *wake; /* auto-reset */
	volatile bool stopping;
	volatile long generation; /* goes up with every request: a transfer running for an older one stops */
	char *dir;

	/* guarded by mutex */
	bool pending;
	char *api_base;
	char *token;
	char *profile_id;
} lm;

struct run {
	long generation;
};

static int run_interrupt(void *opaque)
{
	const struct run *run = opaque;
	return os_atomic_load_bool(&lm.stopping) || os_atomic_load_long(&lm.generation) != run->generation;
}

static void set_string(char **dst, const char *value)
{
	bfree(*dst);
	*dst = value ? bstrdup(value) : NULL;
}

static const char *base_name(const char *path)
{
	const char *base = path;
	for (const char *c = path; *c; c++) {
		if (*c == '/' || *c == '\\')
			base = c + 1;
	}
	return base;
}

static void file_path(struct dstr *out, const char *name)
{
	dstr_printf(out, "%s/%s", lm.dir, name);
}

/* The library as Relay lists it: entries the mirror keeps (LUTs with a valid hash), at most MAX_ENTRIES. */
static bool fetch_list(const char *api_base, const char *token, const char *profile, struct run *run,
		       struct entry_list *list, enum http_status *status_out)
{
	struct dstr url = {0};
	dstr_printf(&url, "%s/library?profile=%s&kind=lut", api_base, profile);
	const struct http_call call = {.method = "GET",
				       .url = url.array,
				       .bearer = token,
				       .interrupt = run_interrupt,
				       .opaque = run};
	obs_data_t *reply = NULL;
	const enum http_status status = http_call_json(&call, &reply);
	dstr_free(&url);
	*status_out = status;
	if (status != HTTP_OK) {
		obs_data_release(reply);
		return false;
	}

	obs_data_array_t *items = reply ? obs_data_get_array(reply, "items") : NULL;
	const size_t count = items ? obs_data_array_count(items) : 0;
	for (size_t i = 0; i < count && list->count < MAX_ENTRIES; i++) {
		obs_data_t *item = obs_data_array_item(items, i);
		const char *sha = obs_data_get_string(item, "sha256");
		const char *name = obs_data_get_string(item, "name");
		const char *ext = library_extension(obs_data_get_string(item, "kind"), name);
		struct entry e = {0};
		if (ext && library_cache_name(sha, ext, e.file, sizeof(e.file))) {
			library_display_name(name, e.name, sizeof(e.name));
			memcpy(e.sha256, e.file, LIBRARY_SHA256_LEN);
			e.sha256[LIBRARY_SHA256_LEN] = 0;
			e.size = obs_data_get_int(item, "size");
			list_push(list, &e);
		}
		obs_data_release(item);
	}
	obs_data_array_release(items);
	obs_data_release(reply);
	return true;
}

/* Downloads one entry unless the cache already has that hash. Returns the transfer's status (HTTP_OK also when the
 * file was already there). The file only gets its hash name once the bytes hash to that name. */
static enum http_status ensure_file(const struct entry *e, const char *api_base, const char *token, struct run *run,
				    bool *downloaded)
{
	struct dstr path = {0};
	file_path(&path, e->file);
	const int64_t have = os_file_exists(path.array) ? os_get_file_size(path.array) : -1;
	if (have >= 0 && (e->size <= 0 || have == e->size)) {
		dstr_free(&path); /* same hash: nothing to fetch */
		return HTTP_OK;
	}

	struct dstr part = {0}, url = {0};
	dstr_printf(&part, "%s/%s.dl", lm.dir, e->file);
	dstr_printf(&url, "%s/blobs/%s", api_base, e->sha256);
	char got[65] = "";
	enum http_status status = http_get_file(url.array, token, part.array, got, run_interrupt, NULL, run);
	if (status == HTTP_OK && strcmp(got, e->sha256) != 0) {
		blog(LOG_WARNING, "[ctd-live] Library mirror: \"%s\" did not match its hash, skipped", e->name);
		status = HTTP_BAD_RESPONSE;
	}
	if (status == HTTP_OK && os_rename(part.array, path.array) != 0)
		status = HTTP_LOCAL_FILE;
	if (status == HTTP_OK)
		*downloaded = true;
	else
		os_unlink(part.array);
	dstr_free(&path);
	dstr_free(&part);
	dstr_free(&url);
	return status;
}

static void write_index(const char *profile, const struct entry_list *list)
{
	obs_data_t *index = obs_data_create();
	obs_data_set_int(index, "version", 1);
	obs_data_set_string(index, "profile", profile);
	obs_data_set_int(index, "syncedAt", (long long)time(NULL));
	obs_data_array_t *items = obs_data_array_create();
	for (size_t i = 0; i < list->count; i++) {
		const struct entry *e = &list->items[i];
		if (!e->have)
			continue;
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "name", e->name);
		obs_data_set_string(item, "file", e->file);
		obs_data_set_string(item, "sha256", e->sha256);
		obs_data_set_int(item, "size", e->size);
		obs_data_array_push_back(items, item);
		obs_data_release(item);
	}
	obs_data_set_array(index, "items", items);
	obs_data_array_release(items);

	struct dstr path = {0};
	file_path(&path, INDEX_FILE);
	if (!obs_data_save_json_safe(index, path.array, "tmp", NULL))
		blog(LOG_WARNING, "[ctd-live] Library mirror: couldn't write the index");
	dstr_free(&path);
	obs_data_release(index);
}

/* Deletes what Relay no longer lists, and downloads left over from a crash. Files that aren't ours are left alone. */
static int remove_stale(const struct entry_list *list)
{
	const char **wanted = bzalloc(sizeof(char *) * (list->count + 1));
	for (size_t i = 0; i < list->count; i++)
		wanted[i] = list->items[i].file;

	struct dstr pattern = {0};
	file_path(&pattern, "*");
	os_glob_t *glob = NULL;
	int removed = 0;
	if (os_glob(pattern.array, 0, &glob) == 0) {
		for (size_t i = 0; i < glob->gl_pathc; i++) {
			if (glob->gl_pathv[i].directory)
				continue;
			const char *name = base_name(glob->gl_pathv[i].path);
			if (library_is_stale(name, wanted, list->count)) {
				os_unlink(glob->gl_pathv[i].path);
				removed++;
			}
		}
		os_globfree(glob);
	}
	dstr_free(&pattern);
	bfree(wanted);
	return removed;
}

static void sync_library(const char *api_base, const char *token, const char *profile, long generation)
{
	struct run run = {.generation = generation};
	os_mkdirs(lm.dir);

	struct entry_list list = {0};
	enum http_status status = HTTP_OK;
	if (!fetch_list(api_base, token, profile, &run, &list, &status)) {
		if (status != HTTP_ABORTED && !run_interrupt(&run)) {
			/* the cache stays as it is: what was mirrored keeps working offline */
			if (status == HTTP_FORBIDDEN)
				blog(LOG_INFO, "[ctd-live] Library mirror: this CTD Relay connection can't read the "
					       "library (pair again to enable Show Profiles)");
			else
				blog(LOG_WARNING, "[ctd-live] Library mirror: couldn't read the library: %s",
				     http_status_text(status));
		}
		list_free(&list);
		return;
	}

	int downloaded = 0, failed = 0;
	bool refused = false;
	for (size_t i = 0; i < list.count; i++) {
		struct entry *e = &list.items[i];
		bool fetched = false;
		bool duplicate = false; /* the same hash listed twice: one file */
		for (size_t j = 0; j < i; j++)
			duplicate = duplicate || strcmp(list.items[j].file, e->file) == 0;
		if (duplicate) {
			e->have = true;
			continue;
		}
		if (refused) {
			failed++;
			continue;
		}
		status = ensure_file(e, api_base, token, &run, &fetched);
		if (status == HTTP_ABORTED || run_interrupt(&run)) {
			list_free(&list); /* a newer request is already waiting, or OBS is closing */
			return;
		}
		e->have = status == HTTP_OK;
		downloaded += fetched;
		if (status != HTTP_OK) {
			failed++;
			blog(LOG_WARNING, "[ctd-live] Library mirror: couldn't fetch \"%s\": %s", e->name,
			     http_status_text(status));
			refused = status == HTTP_UNAUTHORIZED || status == HTTP_FORBIDDEN || status == HTTP_UNREACHABLE;
		}
	}

	write_index(profile, &list);
	const int removed = remove_stale(&list);
	if (downloaded || removed || failed)
		blog(LOG_INFO, "[ctd-live] Library mirror: %d in the library, %d downloaded, %d removed, %d failed",
		     (int)list.count, downloaded, removed, failed);
	list_free(&list);
}

static void *worker(void *unused)
{
	UNUSED_PARAMETER(unused);
	os_set_thread_name("ctd-library-mirror");

	while (!os_atomic_load_bool(&lm.stopping)) {
		os_event_wait(lm.wake);
		if (os_atomic_load_bool(&lm.stopping))
			break;

		pthread_mutex_lock(&lm.mutex);
		const bool pending = lm.pending;
		lm.pending = false;
		char *api_base = lm.api_base ? bstrdup(lm.api_base) : NULL;
		char *token = lm.token ? bstrdup(lm.token) : NULL;
		char *profile = lm.profile_id ? bstrdup(lm.profile_id) : NULL;
		const long generation = os_atomic_load_long(&lm.generation);
		pthread_mutex_unlock(&lm.mutex);

		if (pending && api_base && token && profile)
			sync_library(api_base, token, profile, generation);

		if (token)
			memset(token, 0, strlen(token)); /* the token doesn't linger in freed memory */
		bfree(api_base);
		bfree(token);
		bfree(profile);
	}
	return NULL;
}

void library_mirror_init(void)
{
	if (lm.running)
		return;
	pthread_mutex_init(&lm.mutex, NULL);
	os_event_init(&lm.wake, OS_EVENT_TYPE_AUTO);
	os_atomic_set_bool(&lm.stopping, false);
	lm.dir = obs_module_config_path(LIBRARY_DIR);
	if (pthread_create(&lm.thread, NULL, worker, NULL) == 0)
		lm.running = true;
}

void library_mirror_shutdown(void)
{
	if (!lm.running)
		return;
	os_atomic_set_bool(&lm.stopping, true);
	os_event_signal(lm.wake);
	pthread_join(lm.thread, NULL);
	lm.running = false;
	os_event_destroy(lm.wake);
	pthread_mutex_destroy(&lm.mutex);
	if (lm.token)
		memset(lm.token, 0, strlen(lm.token));
	set_string(&lm.api_base, NULL);
	set_string(&lm.token, NULL);
	set_string(&lm.profile_id, NULL);
	bfree(lm.dir);
	lm.dir = NULL;
}

void library_mirror_request(const char *api_base, const char *token, const char *profile_id)
{
	if (!lm.running || !api_base || !*api_base || !token || !*token || !profile_id || !*profile_id)
		return;
	pthread_mutex_lock(&lm.mutex);
	set_string(&lm.api_base, api_base);
	set_string(&lm.token, token);
	set_string(&lm.profile_id, profile_id);
	lm.pending = true;
	os_atomic_inc_long(&lm.generation); /* stops a transfer for an older request */
	pthread_mutex_unlock(&lm.mutex);
	os_event_signal(lm.wake);
}

char *library_mirror_dir(void)
{
	return lm.dir ? bstrdup(lm.dir) : NULL;
}
