/*
 * The issue engine. See issues.h.
 *
 * Once a second it reads every CTD source's state (its "get_feed" proc call) and the poller of the server profile it
 * reads from, and evaluates the rules (SPEC 6.2, the ones the plugin can measure today). Each rule reports a
 * condition as active for a subject; the bookkeeping below turns those into issues: raised once a condition has held
 * for its rule's time, one per (rule, subject), and cleared only when the condition has been gone for 10 s, so a
 * flapping link doesn't raise and clear an issue every second (D3).
 */

#include "issues.h"
#include "failover.h"
#include "servers.h"
#include "ws-vendor.h"
#include "../api/poller.h"
#include "../multistream.h"

#include <obs-module.h>
#include <util/bmem.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TICK_MS 1000
#define CLEAR_AFTER_NS (10ull * 1000000000ull) /* D2, D3 */
#define S_TO_NS(s) ((uint64_t)(s) * 1000000000ull)

/* one condition being watched, raised or not */
struct condition {
	char *id;
	char *rule;
	enum issue_severity severity;
	char *source;
	char *server_id;
	char *path;
	char *title;
	char *cause;
	uint64_t active_since_ns; /* 0 while the condition is off */
	uint64_t inactive_since_ns;
	bool raised;
	bool seen; /* reported active this tick */
	int64_t first_seen_ms;
	int64_t last_seen_ms;
	struct issue_action actions[ISSUE_MAX_ACTIONS];
	int action_count;
	/* D4: a fix that was applied, listed (with Undo) until it is undone; settle() leaves these alone */
	bool applied_fix;
	char undo_key[32]; /* the source setting it changed */
	long long undo_value;
};

/* D5: a safe fix to apply once the tick is done (outside the lock) */
struct pending_fix {
	char issue_id[160];
	char action_id[24];
};

/* what a source's numbers did over time */
struct source_history {
	char *name;
	long last_resyncs;
	uint64_t snaps[8]; /* when the last snaps happened */
	size_t snap_count;
	bool seen;
};

/* a path's bitrate average, for "bitrate collapsed" */
struct path_history {
	char *key; /* "<server id>|<path>" */
	double average_kbps;
	bool seen;
};

/* a poller the engine keeps for a profile its sources read from */
struct held_poller {
	char id[48];
	struct mtx_poller *poller;
	bool seen;
};

static struct {
	pthread_t thread;
	bool running;
	os_event_t *stop;
	pthread_mutex_t mutex; /* guards conditions (the raised ones are what issues_get returns) and seq */
	DARRAY(struct condition) conditions;
	uint64_t seq;
	/* the thread's own */
	DARRAY(struct source_history) sources;
	DARRAY(struct path_history) paths;
	DARRAY(struct held_poller) pollers;
	DARRAY(struct pending_fix) auto_fixes;
	unsigned long long applied_count;
} engine;

/* --------------------------------------------------------------- helpers -- */

static int64_t epoch_ms(void)
{
	struct timespec ts;
	if (timespec_get(&ts, TIME_UTC) != TIME_UTC)
		return 0;
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void replace(char **field, const char *value)
{
	if (*field && value && strcmp(*field, value) == 0)
		return;
	bfree(*field);
	*field = bstrdup(value ? value : "");
}

static void free_condition(struct condition *c)
{
	bfree(c->id);
	bfree(c->rule);
	bfree(c->source);
	bfree(c->server_id);
	bfree(c->path);
	bfree(c->title);
	bfree(c->cause);
}

const char *issue_severity_name(enum issue_severity severity)
{
	switch (severity) {
	case ISSUE_CRITICAL:
		return "critical";
	case ISSUE_WARNING:
		return "warning";
	default:
		return "info";
	}
}

static obs_data_t *issue_data(const struct condition *c)
{
	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "id", c->id);
	obs_data_set_string(data, "rule", c->rule);
	obs_data_set_string(data, "severity", issue_severity_name(c->severity));
	obs_data_set_string(data, "source", c->source);
	obs_data_set_string(data, "path", c->path);
	obs_data_set_string(data, "title", c->title);
	obs_data_set_string(data, "cause", c->cause);
	obs_data_set_int(data, "firstSeen", c->first_seen_ms);
	/* Relay API 1.11: whether viewers can see it. Only for an issue about one source (a path may feed several). */
	if (*c->source) {
		char scene[FO_NAME_MAX];
		const bool on_program = failover_source_on_program(c->source, scene, sizeof(scene));
		obs_data_set_bool(data, "onProgram", on_program);
		if (on_program)
			obs_data_set_string(data, "scene", scene);
	}
	return data;
}

/* D2: the log, obs-websocket and CTD Relay hear about every raise and clear */
static void announce(const struct condition *c, bool raised)
{
	if (raised)
		blog(c->severity == ISSUE_INFO ? LOG_INFO : LOG_WARNING, "[CTD] live-source: issue (%s): %s. %s",
		     issue_severity_name(c->severity), c->title, c->cause);
	else
		blog(LOG_INFO, "[CTD] live-source: issue cleared: %s (after %lld s)", c->title,
		     (long long)((c->last_seen_ms - c->first_seen_ms) / 1000));
	obs_data_t *data = issue_data(c);
	ws_vendor_emit(raised ? "IssueRaised" : "IssueCleared", data);
	ms_post_event(raised ? "obs.issue.raised" : "obs.issue.cleared", data);
	obs_data_release(data);
}

/* ----------------------------------------------------------- conditions -- */

struct observation {
	const char *rule;
	enum issue_severity severity;
	const char *source;
	const char *server_id;
	const char *path;
	const char *title;
	const char *cause;
	int hold_s; /* how long the condition must hold before it is raised */
	const struct issue_action *actions;
	int action_count;
	const char *auto_action; /* D5: the safe fix to apply when it is raised on a source with Auto-fix on */
};

/* call with the mutex held: a rule saw its condition this tick */
static void observe(const struct observation *o, uint64_t now)
{
	struct condition *c = NULL;
	struct dstr id = {0};
	dstr_printf(&id, "%s|%s", o->rule, *o->source ? o->source : o->path);
	for (size_t i = 0; i < engine.conditions.num && !c; i++) {
		if (strcmp(engine.conditions.array[i].id, id.array) == 0)
			c = &engine.conditions.array[i];
	}
	if (!c) {
		c = da_push_back_new(engine.conditions);
		c->id = bstrdup(id.array);
		c->rule = bstrdup(o->rule);
	}
	dstr_free(&id);
	c->severity = o->severity;
	replace(&c->source, o->source);
	replace(&c->server_id, o->server_id);
	replace(&c->path, o->path);
	replace(&c->title, o->title);
	replace(&c->cause, o->cause);
	c->action_count = o->action_count < ISSUE_MAX_ACTIONS ? o->action_count : ISSUE_MAX_ACTIONS;
	for (int i = 0; i < c->action_count; i++)
		c->actions[i] = o->actions[i];
	c->seen = true;
	c->inactive_since_ns = 0;
	if (!c->active_since_ns)
		c->active_since_ns = now;
	c->last_seen_ms = epoch_ms();
	if (!c->raised && now - c->active_since_ns >= S_TO_NS(o->hold_s)) {
		c->raised = true;
		c->first_seen_ms = c->last_seen_ms - (int64_t)((now - c->active_since_ns) / 1000000);
		engine.seq++;
		announce(c, true);
		if (o->auto_action) {
			struct pending_fix *fix = da_push_back_new(engine.auto_fixes);
			snprintf(fix->issue_id, sizeof(fix->issue_id), "%s", c->id);
			snprintf(fix->action_id, sizeof(fix->action_id), "%s", o->auto_action);
		}
	}
}

/* call with the mutex held, after the rules: conditions not seen this tick go quiet, and clear after 10 s */
static void settle(uint64_t now)
{
	for (size_t i = engine.conditions.num; i > 0; i--) {
		struct condition *c = &engine.conditions.array[i - 1];
		if (c->seen || c->applied_fix) {
			c->seen = false;
			continue;
		}
		c->active_since_ns = 0;
		if (!c->inactive_since_ns)
			c->inactive_since_ns = now;
		const bool gone = !c->raised || now - c->inactive_since_ns >= CLEAR_AFTER_NS;
		if (!gone)
			continue; /* D3: held, in case it comes straight back */
		if (c->raised) {
			engine.seq++;
			announce(c, false);
		}
		free_condition(c);
		da_erase(engine.conditions, i - 1);
	}
}

/* ---------------------------------------------------------------- input -- */

struct feed {
	char name[256];
	char server_id[48];
	char server_name[128];
	char host[256];
	char path[256];
	char state[32];
	char kind[16];
	char video_codec[32];
	char audio_codec[32];
	long resyncs;
	long correction_ppm;
	int sample_rate;
	double peak_db[2];
	double clipped_percent;
	long frozen_ms;
	double decode_ms;
	double fps;
	double nominal_fps;
	int decoder_setting; /* enum ctd_decoder_mode: 1 is CPU only */
	int latency_ms;
	bool auto_fix;         /* D5 */
	int audio_channels;    /* enum ctd_channels: 0 is stereo as sent */
	int timecode_source;   /* enum ctd_timecode_source: 0 is none */
	int timecode_detected; /* S7: the channel (1 to 8) LTC was found on, or 0 */
	char sync_group[64];   /* S9: empty for none */
	char sync_state[16];   /* sync_state_name(), or empty */
	double sync_offset_ms;
	bool writes; /* its profile allows changing the server (H8) */
};

/* the profile a source reads from: its own, or for a "Custom" source, the profile at the same address */
static bool resolve_profile(const char *server_id, const char *host, long long port, struct ctd_server *out)
{
	if (*server_id)
		return servers_get(server_id, out);
	for (size_t i = 0; i < servers_count(); i++) {
		if (!servers_get_index(i, out))
			continue;
		if (strcmp(out->host, host) == 0 && out->srt_port == port)
			return true;
		ctd_server_free(out);
	}
	return false;
}

static bool collect_feed(void *param, obs_source_t *source)
{
	const char *id = obs_source_get_unversioned_id(source);
	if (!id || (strcmp(id, "ctd_live_source") != 0 && strcmp(id, "ctd_live_audio_source") != 0))
		return true;
	calldata_t cd = {0};
	if (proc_handler_call(obs_source_get_proc_handler(source), "get_feed", &cd)) {
		obs_data_t *json = obs_data_create_from_json(calldata_string(&cd, "json"));
		if (json) {
			struct feed f = {0};
			snprintf(f.name, sizeof(f.name), "%s", obs_data_get_string(json, "sourceName"));
			snprintf(f.host, sizeof(f.host), "%s", obs_data_get_string(json, "host"));
			snprintf(f.path, sizeof(f.path), "%s", obs_data_get_string(json, "path"));
			snprintf(f.state, sizeof(f.state), "%s", obs_data_get_string(json, "state"));
			snprintf(f.kind, sizeof(f.kind), "%s", obs_data_get_string(json, "kind"));
			snprintf(f.video_codec, sizeof(f.video_codec), "%s", obs_data_get_string(json, "videoCodec"));
			snprintf(f.audio_codec, sizeof(f.audio_codec), "%s", obs_data_get_string(json, "audioCodec"));
			f.resyncs = (long)obs_data_get_int(json, "resyncs");
			f.correction_ppm = (long)obs_data_get_int(json, "correctionPpm");
			f.sample_rate = (int)obs_data_get_int(json, "sampleRate");
			f.peak_db[0] = obs_data_get_double(json, "peakDbLeft");
			f.peak_db[1] = obs_data_get_double(json, "peakDbRight");
			f.clipped_percent = obs_data_get_double(json, "clippedPercent");
			f.frozen_ms = (long)obs_data_get_int(json, "frozenMs");
			f.decode_ms = obs_data_get_double(json, "decodeMs");
			f.fps = obs_data_get_double(json, "fps");
			f.nominal_fps = obs_data_get_double(json, "nominalFps");
			f.decoder_setting = (int)obs_data_get_int(json, "decoderSetting");
			f.latency_ms = (int)obs_data_get_int(json, "latencyMs");
			f.auto_fix = obs_data_get_bool(json, "autoFix");
			f.audio_channels = (int)obs_data_get_int(json, "audioChannels");
			f.timecode_source = (int)obs_data_get_int(json, "timecodeSource");
			f.timecode_detected = (int)obs_data_get_int(json, "timecodeDetectedChannel");
			snprintf(f.sync_group, sizeof(f.sync_group), "%s", obs_data_get_string(json, "syncGroup"));
			snprintf(f.sync_state, sizeof(f.sync_state), "%s", obs_data_get_string(json, "syncState"));
			f.sync_offset_ms = obs_data_get_double(json, "syncOffsetMs");
			struct ctd_server s;
			if (resolve_profile(obs_data_get_string(json, "serverId"), f.host,
					    obs_data_get_int(json, "port"), &s)) {
				snprintf(f.server_id, sizeof(f.server_id), "%s", s.id);
				snprintf(f.server_name, sizeof(f.server_name), "%s", s.name);
				f.writes = s.allow_writes;
				ctd_server_free(&s);
			}
			DARRAY(struct feed) *feeds = param;
			da_push_back(*feeds, &f);
			obs_data_release(json);
		}
	}
	calldata_free(&cd);
	return true;
}

static struct mtx_poller *poller_for(const char *server_id)
{
	for (size_t i = 0; i < engine.pollers.num; i++) {
		if (strcmp(engine.pollers.array[i].id, server_id) == 0) {
			engine.pollers.array[i].seen = true;
			return engine.pollers.array[i].poller;
		}
	}
	struct held_poller *h = da_push_back_new(engine.pollers);
	snprintf(h->id, sizeof(h->id), "%s", server_id);
	h->poller = mtx_poller_acquire(server_id);
	h->seen = true;
	return h->poller;
}

static struct source_history *history_for(const char *name)
{
	for (size_t i = 0; i < engine.sources.num; i++) {
		if (strcmp(engine.sources.array[i].name, name) == 0) {
			engine.sources.array[i].seen = true;
			return &engine.sources.array[i];
		}
	}
	struct source_history *h = da_push_back_new(engine.sources);
	h->name = bstrdup(name);
	h->last_resyncs = -1;
	h->seen = true;
	return h;
}

static struct path_history *path_history_for(const char *server_id, const char *path)
{
	struct dstr key = {0};
	dstr_printf(&key, "%s|%s", server_id, path);
	for (size_t i = 0; i < engine.paths.num; i++) {
		if (strcmp(engine.paths.array[i].key, key.array) == 0) {
			dstr_free(&key);
			engine.paths.array[i].seen = true;
			return &engine.paths.array[i];
		}
	}
	struct path_history *h = da_push_back_new(engine.paths);
	h->key = key.array;
	h->seen = true;
	return h;
}

/* ---------------------------------------------------------------- rules -- */

/* the path's rules, from the profile's poller: once per path, however many sources read it */
static void path_rules(const struct feed *f, const struct mtx_poll_snapshot *snap, uint64_t now)
{
	if (snap->status != MTX_API_OK)
		return;
	const struct mtx_stream *s = NULL;
	for (size_t i = 0; i < snap->count && !s; i++) {
		if (strcmp(snap->items[i].name, f->path) == 0)
			s = &snap->items[i];
	}
	struct dstr title = {0}, cause = {0};

	/* Camera offline: nothing publishing (or the path is gone altogether) */
	if (!s || !s->live) {
		/* MediaMTX drops a path nothing publishes to unless it is in its config, so a missing path usually means
		 * just that */
		dstr_printf(&title, obs_module_text("Issue.CameraOffline.Title"), f->path);
		dstr_printf(&cause, obs_module_text("Issue.CameraOffline.Cause"), f->path, f->server_name);
		struct issue_action actions[1] = {{"copy-url", ""}};
		snprintf(actions[0].label, sizeof(actions[0].label), "%s",
			 obs_module_text("Issue.CameraOffline.Action"));
		const struct observation o = {"camera-offline", ISSUE_WARNING, "", f->server_id, f->path,
					      title.array,      cause.array,   5,  actions,      1};
		observe(&o, now);
	}

	if (s && s->live) {
		/* Upstream packet loss: the camera's SRT link, over 2% for 10 s */
		if (s->has_link && s->loss_percent > 2.0) {
			const int suggest = (int)fmax(120.0, ceil(4.0 * s->rtt_ms / 10.0) * 10.0);
			dstr_printf(&title, obs_module_text("Issue.UpstreamLoss.Title"), f->path, s->loss_percent);
			dstr_printf(&cause, obs_module_text("Issue.UpstreamLoss.Cause"), suggest, s->rtt_ms);
			const struct observation o = {"upstream-loss", ISSUE_WARNING, "",          f->server_id,
						      f->path,         title.array,   cause.array, 10};
			observe(&o, now);
		}
		/* Upstream link slow: a long round trip. (SRT's link-capacity estimate reads far below the real link
		 * when the sender isn't filling it, so "bitrate above 80% of capacity" would cry wolf; it stays in the
		 * dock's tooltip.) */
		if (s->has_link && s->rtt_ms > 800) {
			dstr_printf(&title, obs_module_text("Issue.UpstreamSlow.Title"), f->path);
			dstr_printf(&cause, obs_module_text("Issue.UpstreamSlow.Cause"), s->rtt_ms);
			const struct observation o = {"upstream-slow", ISSUE_WARNING, "",          f->server_id,
						      f->path,         title.array,   cause.array, 10};
			observe(&o, now);
		}
		/* Bitrate collapsed: under 30% of its own minute-long average for 5 s */
		struct path_history *h = path_history_for(f->server_id, f->path);
		/* still sending: nothing at all is a camera gone, which "camera offline" covers */
		if (h->average_kbps > 300 && s->bitrate_kbps > 0 && s->bitrate_kbps < 0.3 * h->average_kbps) {
			dstr_printf(&title, obs_module_text("Issue.BitrateCollapsed.Title"), f->path);
			dstr_printf(&cause, obs_module_text("Issue.BitrateCollapsed.Cause"), s->bitrate_kbps,
				    h->average_kbps);
			const struct observation o = {"bitrate-collapsed", ISSUE_INFO,  "", f->server_id, f->path,
						      title.array,         cause.array, 5};
			observe(&o, now);
		}
		h->average_kbps += (s->bitrate_kbps - h->average_kbps) * (h->average_kbps > 0 ? 1.0 / 60.0 : 1.0);
	}
	dstr_free(&title);
	dstr_free(&cause);
}

/* the source's own rules, from its engine */
static void source_rules(const struct feed *f, uint64_t now)
{
	struct dstr title = {0}, cause = {0};
	const bool live = strcmp(f->state, "LIVE") == 0;

	/* Frequent snaps: more than 3 hard snaps to the live edge in 5 minutes */
	struct source_history *h = history_for(f->name);
	if (h->last_resyncs >= 0 && f->resyncs > h->last_resyncs) {
		for (long n = h->last_resyncs; n < f->resyncs; n++) {
			memmove(h->snaps + 1, h->snaps, sizeof(h->snaps) - sizeof(*h->snaps));
			h->snaps[0] = now;
			if (h->snap_count < 8)
				h->snap_count++;
		}
	}
	h->last_resyncs = f->resyncs;
	size_t recent = 0;
	for (size_t i = 0; i < h->snap_count; i++)
		recent += now - h->snaps[i] < S_TO_NS(300);
	if (recent > 3) {
		dstr_printf(&title, obs_module_text("Issue.FrequentSnaps.Title"), f->name);
		dstr_printf(&cause, obs_module_text("Issue.FrequentSnaps.Cause"), recent);
		/* D4: double this source's SRT latency (at least 400 ms, at most 2 s); D5 may do it on its own */
		const int target = f->latency_ms * 2 < 400 ? 400 : f->latency_ms * 2 > 2000 ? 2000 : f->latency_ms * 2;
		struct issue_action actions[1] = {{"", ""}};
		snprintf(actions[0].id, sizeof(actions[0].id), "raise-latency:%d", target);
		snprintf(actions[0].label, sizeof(actions[0].label), obs_module_text("Issue.FrequentSnaps.Action"),
			 target, f->latency_ms);
		const bool can_raise = f->latency_ms < 2000;
		const struct observation o = {"frequent-snaps",
					      ISSUE_WARNING,
					      f->name,
					      f->server_id,
					      f->path,
					      title.array,
					      cause.array,
					      0,
					      actions,
					      can_raise ? 1 : 0,
					      can_raise && f->auto_fix ? actions[0].id : NULL};
		observe(&o, now);
	}

	/* A/V drift: soft correction pinned at its 0.2% limit for 30 s */
	if (live && labs(f->correction_ppm) >= 1990) {
		dstr_printf(&title, obs_module_text("Issue.AvDrift.Title"), f->name);
		dstr_copy(&cause, obs_module_text("Issue.AvDrift.Cause"));
		const struct observation o = {"av-drift", ISSUE_INFO,  f->name,     f->server_id,
					      f->path,    title.array, cause.array, 30};
		observe(&o, now);
	}

	/* Audio not at 48 kHz */
	if (live && f->sample_rate > 0 && f->sample_rate != 48000) {
		dstr_printf(&title, obs_module_text("Issue.AudioRate.Title"), f->sample_rate / 1000.0, f->name);
		dstr_printf(&cause, obs_module_text("Issue.AudioRate.Cause"), f->sample_rate / 1000.0);
		const struct observation o = {"audio-rate", ISSUE_INFO,  f->name,     f->server_id,
					      f->path,      title.array, cause.array, 3};
		observe(&o, now);
	}

	/* Audio missing: a video feed with no audio track */
	if (live && strcmp(f->kind, "video") == 0 && *f->video_codec && !*f->audio_codec) {
		dstr_printf(&title, obs_module_text("Issue.AudioMissing.Title"), f->name);
		dstr_copy(&cause, obs_module_text("Issue.AudioMissing.Cause"));
		const struct observation o = {"audio-missing", ISSUE_INFO,  f->name,     f->server_id,
					      f->path,         title.array, cause.array, 5};
		observe(&o, now);
	}

	const bool has_audio = live && *f->audio_codec;
	const double loud = f->peak_db[0] > f->peak_db[1] ? f->peak_db[0] : f->peak_db[1];
	const double quiet = f->peak_db[0] > f->peak_db[1] ? f->peak_db[1] : f->peak_db[0];

	/* Audio silent: both channels under -60 dBFS for 10 s while connected */
	if (has_audio && loud < -60.0) {
		dstr_printf(&title, obs_module_text("Issue.AudioSilent.Title"), f->name);
		dstr_copy(&cause, obs_module_text("Issue.AudioSilent.Cause"));
		const struct observation o = {"audio-silent", ISSUE_WARNING, f->name,     f->server_id,
					      f->path,        title.array,   cause.array, 10};
		observe(&o, now);
	}

	/* Audio clipping: more than 1% of samples at full scale */
	if (has_audio && f->clipped_percent > 1.0) {
		dstr_printf(&title, obs_module_text("Issue.AudioClipping.Title"), f->name);
		dstr_printf(&cause, obs_module_text("Issue.AudioClipping.Cause"), f->clipped_percent);
		const struct observation o = {"audio-clipping", ISSUE_INFO,  f->name,     f->server_id,
					      f->path,          title.array, cause.array, 10};
		observe(&o, now);
	}

	/* One channel dead: one side 40 dB below the other while the other carries sound */
	if (has_audio && loud > -50.0 && loud - quiet > 40.0) {
		dstr_printf(&title, obs_module_text("Issue.ChannelDead.Title"), f->name);
		const bool left_live = f->peak_db[0] > f->peak_db[1];
		dstr_printf(&cause,
			    obs_module_text(left_live ? "Issue.ChannelDead.CauseRight" : "Issue.ChannelDead.CauseLeft"),
			    loud - quiet);
		/* D4/D5: A1, only while the channels are as sent (any other choice was made on purpose) */
		struct issue_action actions[1] = {{"", ""}};
		snprintf(actions[0].id, sizeof(actions[0].id), "channels:%d", left_live ? 1 : 2);
		snprintf(actions[0].label, sizeof(actions[0].label), "%s",
			 obs_module_text(left_live ? "Issue.ChannelDead.ActionLeft" : "Issue.ChannelDead.ActionRight"));
		const bool as_sent = f->audio_channels == 0;
		const struct observation o = {"channel-dead",
					      ISSUE_WARNING,
					      f->name,
					      f->server_id,
					      f->path,
					      title.array,
					      cause.array,
					      10,
					      actions,
					      as_sent ? 1 : 0,
					      as_sent && f->auto_fix ? actions[0].id : NULL};
		observe(&o, now);
	}

	/* S7: LTC on a channel that isn't the timecode input: a loud buzz in the program audio */
	if (has_audio && f->timecode_source == 0 && f->timecode_detected > 0) {
		const int channel = f->timecode_detected;
		const bool side = channel == 1 || channel == 2; /* left or right */
		dstr_printf(&title, obs_module_text("Issue.TimecodeOnProgram.Title"), f->name);
		if (side)
			dstr_copy(&cause, obs_module_text(channel == 1 ? "Issue.TimecodeOnProgram.CauseLeft"
								       : "Issue.TimecodeOnProgram.CauseRight"));
		else
			dstr_printf(&cause, obs_module_text("Issue.TimecodeOnProgram.CauseChannel"), channel);
		struct issue_action actions[1] = {{"", ""}};
		snprintf(actions[0].id, sizeof(actions[0].id), "timecode:%d", channel);
		if (side)
			snprintf(actions[0].label, sizeof(actions[0].label), "%s",
				 obs_module_text(channel == 1 ? "Issue.TimecodeOnProgram.ActionLeft"
							      : "Issue.TimecodeOnProgram.ActionRight"));
		else
			snprintf(actions[0].label, sizeof(actions[0].label),
				 obs_module_text("Issue.TimecodeOnProgram.ActionChannel"), channel);
		const struct observation o = {"timecode-on-program",
					      ISSUE_WARNING,
					      f->name,
					      f->server_id,
					      f->path,
					      title.array,
					      cause.array,
					      3,
					      actions,
					      1,
					      f->auto_fix ? actions[0].id : NULL};
		observe(&o, now);
	}

	/* S12, S13: a sync group member that lost its timecode, or needs more delay than the group allows */
	const bool freewheel = strcmp(f->sync_state, "freewheel") == 0;
	if (*f->sync_group && (freewheel || strcmp(f->sync_state, "at-max") == 0)) {
		dstr_printf(&title, obs_module_text("Issue.SyncLost.Title"), f->name, f->sync_group);
		if (freewheel)
			dstr_copy(&cause, obs_module_text("Issue.SyncLost.CauseFreewheel"));
		else
			dstr_printf(&cause, obs_module_text("Issue.SyncLost.CauseAtMax"), f->sync_offset_ms);
		const struct observation o = {"sync-lost", ISSUE_WARNING, f->name,     f->server_id,
					      f->path,     title.array,   cause.array, 2};
		observe(&o, now);
	}

	/* Frozen picture: frames keep arriving but the picture hasn't changed for 5 s (a stuck camera app) */
	if (live && strcmp(f->kind, "video") == 0 && f->frozen_ms >= 5000) {
		dstr_printf(&title, obs_module_text("Issue.FrozenPicture.Title"), f->name);
		dstr_printf(&cause, obs_module_text("Issue.FrozenPicture.Cause"), f->frozen_ms / 1000);
		struct issue_action actions[1] = {{"kick", ""}};
		snprintf(actions[0].label, sizeof(actions[0].label), "%s",
			 obs_module_text("Issue.FrozenPicture.Action"));
		const struct observation o = {"frozen-picture", ISSUE_WARNING,    f->name,     f->server_id,
					      f->path,          title.array,      cause.array, 0,
					      actions,          f->writes ? 1 : 0};
		observe(&o, now);
	}

	/* Low frame rate: under 80% of the stream's own rate for 10 s */
	if (live && f->nominal_fps > 1 && f->fps > 0 && f->fps < 0.8 * f->nominal_fps) {
		dstr_printf(&title, obs_module_text("Issue.LowFps.Title"), f->name, f->fps, f->nominal_fps);
		dstr_copy(&cause, obs_module_text("Issue.LowFps.Cause"));
		const struct observation o = {"low-fps", ISSUE_INFO,  f->name,     f->server_id,
					      f->path,   title.array, cause.array, 10};
		observe(&o, now);
	}

	/* Decoder falling behind: decoding takes longer than a frame lasts, for 5 s */
	const double fps = f->nominal_fps > 1 ? f->nominal_fps : f->fps;
	if (live && fps > 1 && f->decode_ms > 1000.0 / fps) {
		dstr_printf(&title, obs_module_text("Issue.DecoderBehind.Title"), f->name);
		dstr_printf(&cause,
			    obs_module_text(f->decoder_setting == 1 ? "Issue.DecoderBehind.CauseCpu"
								    : "Issue.DecoderBehind.CauseOverloaded"),
			    f->decode_ms, 1000.0 / fps);
		/* D4/D5: on the CPU, move it to the graphics card (Auto falls back to the CPU on its own if it can't) */
		struct issue_action actions[1] = {{"hardware-decode", ""}};
		snprintf(actions[0].label, sizeof(actions[0].label), "%s",
			 obs_module_text("Issue.DecoderBehind.Action"));
		const bool on_cpu = f->decoder_setting == 1;
		const struct observation o = {"decoder-behind",
					      ISSUE_WARNING,
					      f->name,
					      f->server_id,
					      f->path,
					      title.array,
					      cause.array,
					      5,
					      actions,
					      on_cpu ? 1 : 0,
					      on_cpu && f->auto_fix ? "hardware-decode" : NULL};
		observe(&o, now);
	}
	dstr_free(&title);
	dstr_free(&cause);
}

/* --------------------------------------------------------------- thread -- */

static void tick(void)
{
	DARRAY(struct feed) feeds;
	da_init(feeds);
	obs_enum_sources(collect_feed, &feeds);
	const uint64_t now = os_gettime_ns();

	for (size_t i = 0; i < engine.pollers.num; i++)
		engine.pollers.array[i].seen = false;
	for (size_t i = 0; i < engine.sources.num; i++)
		engine.sources.array[i].seen = false;
	for (size_t i = 0; i < engine.paths.num; i++)
		engine.paths.array[i].seen = false;

	pthread_mutex_lock(&engine.mutex);
	for (size_t i = 0; i < feeds.num; i++) {
		const struct feed *f = &feeds.array[i];
		if (strcmp(f->state, "DISCONNECTED") == 0)
			continue; /* H17: on purpose */
		source_rules(f, now);
		/* path rules once per (profile, path) */
		bool done = false;
		for (size_t k = 0; k < i && !done; k++)
			done = strcmp(feeds.array[k].server_id, f->server_id) == 0 &&
			       strcmp(feeds.array[k].path, f->path) == 0;
		if (done || !*f->server_id || !*f->path)
			continue;
		struct mtx_poller *poller = poller_for(f->server_id);
		struct mtx_poll_snapshot snap;
		mtx_poller_snapshot(poller, &snap);
		path_rules(f, &snap, now);
		mtx_poll_snapshot_free(&snap);
	}
	settle(now);
	/* D5: the safe fixes Auto-fix sources asked for, applied outside the lock */
	struct pending_fix *fixes = engine.auto_fixes.array;
	const size_t fix_count = engine.auto_fixes.num;
	da_init(engine.auto_fixes);
	pthread_mutex_unlock(&engine.mutex);
	da_free(feeds);
	for (size_t i = 0; i < fix_count; i++) {
		char message[256];
		if (issues_apply(fixes[i].issue_id, fixes[i].action_id, message, sizeof(message), NULL, 0))
			blog(LOG_INFO, "[CTD] live-source: auto-fix: %s", message);
	}
	bfree(fixes);

	/* forget what no source uses any more */
	for (size_t i = engine.pollers.num; i > 0; i--) {
		if (!engine.pollers.array[i - 1].seen) {
			mtx_poller_release(engine.pollers.array[i - 1].poller);
			da_erase(engine.pollers, i - 1);
		}
	}
	for (size_t i = engine.sources.num; i > 0; i--) {
		if (!engine.sources.array[i - 1].seen) {
			bfree(engine.sources.array[i - 1].name);
			da_erase(engine.sources, i - 1);
		}
	}
	for (size_t i = engine.paths.num; i > 0; i--) {
		if (!engine.paths.array[i - 1].seen) {
			bfree(engine.paths.array[i - 1].key);
			da_erase(engine.paths, i - 1);
		}
	}
}

static void *issues_thread(void *unused)
{
	UNUSED_PARAMETER(unused);
	os_set_thread_name("ctd-issues");
	while (os_event_timedwait(engine.stop, TICK_MS) == ETIMEDOUT)
		tick();
	return NULL;
}

/* ------------------------------------------------------------------ API -- */

void issues_start(void)
{
	if (engine.running)
		return;
	pthread_mutex_init(&engine.mutex, NULL);
	da_init(engine.conditions);
	da_init(engine.sources);
	da_init(engine.paths);
	da_init(engine.pollers);
	da_init(engine.auto_fixes);
	if (os_event_init(&engine.stop, OS_EVENT_TYPE_MANUAL) != 0)
		return;
	engine.running = pthread_create(&engine.thread, NULL, issues_thread, NULL) == 0;
}

void issues_stop(void)
{
	if (!engine.running)
		return;
	os_event_signal(engine.stop);
	pthread_join(engine.thread, NULL);
	engine.running = false;
	os_event_destroy(engine.stop);
	for (size_t i = 0; i < engine.conditions.num; i++)
		free_condition(&engine.conditions.array[i]);
	da_free(engine.conditions);
	for (size_t i = 0; i < engine.sources.num; i++)
		bfree(engine.sources.array[i].name);
	da_free(engine.sources);
	for (size_t i = 0; i < engine.paths.num; i++)
		bfree(engine.paths.array[i].key);
	da_free(engine.paths);
	for (size_t i = 0; i < engine.pollers.num; i++)
		mtx_poller_release(engine.pollers.array[i].poller);
	da_free(engine.pollers);
	da_free(engine.auto_fixes);
	pthread_mutex_destroy(&engine.mutex);
}

void issues_get(struct issue_list *out)
{
	memset(out, 0, sizeof(*out));
	if (!engine.running)
		return;
	pthread_mutex_lock(&engine.mutex);
	size_t raised = 0;
	for (size_t i = 0; i < engine.conditions.num; i++)
		raised += engine.conditions.array[i].raised;
	out->items = raised ? bzalloc(raised * sizeof(*out->items)) : NULL;
	for (size_t i = 0; i < engine.conditions.num; i++) {
		const struct condition *c = &engine.conditions.array[i];
		if (!c->raised)
			continue;
		struct issue *to = &out->items[out->count++];
		to->id = bstrdup(c->id);
		to->rule = bstrdup(c->rule);
		to->severity = c->severity;
		to->source = bstrdup(c->source);
		to->server_id = bstrdup(c->server_id);
		to->path = bstrdup(c->path);
		to->title = bstrdup(c->title);
		to->cause = bstrdup(c->cause);
		to->first_seen_ms = c->first_seen_ms;
		to->last_seen_ms = c->last_seen_ms;
		to->action_count = c->action_count;
		memcpy(to->actions, c->actions, sizeof(to->actions));
		to->applied_fix = c->applied_fix;
	}
	out->seq = engine.seq;
	pthread_mutex_unlock(&engine.mutex);
	/* newest first */
	for (size_t i = 1; i < out->count; i++) {
		for (size_t k = i; k > 0 && out->items[k].first_seen_ms > out->items[k - 1].first_seen_ms; k--) {
			struct issue swap = out->items[k];
			out->items[k] = out->items[k - 1];
			out->items[k - 1] = swap;
		}
	}
}

uint64_t issues_seq(void)
{
	if (!engine.running)
		return 0;
	pthread_mutex_lock(&engine.mutex);
	const uint64_t seq = engine.seq;
	pthread_mutex_unlock(&engine.mutex);
	return seq;
}

/* ------------------------------------------------------------ D4 fixes -- */

/* a CTD source's integer setting: changes it (obs_source_update reconnects if it must), returns the old value */
static bool set_source_int(const char *source_name, const char *key, long long value, long long *previous)
{
	obs_source_t *source = obs_get_source_by_name(source_name);
	if (!source)
		return false;
	obs_data_t *settings = obs_source_get_settings(source);
	if (previous)
		*previous = obs_data_get_int(settings, key);
	obs_data_t *change = obs_data_create();
	obs_data_set_int(change, key, value);
	obs_source_update(source, change);
	obs_data_release(change);
	obs_data_release(settings);
	obs_source_release(source);
	return true;
}

/* D4: the applied fix stays listed, with Undo, until it is undone */
static void record_fix(const char *source, const char *server_id, const char *path, const char *title,
		       const char *undo_label, const char *key, long long previous)
{
	pthread_mutex_lock(&engine.mutex);
	struct condition *c = da_push_back_new(engine.conditions);
	struct dstr id = {0};
	dstr_printf(&id, "fix-applied|%llu", ++engine.applied_count);
	c->id = id.array;
	c->rule = bstrdup("fix-applied");
	c->severity = ISSUE_INFO;
	c->source = bstrdup(source);
	c->server_id = bstrdup(server_id);
	c->path = bstrdup(path);
	c->title = bstrdup(title);
	c->cause = bstrdup(obs_module_text("Issue.Fix.Cause"));
	c->raised = true;
	c->applied_fix = true;
	c->first_seen_ms = c->last_seen_ms = epoch_ms();
	c->action_count = 1;
	snprintf(c->actions[0].id, sizeof(c->actions[0].id), "undo");
	snprintf(c->actions[0].label, sizeof(c->actions[0].label), "%s", undo_label);
	snprintf(c->undo_key, sizeof(c->undo_key), "%s", key);
	c->undo_value = previous;
	engine.seq++;
	pthread_mutex_unlock(&engine.mutex);
	blog(LOG_INFO, "[CTD] live-source: fix applied: %s", title);
}

bool issues_apply(const char *issue_id, const char *action_id, char *message, size_t message_size, char *url,
		  size_t url_size)
{
	snprintf(message, message_size, "%s", obs_module_text("Issue.Apply.Gone"));
	if (url && url_size)
		url[0] = '\0';
	if (!engine.running || !issue_id || !action_id)
		return false;

	/* what the issue is about, copied out of the lock */
	char source[256] = "", server_id[48] = "", path[256] = "", key[32] = "";
	long long undo_value = 0;
	bool found = false, offered = false;
	pthread_mutex_lock(&engine.mutex);
	for (size_t i = 0; i < engine.conditions.num && !found; i++) {
		const struct condition *c = &engine.conditions.array[i];
		if (strcmp(c->id, issue_id) != 0)
			continue;
		found = true;
		snprintf(source, sizeof(source), "%s", c->source);
		snprintf(server_id, sizeof(server_id), "%s", c->server_id);
		snprintf(path, sizeof(path), "%s", c->path);
		snprintf(key, sizeof(key), "%s", c->undo_key);
		undo_value = c->undo_value;
		for (int a = 0; a < c->action_count; a++)
			offered |= strcmp(c->actions[a].id, action_id) == 0;
	}
	pthread_mutex_unlock(&engine.mutex);
	if (!found)
		return false;
	if (!offered) {
		snprintf(message, message_size, "%s", obs_module_text("Issue.Apply.NotOffered"));
		return false;
	}

	if (strncmp(action_id, "raise-latency:", 14) == 0) {
		const long long target = atoll(action_id + 14);
		long long previous = 0;
		if (!set_source_int(source, "latency_ms", target, &previous)) {
			snprintf(message, message_size, obs_module_text("Issue.Apply.SourceGone"), source);
			return false;
		}
		struct dstr title = {0}, undo = {0};
		dstr_printf(&title, obs_module_text("Issue.Fix.Latency"), source, target, previous);
		dstr_printf(&undo, obs_module_text("Issue.Fix.LatencyUndo"), previous);
		record_fix(source, server_id, path, title.array, undo.array, "latency_ms", previous);
		snprintf(message, message_size, "%s", title.array);
		dstr_free(&title);
		dstr_free(&undo);
		return true;
	}
	if (strcmp(action_id, "hardware-decode") == 0) {
		long long previous = 0;
		if (!set_source_int(source, "decoder", 0 /* Auto */, &previous)) {
			snprintf(message, message_size, obs_module_text("Issue.Apply.SourceGone"), source);
			return false;
		}
		struct dstr title = {0};
		dstr_printf(&title, obs_module_text("Issue.Fix.HardwareDecode"), source);
		record_fix(source, server_id, path, title.array, obs_module_text("Issue.Fix.HardwareDecodeUndo"),
			   "decoder", previous);
		snprintf(message, message_size, "%s", title.array);
		dstr_free(&title);
		return true;
	}
	if (strncmp(action_id, "channels:", 9) == 0) {
		const long long channels = atoll(action_id + 9);
		long long previous = 0;
		if (!set_source_int(source, "audio_channels", channels, &previous)) {
			snprintf(message, message_size, obs_module_text("Issue.Apply.SourceGone"), source);
			return false;
		}
		struct dstr title = {0};
		dstr_printf(&title,
			    obs_module_text(channels == 1 ? "Issue.Fix.ChannelsLeft" : "Issue.Fix.ChannelsRight"),
			    source);
		record_fix(source, server_id, path, title.array, obs_module_text("Issue.Fix.ChannelsUndo"),
			   "audio_channels", previous);
		snprintf(message, message_size, "%s", title.array);
		dstr_free(&title);
		return true;
	}
	if (strncmp(action_id, "timecode:", 9) == 0) {
		const long long channel = atoll(action_id + 9);
		long long previous = 0;
		if (channel < 1 || channel > 8 || !set_source_int(source, "timecode_input", channel, &previous)) {
			snprintf(message, message_size, obs_module_text("Issue.Apply.SourceGone"), source);
			return false;
		}
		struct dstr title = {0};
		dstr_printf(&title, obs_module_text("Issue.Fix.Timecode"), source, channel);
		record_fix(source, server_id, path, title.array, obs_module_text("Issue.Fix.TimecodeUndo"),
			   "timecode_input", previous);
		snprintf(message, message_size, "%s", title.array);
		dstr_free(&title);
		return true;
	}
	if (strcmp(action_id, "undo") == 0) {
		if (!set_source_int(source, key, undo_value, NULL)) {
			snprintf(message, message_size, obs_module_text("Issue.Apply.SourceGone"), source);
			return false;
		}
		pthread_mutex_lock(&engine.mutex);
		for (size_t i = 0; i < engine.conditions.num; i++) {
			if (strcmp(engine.conditions.array[i].id, issue_id) == 0) {
				snprintf(message, message_size, obs_module_text("Issue.Apply.Undone"),
					 engine.conditions.array[i].title);
				free_condition(&engine.conditions.array[i]);
				da_erase(engine.conditions, i);
				engine.seq++;
				break;
			}
		}
		pthread_mutex_unlock(&engine.mutex);
		blog(LOG_INFO, "[CTD] live-source: %s", message);
		return true;
	}

	/* the rest need the server profile */
	struct ctd_server s;
	if (!servers_get(server_id, &s)) {
		snprintf(message, message_size, "%s", obs_module_text("Issue.Apply.ProfileGone"));
		return false;
	}
	bool ok = false;
	if (strcmp(action_id, "copy-url") == 0) {
		if (url && url_size)
			snprintf(url, url_size, "srt://%s:%d?streamid=publish:%s", s.host, s.srt_port, path);
		snprintf(message, message_size, obs_module_text("Issue.Apply.UrlReady"), path);
		ok = true;
	} else if (strcmp(action_id, "kick") == 0) {
		ok = issues_kick(s.id, path, message, message_size);
	}
	ctd_server_free(&s);
	return ok;
}

bool issues_kick(const char *server_id, const char *path, char *message, size_t message_size)
{
	struct ctd_server s;
	if (!servers_get(server_id, &s)) {
		snprintf(message, message_size, "%s", obs_module_text("Issue.Apply.ProfileGone"));
		return false;
	}
	bool ok = false;
	/* H8: only on a profile that allows changing the server; the publisher from its poller */
	char api_host[256];
	int api_port = 0;
	if (!s.allow_writes || !ctd_server_api_address(&s, api_host, sizeof(api_host), &api_port)) {
		snprintf(message, message_size, "%s", obs_module_text("Issue.Apply.NoKick"));
	} else {
		struct mtx_poller *poller = mtx_poller_acquire(s.id);
		struct mtx_poll_snapshot snap;
		mtx_poller_snapshot(poller, &snap);
		char type[32] = "", id[64] = "";
		for (size_t i = 0; i < snap.count; i++) {
			if (strcmp(snap.items[i].name, path) == 0) {
				snprintf(type, sizeof(type), "%s", snap.items[i].publisher);
				snprintf(id, sizeof(id), "%s", snap.items[i].publisher_id);
			}
		}
		mtx_poll_snapshot_free(&snap);
		const struct mtx_api_server api = {api_host, api_port, s.api_user, s.api_pass, NULL, NULL, s.api_token};
		const enum mtx_api_status status = *id ? mtx_api_kick(&api, type, id) : MTX_API_NOT_FOUND;
		mtx_poller_refresh(poller);
		mtx_poller_release(poller);
		ok = status == MTX_API_OK;
		snprintf(message, message_size,
			 obs_module_text(ok    ? "Issue.Apply.Kicked"
					 : *id ? "Issue.Apply.KickFailed"
					       : "Issue.Apply.NothingPublishing"),
			 path);
		blog(ok ? LOG_INFO : LOG_WARNING, "[CTD] live-source: %s", message);
	}
	ctd_server_free(&s);
	return ok;
}

void issue_list_free(struct issue_list *list)
{
	for (size_t i = 0; i < list->count; i++) {
		struct issue *x = &list->items[i];
		bfree(x->id);
		bfree(x->rule);
		bfree(x->source);
		bfree(x->server_id);
		bfree(x->path);
		bfree(x->title);
		bfree(x->cause);
	}
	bfree(list->items);
	memset(list, 0, sizeof(*list));
}
