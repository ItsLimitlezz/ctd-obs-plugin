/* M4 device mapping in the plugin: the libobs side of devices.h. See device-map.h. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* recursive mutexes in util/threading.h under -std=c17 */
#endif
#include "device-map.h"
#include "devices.h"
#include "devices-identity.h"
#include "show-internal.h"
#include "store.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/bmem.h>
#include <util/threading.h>

#include <stdio.h>
#include <string.h>

#define APP_ID_PREFIX "ctd-obs-plugin-"

static struct {
	pthread_mutex_t mutex;
	bool mutex_ok;
	bool initialized;
	volatile bool exiting; /* OBS is closing: the UI thread no longer runs queued tasks */
	char pending_id[37];   /* the switch being staged and its mapping, for the applied hook */
	cj *pending;
	show_devices_open_fn opener;
	void *opener_opaque;
} dm;

static void lock(void)
{
	if (dm.mutex_ok)
		pthread_mutex_lock(&dm.mutex);
}

static void unlock(void)
{
	if (dm.mutex_ok)
		pthread_mutex_unlock(&dm.mutex);
}

static char *dup_or_empty(const char *s)
{
	return bstrdup(s ? s : "");
}

/* ------------------------------------------------- this machine's devices --- */

/* The device list OBS shows in the source's properties: {"id", "name"} per item, in OBS's order */
static cj *list_devices(const char *source_type)
{
	cj *list = cj_new_array();
	const char *id_key = NULL, *name_key = NULL;
	if (!devices_identity_keys(source_type, &id_key, &name_key) || !id_key)
		return list;
	obs_properties_t *props = obs_get_source_properties(source_type);
	if (!props)
		return list;
	obs_property_t *p = obs_properties_get(props, id_key);
	if (p && obs_property_get_type(p) == OBS_PROPERTY_LIST &&
	    obs_property_list_format(p) == OBS_COMBO_FORMAT_STRING) {
		const size_t n = obs_property_list_item_count(p);
		for (size_t i = 0; i < n; i++) {
			const char *value = obs_property_list_item_string(p, i);
			if (!value || !*value || obs_property_list_item_disabled(p, i))
				continue;
			cj *item = cj_append(list, cj_new_object());
			cj_set_str(item, "id", value);
			const char *name = obs_property_list_item_name(p, i);
			cj_set_str(item, "name", name && *name ? name : value);
		}
	}
	obs_properties_destroy(props);
	return list;
}

/* {"<sourceType>": [devices]} for every mappable type in the manifest's devices[] */
static cj *list_candidates(const cj *devices)
{
	cj *candidates = cj_new_object();
	for (size_t i = 0; i < cj_size(devices); i++) {
		const char *type = cj_get_str(cj_at(devices, i), "sourceType");
		const char *cls = devices_identity_class(type);
		if (!cls || strcmp(cls, "window") == 0 || cj_get(candidates, type))
			continue;
		cj_set(candidates, type, list_devices(type));
	}
	return candidates;
}

struct enumerate_task {
	const cj *devices;
	cj *candidates;
};

static void enumerate_ui(void *param)
{
	struct enumerate_task *t = param;
	t->candidates = list_candidates(t->devices);
}

/* Device lists come from the sources' property callbacks, which expect OBS's UI thread (DirectShow enumerates through
 * COM, initialized there). The stage hook runs on the worker while the UI thread is free, so it waits for a UI task. */
static cj *candidates_from_worker(const cj *devices)
{
	struct enumerate_task t = {devices, NULL};
	if (obs_in_task_thread(OBS_TASK_UI))
		enumerate_ui(&t);
	else if (!dm.exiting)
		obs_queue_task(OBS_TASK_UI, enumerate_ui, &t, true);
	return t.candidates ? t.candidates : cj_new_object();
}

/* ------------------------------------------------------------- hooks --- */

static void warn(cj *warnings, const char *fmt, const char *a, const char *b)
{
	char line[400];
	snprintf(line, sizeof(line), fmt, a ? a : "", b ? b : "");
	cj_append(warnings, cj_new_string(line));
}

static bool stage_hook(void *opaque, struct show_stage_ctx *ctx, char *err, size_t err_size)
{
	(void)opaque;
	(void)err;
	(void)err_size;
	const cj *devices = cj_get(ctx->manifest, "devices");
	cj *mapping = NULL;
	if (cj_size(devices)) {
		cj *candidates = candidates_from_worker(devices);
		cj *remembered = show_store_device_map(ctx->id, ctx->machine);
		cj *aliases = show_store_device_aliases(ctx->machine);
		mapping = show_devices_map(devices, candidates, remembered, aliases);
		show_devices_apply(ctx->collection, mapping);
		int need = 0;
		for (size_t i = 0; i < cj_size(mapping); i++) {
			const cj *e = cj_at(mapping, i);
			const char *status = cj_get_str(e, "status");
			if (strcmp(status, SHOW_DEVICE_MISSING) == 0) {
				warn(ctx->warnings, "%s: no matching device on this machine (was %s).",
				     cj_get_str(e, "label"), cj_get_str(e, "was"));
				need++;
			} else if (strcmp(status, SHOW_DEVICE_SUGGESTED) == 0 ||
				   cj_get_bool(e, "rememberedMissing", false)) {
				need++;
			}
		}
		if (need) {
			char line[160];
			snprintf(line, sizeof(line), "%d device(s) need a look: use Devices… to pick them.", need);
			cj_append(ctx->warnings, cj_new_string(line));
		}
		cj_free(candidates);
		cj_free(remembered);
		cj_free(aliases);
	}
	lock();
	cj_free(dm.pending);
	dm.pending = mapping;
	snprintf(dm.pending_id, sizeof(dm.pending_id), "%s", ctx->id);
	unlock();
	return true;
}

static void applied_hook(void *opaque, const char *id, int64_t seq, const cj *manifest)
{
	(void)opaque;
	(void)seq;
	(void)manifest;
	lock();
	cj *mapping = strcmp(dm.pending_id, id) == 0 ? dm.pending : NULL;
	if (mapping)
		dm.pending = NULL;
	dm.pending_id[0] = '\0';
	show_devices_open_fn opener = dm.opener;
	void *opener_opaque = dm.opener_opaque;
	unlock();
	if (!mapping)
		return;
	/* every successful auto-map the user doesn't change is remembered (design section 5) */
	char machine[64];
	show_machine_name(machine, sizeof(machine));
	cj *map = show_store_device_map(id, machine);
	if (!map)
		map = cj_new_object();
	if (show_devices_remember_auto(map, mapping) && !show_store_write_device_map(id, machine, map))
		blog(LOG_WARNING, SHOW_LOG_PREFIX "can't save this machine's device map");
	const bool open = show_devices_needs_fix(mapping);
	cj_free(map);
	cj_free(mapping);
	if (open && opener)
		opener(opener_opaque, id);
}

static void event_cb(enum obs_frontend_event event, void *unused)
{
	(void)unused;
	if (event == OBS_FRONTEND_EVENT_EXIT)
		dm.exiting = true;
}

void show_devices_init(void)
{
	if (dm.initialized)
		return;
	dm.mutex_ok = pthread_mutex_init(&dm.mutex, NULL) == 0;
	show_add_stage_hook(stage_hook, NULL);
	show_add_applied_hook(applied_hook, NULL);
	obs_frontend_add_event_callback(event_cb, NULL);
	dm.initialized = true;
}

void show_devices_shutdown(void)
{
	if (!dm.initialized)
		return;
	obs_frontend_remove_event_callback(event_cb, NULL);
	lock();
	cj_free(dm.pending);
	dm.pending = NULL;
	dm.opener = NULL;
	unlock();
	if (dm.mutex_ok)
		pthread_mutex_destroy(&dm.mutex);
	dm.mutex_ok = false;
	dm.initialized = false;
}

void show_devices_set_opener(show_devices_open_fn fn, void *opaque)
{
	lock();
	dm.opener = fn;
	dm.opener_opaque = opaque;
	unlock();
}

/* ------------------------------------------------------------ dialog --- */

/* the manifest of the revision loaded now */
static cj *applied_manifest(const char *id, char *name, size_t name_size)
{
	char active[37];
	if (strcmp(show_active_id_copy(active), id) != 0)
		return NULL;
	cj *meta = show_store_meta(id);
	int64_t seq = cj_get_int(meta, "appliedSeq", 0);
	snprintf(name, name_size, "%s", meta && cj_get_str(meta, "name") ? cj_get_str(meta, "name") : "");
	cj_free(meta);
	struct show_bundle b;
	if (!show_store_load(id, seq, &b, NULL, 0))
		return NULL;
	cj *manifest = b.manifest;
	b.manifest = NULL;
	show_bundle_free(&b);
	return manifest;
}

static const cj *find_device(const cj *manifest, const char *slot)
{
	const cj *devices = cj_get(manifest, "devices");
	for (size_t i = 0; slot && i < cj_size(devices); i++)
		if (strcmp(cj_get_str(cj_at(devices, i), "slot") ? cj_get_str(cj_at(devices, i), "slot") : "", slot) ==
		    0)
			return cj_at(devices, i);
	return NULL;
}

bool show_devices_get(const char *id, struct show_device_rows *out)
{
	char name[128], machine[64];
	memset(out, 0, sizeof(*out));
	cj *manifest = id ? applied_manifest(id, name, sizeof(name)) : NULL;
	const cj *devices = cj_get(manifest, "devices");
	if (!manifest || !cj_size(devices)) {
		cj_free(manifest);
		return false;
	}
	show_machine_name(machine, sizeof(machine));
	cj *candidates = list_candidates(devices);
	cj *remembered = show_store_device_map(id, machine);
	cj *aliases = show_store_device_aliases(machine);
	cj *mapping = show_devices_map(devices, candidates, remembered, aliases);

	/* "Was: Cam Link 4K (studio-pc)" */
	const char *app = cj_get_str(cj_get(manifest, "savedBy"), "appId");
	const char *saved_on = app && strncmp(app, APP_ID_PREFIX, strlen(APP_ID_PREFIX)) == 0
				       ? app + strlen(APP_ID_PREFIX)
				       : (app ? app : "");

	snprintf(out->id, sizeof(out->id), "%s", id);
	out->profile_name = bstrdup(name);
	out->machine = bstrdup(machine);
	const size_t n = cj_size(mapping);
	out->rows = n ? bzalloc(n * sizeof(*out->rows)) : NULL;
	for (size_t i = 0; i < n; i++) {
		const cj *e = cj_at(mapping, i);
		struct show_device_row *r = &out->rows[i];
		const char *type = cj_get_str(e, "sourceType");
		const char *id_key = NULL, *name_key = NULL;
		devices_identity_keys(type, &id_key, &name_key);
		r->slot = dup_or_empty(cj_get_str(e, "slot"));
		r->label = dup_or_empty(cj_get_str(e, "label"));
		r->source_type = dup_or_empty(type);
		r->device_class = dup_or_empty(cj_get_str(e, "class"));
		char was[400];
		const char *was_name = cj_get_str(e, "was");
		if (saved_on[0] && was_name && *was_name)
			snprintf(was, sizeof(was), "%s (%s)", was_name, saved_on);
		else
			snprintf(was, sizeof(was), "%s", was_name ? was_name : "");
		r->was = bstrdup(was);
		r->status = dup_or_empty(cj_get_str(e, "status"));
		r->score = (int)cj_get_int(e, "score", 0);
		r->remembered_missing = cj_get_bool(e, "rememberedMissing", false);

		/* what the source uses now */
		obs_source_t *source = obs_get_source_by_uuid(r->slot);
		const char *current = NULL;
		obs_data_t *settings = NULL;
		if (source) {
			r->source_name = dup_or_empty(obs_source_get_name(source));
			settings = obs_source_get_settings(source);
			current = id_key ? obs_data_get_string(settings, id_key) : NULL;
		} else {
			r->source_name = bstrdup("");
		}
		r->current_id = dup_or_empty(current);
		r->current_name = bstrdup("");

		const cj *list = cj_get(candidates, type);
		r->option_count = cj_size(list);
		r->options = r->option_count ? bzalloc(r->option_count * sizeof(*r->options)) : NULL;
		for (size_t j = 0; j < r->option_count; j++) {
			const cj *c = cj_at(list, j);
			r->options[j].id = dup_or_empty(cj_get_str(c, "id"));
			r->options[j].name = dup_or_empty(cj_get_str(c, "name"));
			if (current && *current && strcmp(current, r->options[j].id) == 0) {
				bfree(r->current_name);
				r->current_name = bstrdup(r->options[j].name);
			}
		}
		if (!r->current_name[0] && name_key && settings) {
			bfree(r->current_name);
			r->current_name = dup_or_empty(obs_data_get_string(settings, name_key));
		}
		obs_data_release(settings);
		obs_source_release(source);
	}
	out->count = n;
	cj_free(mapping);
	cj_free(aliases);
	cj_free(remembered);
	cj_free(candidates);
	cj_free(manifest);
	return true;
}

void show_devices_free(struct show_device_rows *rows)
{
	if (!rows)
		return;
	for (size_t i = 0; i < rows->count; i++) {
		struct show_device_row *r = &rows->rows[i];
		bfree(r->slot);
		bfree(r->source_name);
		bfree(r->label);
		bfree(r->source_type);
		bfree(r->device_class);
		bfree(r->was);
		bfree(r->status);
		bfree(r->current_id);
		bfree(r->current_name);
		for (size_t j = 0; j < r->option_count; j++) {
			bfree(r->options[j].id);
			bfree(r->options[j].name);
		}
		bfree(r->options);
	}
	bfree(rows->rows);
	bfree(rows->profile_name);
	bfree(rows->machine);
	memset(rows, 0, sizeof(*rows));
}

bool show_devices_choose(const char *id, const char *slot, const char *device_id, const char *device_name, char *err,
			 size_t err_size)
{
	char name[128], machine[64];
	bool ok = false;
	cj *manifest = id ? applied_manifest(id, name, sizeof(name)) : NULL;
	const cj *device = find_device(manifest, slot);
	cj *map = NULL, *aliases = NULL;
	if (!device) {
		snprintf(err, err_size, "That source isn't part of the show profile loaded now.");
		goto done;
	}
	show_machine_name(machine, sizeof(machine));
	const char *type = cj_get_str(device, "sourceType");
	if (device_id && *device_id) {
		const char *id_key = NULL, *name_key = NULL;
		obs_source_t *source = obs_get_source_by_uuid(slot);
		if (!source || !devices_identity_keys(type, &id_key, &name_key) || !id_key) {
			obs_source_release(source);
			snprintf(err, err_size, "That source is gone from the scene collection.");
			goto done;
		}
		/* straight to the source: not a switch, so allowed while live (design section 5) */
		obs_data_t *update = obs_data_create();
		obs_data_set_string(update, id_key, device_id);
		if (name_key && device_name && *device_name)
			obs_data_set_string(update, name_key, device_name);
		obs_source_update(source, update);
		obs_data_release(update);
		obs_source_release(source);
	}
	map = show_store_device_map(id, machine);
	if (!map)
		map = cj_new_object();
	show_devices_remember(map, slot, device_id, device_name);
	if (!show_store_write_device_map(id, machine, map)) {
		snprintf(err, err_size, "Couldn't save this machine's device map.");
		goto done;
	}
	if (device_id && *device_id) {
		aliases = show_store_device_aliases(machine);
		if (!aliases)
			aliases = cj_new_object();
		if (show_devices_alias(aliases, type, cj_get(device, "identity"), device_id, device_name))
			show_store_write_device_aliases(machine, aliases);
	}
	ok = true;
done:
	cj_free(aliases);
	cj_free(map);
	cj_free(manifest);
	return ok;
}
