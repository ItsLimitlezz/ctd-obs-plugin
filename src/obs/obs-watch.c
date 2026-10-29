/*
 * OBS's outputs and program scenes, for CTD Relay (API v1.4): obs.output when the main stream, the recording or
 * another streaming output (Aitum Vertical's) goes live, reconnects or stops, and obs.scene when a canvas's program
 * scene changes. Scene changes also go to obs-websocket as the vendor event ProgramSceneChanged, so an overlay can
 * follow the vertical canvas too (OBS's own CurrentProgramSceneChanged only covers the main one). See obs-watch.h.
 *
 * One thread polls libobs: scenes ten times a second (an overlay should switch with the cut, and a transition counts
 * from the moment it starts: its destination is the program from then on), outputs once a second. Outputs and canvases
 * are thread-safe to read, and polling sees Aitum's outputs and canvases the same way as OBS's own, which the
 * frontend's events don't. Every minute the whole state is sent again, so the hub catches up after it (or this
 * connection) was away.
 */

#include "obs-watch.h"
#include "ws-vendor.h"
#include "../multistream.h"

#include <obs-module.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include <errno.h>
#include <string.h>

#define WATCH_EVERY_MS 100
#define OUTPUTS_EVERY 10 /* rounds: once a second */
#define RESEND_EVERY_NS (60ull * 1000000000ull)
#define MAX_TRACKED 32

struct tracked {
	char key[160];   /* output: its name; scene: its canvas label */
	char value[160]; /* output: its state; scene: the scene's name */
	char uuid[48];   /* scene: its uuid */
	bool active_once;
	uint64_t list_hash;     /* scene: the canvas's scene list, to notice a scene added, renamed or removed */
	obs_data_array_t *list; /* scene: that list, [{name, uuid}] (for GetProgramScenes) */
};

static struct {
	pthread_t thread;
	bool running;
	os_event_t *stop;
	struct tracked outputs[MAX_TRACKED];
	size_t output_count;
	struct tracked scenes[MAX_TRACKED];
	size_t scene_count;
	uint64_t resent_ns;
	bool resend;
	pthread_mutex_t scenes_mutex; /* scenes: the thread writes, GetProgramScenes reads */
} watch;

static struct tracked *find(struct tracked *list, size_t *count, const char *key)
{
	for (size_t i = 0; i < *count; i++) {
		if (strcmp(list[i].key, key) == 0)
			return &list[i];
	}
	if (*count >= MAX_TRACKED)
		return NULL;
	struct tracked *t = &list[(*count)++];
	memset(t, 0, sizeof(*t));
	snprintf(t->key, sizeof(t->key), "%s", key);
	return t;
}

/* ------------------------------------------------------------- canvases -- */

/* "landscape" for OBS's main canvas, "vertical" for one taller than wide (Aitum Vertical), otherwise its name */
static void canvas_label(obs_canvas_t *canvas, char *out, size_t size)
{
	obs_canvas_t *main_canvas = obs_get_main_canvas();
	const bool is_main = canvas == main_canvas;
	obs_canvas_release(main_canvas);
	struct obs_video_info ovi;
	if (is_main)
		snprintf(out, size, "landscape");
	else if (obs_canvas_get_video_info(canvas, &ovi) && ovi.base_height > ovi.base_width)
		snprintf(out, size, "vertical");
	else
		snprintf(out, size, "%s", obs_canvas_get_name(canvas));
}

struct canvas_for_video {
	video_t *video;
	char label[64];
};

static bool match_canvas(void *param, obs_canvas_t *canvas)
{
	struct canvas_for_video *find_video = param;
	if (obs_canvas_get_video(canvas) != find_video->video)
		return true;
	canvas_label(canvas, find_video->label, sizeof(find_video->label));
	return false;
}

/* ----------------------------------------------------------------- scenes -- */

/* every scene of a canvas, [{name, uuid}] in its own order (groups left out), and a hash of it */
#define MAX_LISTED_SCENES 80 /* CTD Relay takes at most 8 KB of data per event */

struct scene_list {
	obs_data_array_t *array;
	uint64_t hash;
	bool truncated; /* more scenes than MAX_LISTED_SCENES */
};

static bool add_scene(void *param, obs_source_t *source)
{
	struct scene_list *list = param;
	if (obs_source_is_group(source))
		return true;
	const char *name = obs_source_get_name(source);
	const char *uuid = obs_source_get_uuid(source);
	if (obs_data_array_count(list->array) < MAX_LISTED_SCENES) {
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "name", name ? name : "");
		obs_data_set_string(item, "uuid", uuid ? uuid : "");
		obs_data_array_push_back(list->array, item);
		obs_data_release(item);
	} else {
		list->truncated = true;
	}
	for (const char *s = name; s && *s; s++) /* FNV-1a over names and uuids */
		list->hash = (list->hash ^ (unsigned char)*s) * 1099511628211ull;
	for (const char *s = uuid; s && *s; s++)
		list->hash = (list->hash ^ (unsigned char)*s) * 1099511628211ull;
	list->hash = (list->hash ^ 0xff) * 1099511628211ull;
	return true;
}

/*
 * To CTD Relay always (obs.scene), to obs-websocket only on a real change (ProgramSceneChanged). scenes is the canvas's
 * whole list, so an app can map scenes before OBS ever switches to them. A change of the list alone goes out like a
 * resend (no "previous"), which apps take as a resync, never as a switch.
 */
static void post_scene(const char *canvas, const char *scene, const char *uuid, const char *previous, bool changed,
		       const struct scene_list *scenes)
{
	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "canvas", canvas);
	obs_data_set_string(data, "scene", scene);
	obs_data_set_string(data, "sceneUuid", uuid);
	obs_data_set_array(data, "scenes", scenes->array);
	if (scenes->truncated)
		obs_data_set_bool(data, "scenesTruncated", true); /* the first MAX_LISTED_SCENES only */
	if (previous && *previous)
		obs_data_set_string(data, "previous", previous);
	obs_data_set_int(data, "atMs", (long long)(os_gettime_ns() / 1000000)); /* host clock, for ordering */
	ms_post_event("obs.scene", data);
	if (changed)
		ws_vendor_emit("ProgramSceneChanged", data);
	obs_data_release(data);
}

static bool watch_canvas(void *param, obs_canvas_t *canvas)
{
	UNUSED_PARAMETER(param);
	/* the program: channel 0, through its transition */
	obs_source_t *source = obs_canvas_get_channel(canvas, 0);
	if (source && obs_source_get_type(source) == OBS_SOURCE_TYPE_TRANSITION) {
		obs_source_t *active = obs_transition_get_active_source(source);
		obs_source_release(source);
		source = active;
	}
	if (!source || !obs_scene_from_source(source)) {
		obs_source_release(source);
		return true;
	}
	char label[64];
	canvas_label(canvas, label, sizeof(label));
	const char *scene = obs_source_get_name(source);
	const char *uuid = obs_source_get_uuid(source);
	struct scene_list list = {obs_data_array_create(), 14695981039346656037ull, false};
	obs_canvas_enum_scenes(canvas, add_scene, &list);
	pthread_mutex_lock(&watch.scenes_mutex);
	struct tracked *t = find(watch.scenes, &watch.scene_count, label);
	const bool changed = t && strcmp(t->value, scene) != 0;
	const bool list_changed = t && t->list_hash != list.hash;
	char previous[160] = "";
	const bool post = t && (watch.resend || changed || list_changed);
	if (post) {
		snprintf(previous, sizeof(previous), "%s", changed ? t->value : "");
		snprintf(t->value, sizeof(t->value), "%s", scene);
		snprintf(t->uuid, sizeof(t->uuid), "%s", uuid ? uuid : "");
		t->list_hash = list.hash;
		obs_data_array_release(t->list);
		t->list = list.array;
		obs_data_array_addref(t->list);
	}
	pthread_mutex_unlock(&watch.scenes_mutex);
	if (post)
		post_scene(label, scene, uuid ? uuid : "", previous, changed, &list);
	obs_data_array_release(list.array);
	obs_source_release(source);
	return true;
}

/* ---------------------------------------------------------------- outputs -- */

/* "main" and "record" for OBS's own stream and recording; any other streaming output by its name; NULL to skip */
static const char *output_label(obs_output_t *output)
{
	const char *name = obs_output_get_name(output);
	if (!name)
		return NULL;
	if (strcmp(name, "simple_stream") == 0 || strcmp(name, "adv_stream") == 0)
		return "main";
	if (strcmp(name, "simple_file_output") == 0 || strcmp(name, "adv_file_output") == 0 ||
	    strcmp(name, "simple_ffmpeg_output") == 0 || strcmp(name, "adv_ffmpeg_output") == 0)
		return "record";
	if (obs_output_get_flags(output) & OBS_OUTPUT_SERVICE)
		return name;
	return NULL; /* the virtual camera, the replay buffer, ... */
}

static void post_output(const char *label, obs_output_t *output, const char *state, bool with_error)
{
	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "output", label);
	struct canvas_for_video find_video = {.video = obs_output_video(output)};
	if (find_video.video)
		obs_enum_canvases(match_canvas, &find_video);
	if (*find_video.label)
		obs_data_set_string(data, "canvas", find_video.label);
	obs_data_set_string(data, "state", state);
	const char *error = with_error ? obs_output_get_last_error(output) : NULL;
	if (error && *error)
		obs_data_set_string(data, "error", error);
	ms_post_event("obs.output", data);
	obs_data_release(data);
}

static bool watch_output(void *param, obs_output_t *output)
{
	UNUSED_PARAMETER(param);
	const char *label = output_label(output);
	if (!label)
		return true;
	const bool active = obs_output_active(output);
	const char *state = !active ? "stopped" : obs_output_reconnecting(output) ? "reconnecting" : "live";
	struct tracked *t = find(watch.outputs, &watch.output_count, label);
	if (!t)
		return true;
	const bool changed = strcmp(t->value, state) != 0;
	const bool was_active = t->active_once;
	snprintf(t->value, sizeof(t->value), "%s", state);
	t->active_once |= active;
	/* an output that has never run is left out: OBS has several idle ones */
	if (t->active_once && (changed || watch.resend))
		post_output(label, output, state, changed && !active && was_active);
	return true;
}

/* ----------------------------------------------------------------- thread -- */

static void *watch_thread(void *unused)
{
	UNUSED_PARAMETER(unused);
	os_set_thread_name("ctd-obs-watch");
	unsigned round = 0;
	while (os_event_timedwait(watch.stop, WATCH_EVERY_MS) == ETIMEDOUT) {
		const uint64_t now = os_gettime_ns();
		watch.resend = now - watch.resent_ns >= RESEND_EVERY_NS;
		if (watch.resend)
			watch.resent_ns = now;
		if (watch.resend || round++ % OUTPUTS_EVERY == 0)
			obs_enum_outputs(watch_output, NULL);
		obs_enum_canvases(watch_canvas, NULL);
	}
	return NULL;
}

void obs_watch_start(void)
{
	if (watch.running || os_event_init(&watch.stop, OS_EVENT_TYPE_MANUAL) != 0)
		return;
	pthread_mutex_init(&watch.scenes_mutex, NULL);
	watch.resent_ns = os_gettime_ns();
	watch.running = pthread_create(&watch.thread, NULL, watch_thread, NULL) == 0;
	if (!watch.running)
		os_event_destroy(watch.stop);
}

void obs_watch_stop(void)
{
	if (!watch.running)
		return;
	os_event_signal(watch.stop);
	pthread_join(watch.thread, NULL);
	os_event_destroy(watch.stop);
	watch.running = false;
	for (size_t i = 0; i < watch.scene_count; i++) {
		obs_data_array_release(watch.scenes[i].list);
		watch.scenes[i].list = NULL;
	}
	watch.scene_count = 0;
	pthread_mutex_destroy(&watch.scenes_mutex);
}

obs_data_t *obs_watch_program_scenes(void)
{
	obs_data_t *result = obs_data_create();
	obs_data_array_t *list = obs_data_array_create();
	if (watch.running) {
		pthread_mutex_lock(&watch.scenes_mutex);
		for (size_t i = 0; i < watch.scene_count; i++) {
			obs_data_t *item = obs_data_create();
			obs_data_set_string(item, "canvas", watch.scenes[i].key);
			obs_data_set_string(item, "scene", watch.scenes[i].value);
			obs_data_set_string(item, "sceneUuid", watch.scenes[i].uuid);
			if (watch.scenes[i].list)
				obs_data_set_array(item, "scenes", watch.scenes[i].list);
			obs_data_array_push_back(list, item);
			obs_data_release(item);
		}
		pthread_mutex_unlock(&watch.scenes_mutex);
	}
	obs_data_set_array(result, "scenes", list);
	obs_data_array_release(list);
	return result;
}
