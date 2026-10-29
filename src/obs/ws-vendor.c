/*
 * The obs-websocket vendor "ctd" (SPEC 9, I1 to I3): anything that already talks to OBS over WebSocket can use the
 * plugin with no extra port or password, through CallVendorRequest and VendorEvent. See ws-vendor.h.
 *
 * obs-websocket 5 exposes its vendor API on a proc handler it hands out from OBS's global one
 * ("obs_websocket_api_get_ph"); these few calls are all the plugin needs, so they are written here rather than
 * vendoring obs-websocket's header. Requests run on obs-websocket's thread.
 */

#include "ws-vendor.h"
#include "issues.h"
#include "servers.h"
#include "sync-groups.h"
#include "obs-watch.h"
#include "webhook.h"
#include "../api/poller.h"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/dstr.h>

#include <string.h>

typedef void (*ws_request_fn)(obs_data_t *request, obs_data_t *response, void *param);

/* the shape obs-websocket's vendor_request_register reads from the "callback" pointer */
struct ws_request_callback {
	ws_request_fn callback;
	void *param;
};

static proc_handler_t *ws_ph;
static void *ws_vendor;

/* ---------------------------------------------------------- vendor API -- */

static proc_handler_t *websocket_ph(void)
{
	calldata_t cd = {0};
	proc_handler_t *ph = NULL;
	if (proc_handler_call(obs_get_proc_handler(), "obs_websocket_api_get_ph", &cd))
		ph = calldata_ptr(&cd, "ph");
	calldata_free(&cd);
	return ph;
}

static bool register_request(const char *type, ws_request_fn fn)
{
	/* obs-websocket copies the callback struct, so a stack one will do */
	struct ws_request_callback cb = {fn, NULL};
	calldata_t cd = {0};
	calldata_set_ptr(&cd, "vendor", ws_vendor);
	calldata_set_string(&cd, "type", type);
	calldata_set_ptr(&cd, "callback", &cb);
	proc_handler_call(ws_ph, "vendor_request_register", &cd);
	const bool ok = calldata_bool(&cd, "success");
	calldata_free(&cd);
	return ok;
}

/* ------------------------------------------------------------ requests -- */

/* GetServers: the server profiles, without secrets */
static void get_servers(obs_data_t *request, obs_data_t *response, void *param)
{
	UNUSED_PARAMETER(request);
	UNUSED_PARAMETER(param);
	obs_data_array_t *list = obs_data_array_create();
	for (size_t i = 0; i < servers_count(); i++) {
		struct ctd_server s;
		if (!servers_get_index(i, &s))
			continue;
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "id", s.id);
		obs_data_set_string(item, "name", s.name);
		obs_data_set_string(item, "host", s.host);
		obs_data_set_int(item, "srtPort", s.srt_port);
		obs_data_set_string(item, "apiUrl", s.api_url);
		obs_data_set_bool(item, "isDefault", s.is_default);
		obs_data_set_bool(item, "allowWrites", s.allow_writes);
		obs_data_array_push_back(list, item);
		obs_data_release(item);
		ctd_server_free(&s);
	}
	obs_data_set_array(response, "servers", list);
	obs_data_array_release(list);
}

static const char *api_status_name(enum mtx_api_status status)
{
	switch (status) {
	case MTX_API_OK:
		return "connected";
	case MTX_API_UNKNOWN:
		return "unknown";
	case MTX_API_UNAUTHORIZED:
		return "loginFailed";
	case MTX_API_NOT_FOUND:
		return "noApi";
	default:
		return "unreachable";
	}
}

/* GetStreams {serverId?}: the server's streams, from its poller (the default profile without an id) */
static void get_streams(obs_data_t *request, obs_data_t *response, void *param)
{
	UNUSED_PARAMETER(param);
	struct ctd_server s;
	if (!servers_get(obs_data_get_string(request, "serverId"), &s)) {
		obs_data_set_string(response, "error", "no such server");
		return;
	}
	obs_data_set_string(response, "serverId", s.id);
	struct mtx_poller *poller = mtx_poller_acquire(s.id);
	ctd_server_free(&s);
	struct mtx_poll_snapshot snap;
	mtx_poller_snapshot(poller, &snap);
	obs_data_set_string(response, "status", api_status_name(snap.status));
	obs_data_set_string(response, "version", snap.version);
	obs_data_set_int(response, "latencyMs", snap.latency_ms);
	obs_data_array_t *list = obs_data_array_create();
	for (size_t i = 0; i < snap.count; i++) {
		const struct mtx_stream *st = &snap.items[i];
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "name", st->name);
		obs_data_set_bool(item, "live", st->live);
		obs_data_set_string(item, "publisher", st->publisher);
		obs_data_set_string(item, "tracks", st->tracks);
		obs_data_set_int(item, "readers", st->readers);
		obs_data_set_double(item, "bitrateKbps", st->bitrate_kbps);
		if (st->has_link) {
			obs_data_set_string(item, "remote", st->remote);
			obs_data_set_double(item, "rttMs", st->rtt_ms);
			obs_data_set_double(item, "lossPercent", st->loss_percent);
		}
		obs_data_array_push_back(list, item);
		obs_data_release(item);
	}
	obs_data_set_array(response, "streams", list);
	obs_data_array_release(list);
	mtx_poll_snapshot_free(&snap);
	mtx_poller_release(poller);
}

static bool is_ctd_source(obs_source_t *source)
{
	const char *id = obs_source_get_unversioned_id(source);
	return id && (strcmp(id, "ctd_live_source") == 0 || strcmp(id, "ctd_live_audio_source") == 0);
}

static bool add_feed(void *param, obs_source_t *source)
{
	if (!is_ctd_source(source))
		return true;
	calldata_t cd = {0};
	if (proc_handler_call(obs_source_get_proc_handler(source), "get_feed", &cd)) {
		obs_data_t *feed = obs_data_create_from_json(calldata_string(&cd, "json"));
		if (feed) {
			obs_data_array_push_back(param, feed);
			obs_data_release(feed);
		}
	}
	calldata_free(&cd);
	return true;
}

/* GetFeeds: every CTD source, with its state */
static void get_feeds(obs_data_t *request, obs_data_t *response, void *param)
{
	UNUSED_PARAMETER(request);
	UNUSED_PARAMETER(param);
	obs_data_array_t *list = obs_data_array_create();
	obs_enum_sources(add_feed, list);
	obs_data_set_array(response, "feeds", list);
	obs_data_array_release(list);
}

/* GetFeed {sourceName} */
static void get_feed(obs_data_t *request, obs_data_t *response, void *param)
{
	UNUSED_PARAMETER(param);
	obs_source_t *source = obs_get_source_by_name(obs_data_get_string(request, "sourceName"));
	if (!source || !is_ctd_source(source)) {
		obs_data_set_string(response, "error", "no such CTD source");
		obs_source_release(source);
		return;
	}
	obs_data_array_t *one = obs_data_array_create();
	add_feed(one, source);
	obs_data_t *feed = obs_data_array_count(one) ? obs_data_array_item(one, 0) : NULL;
	if (feed)
		obs_data_set_obj(response, "feed", feed);
	obs_data_release(feed);
	obs_data_array_release(one);
	obs_source_release(source);
}

/* ReconnectFeed {sourceName}: drops the connection and dials again at once (R17) */
static void reconnect_feed(obs_data_t *request, obs_data_t *response, void *param)
{
	UNUSED_PARAMETER(param);
	obs_source_t *source = obs_get_source_by_name(obs_data_get_string(request, "sourceName"));
	bool ok = false;
	if (source && is_ctd_source(source)) {
		calldata_t cd = {0};
		ok = proc_handler_call(obs_source_get_proc_handler(source), "reconnect", &cd);
		calldata_free(&cd);
	}
	obs_source_release(source);
	obs_data_set_bool(response, "ok", ok);
	if (!ok)
		obs_data_set_string(response, "error", "no such CTD source");
}

/* SetAudioRepair {sourceName, channels?, fillGaps?, lockAudio?, rateOverride?, videoOnly?}: section 7. Only the fields
 * given change; videoOnly reconnects the feed. */
static void set_audio_repair(obs_data_t *request, obs_data_t *response, void *param)
{
	UNUSED_PARAMETER(param);
	obs_source_t *source = obs_get_source_by_name(obs_data_get_string(request, "sourceName"));
	if (!source || !is_ctd_source(source)) {
		obs_source_release(source);
		obs_data_set_bool(response, "ok", false);
		obs_data_set_string(response, "error", "no such CTD source");
		return;
	}
	obs_data_t *changes = obs_data_create();
	if (obs_data_has_user_value(request, "channels")) {
		const long long channels = obs_data_get_int(request, "channels");
		if (channels < 0 || channels > 5) {
			obs_data_release(changes);
			obs_source_release(source);
			obs_data_set_bool(response, "ok", false);
			obs_data_set_string(response, "error",
					    "channels is 0 to 5 (stereo, left, right, mono, swap, "
					    "invert right)");
			return;
		}
		obs_data_set_int(changes, "audio_channels", channels);
	}
	if (obs_data_has_user_value(request, "fillGaps"))
		obs_data_set_bool(changes, "fill_audio_gaps", obs_data_get_bool(request, "fillGaps"));
	if (obs_data_has_user_value(request, "lockAudio"))
		obs_data_set_bool(changes, "lock_audio_to_video", obs_data_get_bool(request, "lockAudio"));
	if (obs_data_has_user_value(request, "rateOverride")) {
		const long long rate = obs_data_get_int(request, "rateOverride");
		if (rate != 0 && rate != 44100 && rate != 48000) {
			obs_data_release(changes);
			obs_source_release(source);
			obs_data_set_bool(response, "ok", false);
			obs_data_set_string(response, "error", "rateOverride is 0, 44100 or 48000");
			return;
		}
		obs_data_set_int(changes, "audio_rate_override", rate);
	}
	if (obs_data_has_user_value(request, "videoOnly"))
		obs_data_set_bool(changes, "video_only", obs_data_get_bool(request, "videoOnly"));
	obs_source_update(source, changes);
	obs_data_release(changes);
	obs_source_release(source);
	obs_data_set_bool(response, "ok", true);
}

/* SetDelays (SPEC I2; also SetDelay) {sourceName, videoMs?, audioMs?}: S1, 0 to 2000 ms each; it moves there smoothly (S3) */
static void set_delay(obs_data_t *request, obs_data_t *response, void *param)
{
	UNUSED_PARAMETER(param);
	obs_source_t *source = obs_get_source_by_name(obs_data_get_string(request, "sourceName"));
	const char *error = NULL;
	if (!source || !is_ctd_source(source))
		error = "no such CTD source";
	const char *const fields[2] = {"videoMs", "audioMs"};
	const char *const keys[2] = {"video_delay_ms", "audio_delay_ms"};
	obs_data_t *changes = obs_data_create();
	for (int i = 0; i < 2 && !error; i++) {
		if (!obs_data_has_user_value(request, fields[i]))
			continue;
		const long long ms = obs_data_get_int(request, fields[i]);
		if (ms < 0 || ms > 2000)
			error = "a delay is 0 to 2000 ms";
		else
			obs_data_set_int(changes, keys[i], ms);
	}
	if (!error)
		obs_source_update(source, changes);
	obs_data_release(changes);
	obs_source_release(source);
	obs_data_set_bool(response, "ok", !error);
	if (error)
		obs_data_set_string(response, "error", error);
}

/* SetTimecodeInput {sourceName, input}: S4, 0 none, 1 to 8 LTC on that audio channel, 9 SEI timecode */
static void set_timecode_input(obs_data_t *request, obs_data_t *response, void *param)
{
	UNUSED_PARAMETER(param);
	obs_source_t *source = obs_get_source_by_name(obs_data_get_string(request, "sourceName"));
	const long long input = obs_data_get_int(request, "input");
	const char *error = NULL;
	if (!source || !is_ctd_source(source))
		error = "no such CTD source";
	else if (!obs_data_has_user_value(request, "input") || input < 0 || input > 9)
		error = "input is 0 (none), 1 to 8 (LTC on that channel) or 9 (SEI timecode)";
	if (!error) {
		obs_data_t *changes = obs_data_create();
		obs_data_set_int(changes, "timecode_input", input);
		obs_source_update(source, changes);
		obs_data_release(changes);
	}
	obs_source_release(source);
	obs_data_set_bool(response, "ok", !error);
	if (error)
		obs_data_set_string(response, "error", error);
}

/* GetSyncGroups: every group with members (S15): its settings and, per member, state, timecode, offset and delay */
static void get_sync_groups(obs_data_t *request, obs_data_t *response, void *param)
{
	UNUSED_PARAMETER(request);
	UNUSED_PARAMETER(param);
	struct ctd_sync_group_view *views = bzalloc(sizeof(*views) * 16);
	const size_t count = sync_groups_snapshot(views, 16);
	obs_data_array_t *groups = obs_data_array_create();
	for (size_t g = 0; g < count; g++) {
		const struct ctd_sync_group_view *v = &views[g];
		obs_data_t *group = obs_data_create();
		obs_data_set_string(group, "name", v->name);
		obs_data_set_string(group, "mode", v->mode == SYNC_FIXED_LATENCY ? "fixed" : "align");
		obs_data_set_int(group, "maxDelayMs", v->max_delay_ms);
		obs_data_set_int(group, "fixedLatencyMs", v->fixed_latency_ms);
		obs_data_array_t *members = obs_data_array_create();
		for (int m = 0; m < v->count; m++) {
			const struct ctd_sync_member_view *mv = &v->members[m];
			obs_data_t *member = obs_data_create();
			obs_data_set_string(member, "sourceName", mv->source);
			obs_data_set_string(member, "state", sync_state_name(mv->state));
			obs_data_set_bool(member, "lost", sync_state_lost(mv->state));
			obs_data_set_string(member, "timecode", mv->timecode);
			obs_data_set_double(member, "offsetMs", mv->offset_ms);
			obs_data_set_double(member, "delayMs", mv->delay_ms);
			obs_data_set_bool(member, "reference", mv->reference);
			obs_data_array_push_back(members, member);
			obs_data_release(member);
		}
		obs_data_set_array(group, "members", members);
		obs_data_array_release(members);
		obs_data_array_push_back(groups, group);
		obs_data_release(group);
	}
	bfree(views);
	obs_data_set_array(response, "groups", groups);
	obs_data_array_release(groups);
}

/* SetSyncGroup: {sourceName, group} puts a source in a group ("" for none); {group, mode?, maxDelayMs?,
 * fixedLatencyMs?} changes a group's settings. Both can come in one request. */
static void set_sync_group(obs_data_t *request, obs_data_t *response, void *param)
{
	UNUSED_PARAMETER(param);
	const char *source_name = obs_data_get_string(request, "sourceName");
	const char *group = obs_data_get_string(request, "group");
	const char *error = NULL;
	if (*source_name) {
		obs_source_t *source = obs_get_source_by_name(source_name);
		if (!source || !is_ctd_source(source)) {
			error = "no such CTD source";
		} else if (strlen(group) >= 64) {
			error = "a group name is under 64 characters";
		} else {
			obs_data_t *changes = obs_data_create();
			obs_data_set_string(changes, "sync_group", group);
			obs_source_update(source, changes);
			obs_data_release(changes);
		}
		obs_source_release(source);
	}
	const bool settings = obs_data_has_user_value(request, "mode") ||
			      obs_data_has_user_value(request, "maxDelayMs") ||
			      obs_data_has_user_value(request, "fixedLatencyMs");
	if (!error && settings) {
		const char *mode = obs_data_get_string(request, "mode");
		const int mode_value = !*mode                       ? -1
				       : strcmp(mode, "fixed") == 0 ? SYNC_FIXED_LATENCY
								    : SYNC_ALIGN_TO_SLOWEST;
		const int max = obs_data_has_user_value(request, "maxDelayMs")
					? (int)obs_data_get_int(request, "maxDelayMs")
					: -1;
		const int fixed = obs_data_has_user_value(request, "fixedLatencyMs")
					  ? (int)obs_data_get_int(request, "fixedLatencyMs")
					  : -1;
		if (!*group || !sync_groups_configure(group, mode_value, max, fixed))
			error = "name the group (under 64 characters)";
	}
	if (!error && !*source_name && !settings)
		error = "give a sourceName and group, or a group and its settings";
	obs_data_set_bool(response, "ok", !error);
	if (error)
		obs_data_set_string(response, "error", error);
}

/* GetProgramScenes: every canvas's program scene ("landscape", "vertical" for Aitum's), for overlays that follow it */
static void get_program_scenes(obs_data_t *request, obs_data_t *response, void *param)
{
	UNUSED_PARAMETER(request);
	UNUSED_PARAMETER(param);
	obs_data_t *scenes = obs_watch_program_scenes();
	obs_data_array_t *list = obs_data_get_array(scenes, "scenes");
	obs_data_set_array(response, "scenes", list);
	obs_data_array_release(list);
	obs_data_release(scenes);
}

/* StartSyncCheck {sourceName}: S16, 10 s; the answer shows in GetFeed (syncCheckState, syncCheckOffsetMs).
 * ApplySyncCheck {sourceName}: sets the delays that undo the measured offset. */
static void call_source(obs_data_t *request, obs_data_t *response, const char *proc)
{
	obs_source_t *source = obs_get_source_by_name(obs_data_get_string(request, "sourceName"));
	bool ok = false;
	if (source && is_ctd_source(source)) {
		calldata_t cd = {0};
		ok = proc_handler_call(obs_source_get_proc_handler(source), proc, &cd);
		if (ok && strcmp(proc, "apply_sync_check") == 0)
			ok = calldata_bool(&cd, "ok");
		calldata_free(&cd);
	}
	obs_source_release(source);
	obs_data_set_bool(response, "ok", ok);
	if (!ok)
		obs_data_set_string(response, "error",
				    strcmp(proc, "apply_sync_check") == 0 && source ? "no finished sync check to apply"
										    : "no such CTD source");
}

static void start_sync_check(obs_data_t *request, obs_data_t *response, void *param)
{
	UNUSED_PARAMETER(param);
	call_source(request, response, "start_sync_check");
}

static void apply_sync_check(obs_data_t *request, obs_data_t *response, void *param)
{
	UNUSED_PARAMETER(param);
	call_source(request, response, "apply_sync_check");
}

/* GetPublishUrl {serverId?, path, protocol?}: the URL a camera publishes this path to (H11), on MediaMTX's default
 * ports: srt (the profile's SRT port), rtmp (1935) or whip (8889). No credentials are added. */
static void get_publish_url(obs_data_t *request, obs_data_t *response, void *param)
{
	UNUSED_PARAMETER(param);
	const char *path = obs_data_get_string(request, "path");
	const char *protocol = obs_data_get_string(request, "protocol");
	struct ctd_server s;
	if (!*path || !servers_get(obs_data_get_string(request, "serverId"), &s)) {
		obs_data_set_bool(response, "ok", false);
		obs_data_set_string(response, "error", *path ? "no such server profile" : "give a path");
		return;
	}
	struct dstr url = {0};
	if (strcmp(protocol, "rtmp") == 0)
		dstr_printf(&url, "rtmp://%s:1935/%s", s.host, path);
	else if (strcmp(protocol, "whip") == 0)
		dstr_printf(&url, "http://%s:8889/%s/whip", s.host, path);
	else
		dstr_printf(&url, "srt://%s:%d?streamid=publish:%s", s.host, s.srt_port, path);
	obs_data_set_bool(response, "ok", true);
	obs_data_set_string(response, "url", url.array);
	dstr_free(&url);
	ctd_server_free(&s);
}

/* KickPublisher {serverId?, path}: makes the camera reconnect (H8: only on a profile that allows changes) */
static void kick_publisher(obs_data_t *request, obs_data_t *response, void *param)
{
	UNUSED_PARAMETER(param);
	char message[256];
	const bool ok = issues_kick(obs_data_get_string(request, "serverId"), obs_data_get_string(request, "path"),
				    message, sizeof(message));
	obs_data_set_bool(response, "ok", ok);
	obs_data_set_string(response, "message", message);
}

/* AddFeedToScene {serverId?, path, sceneName?, sourceName?}: a CTD Live Source for the path (sourceName, or the path
 * itself, made unique), added to the scene (the current program scene by default) and fitted to the canvas (H10) */
static void add_feed_to_scene(obs_data_t *request, obs_data_t *response, void *param)
{
	UNUSED_PARAMETER(param);
	const char *path = obs_data_get_string(request, "path");
	const char *scene_name = obs_data_get_string(request, "sceneName");
	struct ctd_server s;
	if (!*path || !servers_get(obs_data_get_string(request, "serverId"), &s)) {
		obs_data_set_bool(response, "ok", false);
		obs_data_set_string(response, "error", *path ? "no such server profile" : "give a path");
		return;
	}
	obs_source_t *scene_source = *scene_name ? obs_get_source_by_name(scene_name)
						 : obs_frontend_get_current_scene();
	obs_scene_t *scene = scene_source ? obs_scene_from_source(scene_source) : NULL;
	if (!scene) {
		obs_source_release(scene_source);
		ctd_server_free(&s);
		obs_data_set_bool(response, "ok", false);
		obs_data_set_string(response, "error", "no such scene");
		return;
	}
	const char *wanted = obs_data_get_string(request, "sourceName");
	struct dstr name = {0};
	dstr_copy(&name, *wanted ? wanted : path);
	for (int n = 2;; n++) {
		obs_source_t *existing = obs_get_source_by_name(name.array);
		if (!existing)
			break;
		obs_source_release(existing);
		dstr_printf(&name, "%s %d", *wanted ? wanted : path, n);
	}
	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "server", s.id);
	obs_data_set_string(settings, "path", path);
	obs_source_t *source = obs_source_create("ctd_live_source", name.array, settings, NULL);
	obs_data_release(settings);
	bool ok = false;
	if (source) {
		obs_sceneitem_t *item = obs_scene_add(scene, source);
		struct obs_video_info ovi;
		if (item && obs_get_video_info(&ovi)) {
			const struct vec2 bounds = {(float)ovi.base_width, (float)ovi.base_height};
			obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER);
			obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
			obs_sceneitem_set_bounds(item, &bounds);
		}
		ok = item != NULL;
		blog(LOG_INFO, "[ctd-live] obs-websocket: added '%s' to '%s'", name.array,
		     obs_source_get_name(scene_source));
		obs_source_release(source);
	}
	obs_data_set_bool(response, "ok", ok);
	obs_data_set_string(response, "sourceName", name.array);
	dstr_free(&name);
	obs_source_release(scene_source);
	ctd_server_free(&s);
}

/* ResetAudio {sourceName}: rebuilds the audio decoder and resampler, video untouched (A5) */
static void reset_audio(obs_data_t *request, obs_data_t *response, void *param)
{
	UNUSED_PARAMETER(param);
	obs_source_t *source = obs_get_source_by_name(obs_data_get_string(request, "sourceName"));
	bool ok = false;
	if (source && is_ctd_source(source)) {
		calldata_t cd = {0};
		ok = proc_handler_call(obs_source_get_proc_handler(source), "reset_audio", &cd);
		calldata_free(&cd);
	}
	obs_source_release(source);
	obs_data_set_bool(response, "ok", ok);
	if (!ok)
		obs_data_set_string(response, "error", "no such CTD source");
}

/* GetIssues: the raised issues, newest first (D2) */
static void get_issues(obs_data_t *request, obs_data_t *response, void *param)
{
	UNUSED_PARAMETER(request);
	UNUSED_PARAMETER(param);
	struct issue_list issues;
	issues_get(&issues);
	obs_data_array_t *list = obs_data_array_create();
	for (size_t i = 0; i < issues.count; i++) {
		const struct issue *x = &issues.items[i];
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "id", x->id);
		obs_data_set_string(item, "rule", x->rule);
		obs_data_set_string(item, "severity", issue_severity_name(x->severity));
		obs_data_set_string(item, "source", x->source);
		obs_data_set_string(item, "serverId", x->server_id);
		obs_data_set_string(item, "path", x->path);
		obs_data_set_string(item, "title", x->title);
		obs_data_set_string(item, "cause", x->cause);
		obs_data_set_int(item, "firstSeen", x->first_seen_ms);
		obs_data_set_int(item, "lastSeen", x->last_seen_ms);
		obs_data_set_bool(item, "appliedFix", x->applied_fix);
		obs_data_array_t *actions = obs_data_array_create();
		for (int a = 0; a < x->action_count; a++) {
			obs_data_t *action = obs_data_create();
			obs_data_set_string(action, "id", x->actions[a].id);
			obs_data_set_string(action, "label", x->actions[a].label);
			obs_data_array_push_back(actions, action);
			obs_data_release(action);
		}
		obs_data_set_array(item, "actions", actions);
		obs_data_array_release(actions);
		obs_data_array_push_back(list, item);
		obs_data_release(item);
	}
	obs_data_set_array(response, "issues", list);
	obs_data_array_release(list);
	issue_list_free(&issues);
}

/* ApplyFix {issueId, actionId}: one of an issue's fixes, or "undo" on an applied one (D4) */
static void apply_fix(obs_data_t *request, obs_data_t *response, void *param)
{
	UNUSED_PARAMETER(param);
	char message[256], url[512];
	const bool ok = issues_apply(obs_data_get_string(request, "issueId"), obs_data_get_string(request, "actionId"),
				     message, sizeof(message), url, sizeof(url));
	obs_data_set_bool(response, "ok", ok);
	obs_data_set_string(response, "message", message);
	if (*url)
		obs_data_set_string(response, "url", url);
}

/* ---------------------------------------------------------------- API -- */

void ws_vendor_register(void)
{
	ws_ph = websocket_ph();
	if (!ws_ph) {
		blog(LOG_INFO, "[ctd-live] obs-websocket isn't loaded: no \"ctd\" vendor requests");
		return;
	}
	calldata_t cd = {0};
	calldata_set_string(&cd, "name", "ctd");
	proc_handler_call(ws_ph, "vendor_register", &cd);
	ws_vendor = calldata_ptr(&cd, "vendor");
	calldata_free(&cd);
	if (!ws_vendor) {
		blog(LOG_WARNING, "[ctd-live] couldn't register the obs-websocket vendor \"ctd\"");
		return;
	}
	const bool ok = register_request("GetServers", get_servers) && register_request("GetStreams", get_streams) &&
			register_request("GetFeeds", get_feeds) && register_request("GetFeed", get_feed) &&
			register_request("ReconnectFeed", reconnect_feed) &&
			register_request("SetAudioRepair", set_audio_repair) &&
			register_request("ResetAudio", reset_audio) && register_request("SetDelays", set_delay) &&
			register_request("SetDelay", set_delay) && /* the name step 16 shipped with */
			register_request("SetTimecodeInput", set_timecode_input) &&
			register_request("GetSyncGroups", get_sync_groups) &&
			register_request("GetProgramScenes", get_program_scenes) &&
			register_request("StartSyncCheck", start_sync_check) &&
			register_request("GetPublishUrl", get_publish_url) &&
			register_request("KickPublisher", kick_publisher) &&
			register_request("AddFeedToScene", add_feed_to_scene) &&
			register_request("ApplySyncCheck", apply_sync_check) &&
			register_request("SetSyncGroup", set_sync_group) && register_request("GetIssues", get_issues) &&
			register_request("ApplyFix", apply_fix);
	blog(ok ? LOG_INFO : LOG_WARNING, "[ctd-live] obs-websocket vendor \"ctd\" %s",
	     ok ? "registered" : "partly registered");
}

void ws_vendor_emit(const char *event_type, obs_data_t *data)
{
	webhook_post(event_type, data); /* I4: the same events, without obs-websocket too */
	if (!ws_ph || !ws_vendor)
		return;
	calldata_t cd = {0};
	calldata_set_ptr(&cd, "vendor", ws_vendor);
	calldata_set_string(&cd, "type", event_type);
	calldata_set_ptr(&cd, "data", data);
	proc_handler_call(ws_ph, "vendor_event_emit", &cd);
	calldata_free(&cd);
}
