/*
 * CTD Live Source: the OBS source (SPEC 10.1, E3) around the engine (src/engine/engine.h).
 *
 * The engine does everything from connecting to timed, decoded frames; this file turns its callbacks into OBS frames,
 * audio and the signal-loss picture, runs it on a thread, and owns the settings, the properties window, the MediaMTX
 * stream list and the stats text. A change to the stream's settings stops the engine and starts a new one; the loss
 * settings and soft correction apply to the running engine.
 */

#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <util/dstr.h>

#include <libavutil/hwcontext.h>

#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../engine/engine.h"
#include "../api/mediamtx-api.h"
#include "../api/poller.h"
#include "../multistream.h"
#include "loss-card.h"
#include "loss-video.h"
#include "issues.h"
#include "servers.h"
#include "sync-groups.h"
#include "ws-vendor.h"

#define S_SERVER        "server" /* a server profile's id (H4); empty: the address fields below */
#define S_HOST          "host"
#define S_PORT          "port"
#define S_PATH          "path"
#define S_LATENCY_MS    "latency_ms"
#define S_DRIFT_CAP_MS  "drift_cap_ms"
#define S_RECONNECT_S   "reconnect_s"
#define S_STALL_S       "stall_s"
#define S_API_PORT      "api_port"
#define S_API_USER      "api_user"
#define S_API_PASS      "api_pass"
#define S_ALLOW_EDITS   "allow_path_edits"
#define S_NEW_PATH      "new_path"
#define S_NEW_PATH_SLATE "new_path_always_available"
#define S_LOSS_MODE     "loss_mode"
#define S_HOLD_MS       "hold_ms"
#define S_DECODER       "decoder"
#define S_SOFT_CORRECTION "soft_correction"
#define S_LOSS_IMAGE    "loss_image"
#define S_DISCONNECT_INACTIVE "disconnect_inactive" /* H17: off by default: sources keep running when not shown */
#define S_AUTO_FIX "auto_fix" /* D5: off by default: apply the safe fixes for this source's issues by itself */
/* section 7, audio repair */
#define S_CHANNELS "audio_channels"        /* A1: enum ctd_channels */
#define S_FILL_GAPS "fill_audio_gaps"      /* A2: on by default */
#define S_LOCK_AUDIO "lock_audio_to_video" /* A3 */
#define S_RATE_OVERRIDE "audio_rate_override" /* A4: 0 = as labelled */
/* section 8.1, delay */
#define S_VIDEO_DELAY_MS "video_delay_ms" /* S1: 0 to 2000 */
#define S_AUDIO_DELAY_MS "audio_delay_ms"
#define S_DELAY_CAP_MB "delay_memory_mb" /* S2: the most the delayed pictures may hold */
#define S_TIMECODE "timecode_input" /* S4: enum ctd_timecode_source */
#define S_SYNC_GROUP "sync_group"     /* S9: a name; empty for none */
#define S_VIDEO_ONLY "video_only"          /* A6 */
#define S_LAST_WIDTH    "last_width" /* not shown: the feed's last picture size, for the card after a restart */
#define S_LAST_HEIGHT   "last_height"

#define CARD_TITLE_LOST    "SIGNAL LOST * RECONNECTING"
#define CARD_TITLE_WAITING "WAITING FOR SIGNAL"
/* The card is a single frame, and OBS can lose it: a canvas that wasn't drawing yet when it was sent (an Aitum Vertical
 * canvas at load) never gets it. So it is sent again this often while it is up. */
#define CARD_RESEND_NS (2ull * 1000000000ull)

/* the stream list is re-fetched when the properties are opened and it is older than this */
#define API_FRESH_NS   (3ull * 1000000000ull)
#define API_TIMEOUT_NS (4ull * 1000000000ull)
/* deleting a path takes a second click within this long */
#define DELETE_CONFIRM_NS (5ull * 1000000000ull)

enum path_op {
	PATH_OP_NONE,
	PATH_OP_CREATE,
	PATH_OP_DELETE,
};

struct ctd_source {
	obs_source_t *source;
	bool audio_only; /* CTD Live Audio: never decodes or outputs video */

	/* settings snapshot, guarded by mutex; with a server profile, the address and credentials are the profile's */
	pthread_mutex_t settings_mutex;
	char *server_id; /* the profile in use, or empty for the source's own address fields */
	char *host;
	int port;
	char *read_user; /* H2: from the profile; never logged */
	char *read_pass;
	char *passphrase;
	char *api_host; /* the profile's API may live elsewhere than its SRT */
	char *api_token;
	struct mtx_poller *poller; /* H15: the profile's shared poller, kept running while this source uses it */
	char *path;
	int latency_ms;
	int drift_cap_ms;
	int reconnect_s;
	int stall_s;
	int api_port;
	char *api_user;
	char *api_pass;
	int decoder;     /* enum ctd_decoder_mode */
	bool video_only; /* A6 */
	char *loss_image;
	bool card_dirty; /* the image or the name changed: draw the card again */
	/* the source's name, for the log (R18) and the card; follows renames. Its own lock, so logging never waits on
	 * the settings. */
	pthread_mutex_t name_mutex;
	char *name;
	/* the settings that apply to a running engine, kept for the next one */
	volatile long loss_mode; /* enum ctd_loss_mode */
	volatile long hold_ms;
	volatile bool soft_correction;     /* SPEC R5 */
	volatile bool disconnect_inactive; /* H17 */
	/* section 7: audio repair that applies to a running engine */
	volatile long channels;
	volatile bool fill_gaps;
	volatile bool lock_audio;
	volatile long rate_override;
	/* section 8.1: delay, also without reconnecting */
	volatile long video_delay_ms;
	volatile long audio_delay_ms;
	volatile long delay_cap_mb;
	volatile long timecode_source; /* section 8.2 */
	/* section 8.3: the sync group; the manager (sync-groups.c) reads and sets these through the proc handler */
	char *sync_group;            /* under settings_mutex */
	volatile long sync_delay_ms; /* the group delay it holds, kept across engine restarts (S13) */
	volatile long sync_state;    /* enum sync_state, or -1 in no group */
	volatile long sync_offset_x10;
	volatile bool sync_reference;
	/* H17: starting and stopping the worker happens from the UI thread (settings) and the graphics thread
	 * (activate/deactivate); paused says the engine was stopped because the source isn't in any active scene */
	pthread_mutex_t worker_mutex;
	bool paused;

	/* the engine and the thread running it; the pointer is guarded by engine_mutex for readers on other threads */
	pthread_mutex_t engine_mutex;
	struct ctd_engine *engine;
	pthread_t thread;
	bool thread_running;

	/* the loss picture, drawn on the engine's thread */
	bool card_waiting;     /* the card says "waiting" rather than "lost" */
	uint64_t card_sent_ns; /* when the built-in card last went to OBS (0: not up) */
	/* the last picture size, so the card fills the same space; saved with the source so it survives restarts */
	volatile long video_cx;
	volatile long video_cy;
	struct loss_card card;
	struct loss_video *card_video; /* plays a video card on its own thread; NULL for audio-only sources */

	/* what the feed carries, for the stats text (R16) */
	pthread_mutex_t input_mutex;
	struct ctd_input input;

	/* OBS events for CTD Relay (API v1.4), on the engine's thread */
	enum ctd_feed_state last_state;
	uint64_t left_live_ns;  /* when the feed stopped being LIVE (0: it hasn't) */
	uint64_t stats_sent_ns; /* the last obs.feed.stats */

	/* MediaMTX stream list, fetched on its own thread so the API can never stall playback or the UI */
	pthread_mutex_t api_mutex;
	struct mtx_path_list api_paths;
	enum mtx_api_status api_status;
	uint64_t api_fetched_ns;
	bool api_fetching;
	os_event_t *api_wake; /* auto-reset: one fetch per signal, plus one more if signalled mid-fetch */
	pthread_t api_thread;
	bool api_thread_running;
	volatile bool destroying;

	/* what the API thread last logged, so the log shows changes only */
	enum mtx_api_status api_logged_status;
	size_t api_logged_total;
	size_t api_logged_live;

	/* a path create/delete asked for in the properties, carried out on the API thread (guarded by api_mutex) */
	enum path_op pending_op;
	char *pending_op_path;
	bool pending_op_slate;
	char *path_op_message;   /* outcome of the last create/delete, shown in the properties */
	char *delete_armed_path; /* waiting for the confirming second click */
	uint64_t delete_armed_ns;
};

/* --------------------------------------------------------------- logging -- */

/* Every line names its source, "[ctd-live:Cam 1] ..." (SPEC R18), so a show's log can be read with many feeds. */
#ifdef __GNUC__
__attribute__((format(printf, 3, 4)))
#endif
static void
ctd_log(struct ctd_source *ctx, int level, const char *format, ...)
{
	char name[128];
	pthread_mutex_lock(&ctx->name_mutex);
	snprintf(name, sizeof(name), "%s", ctx->name ? ctx->name : "");
	pthread_mutex_unlock(&ctx->name_mutex);

	struct dstr message = {0};
	va_list args;
	va_start(args, format);
	dstr_vprintf(&message, format, args);
	va_end(args);
	blog(level, "[ctd-live:%s] %s", name, message.array ? message.array : "");
	dstr_free(&message);
}

static char *source_name(struct ctd_source *ctx)
{
	pthread_mutex_lock(&ctx->name_mutex);
	char *name = bstrdup(ctx->name ? ctx->name : "");
	pthread_mutex_unlock(&ctx->name_mutex);
	return name;
}

/* The engine, if one is running, for a quick call from any thread. */
#define WITH_ENGINE(ctx, call)                          \
	do {                                            \
		pthread_mutex_lock(&(ctx)->engine_mutex); \
		if ((ctx)->engine)                      \
			call;                           \
		pthread_mutex_unlock(&(ctx)->engine_mutex); \
	} while (0)

/* ------------------------------------------------------------ signal loss -- */

static void show_card(struct ctd_source *ctx, bool waiting)
{
	char *name = source_name(ctx);
	pthread_mutex_lock(&ctx->settings_mutex);
	char *image = bstrdup(ctx->loss_image ? ctx->loss_image : "");
	const bool dirty = ctx->card_dirty;
	ctx->card_dirty = false;
	pthread_mutex_unlock(&ctx->settings_mutex);

	/* the size of the feed's own picture, so the scene layout doesn't move; before any picture, the canvas's */
	uint32_t cx = (uint32_t)os_atomic_load_long(&ctx->video_cx);
	uint32_t cy = (uint32_t)os_atomic_load_long(&ctx->video_cy);
	if (!cx || !cy) {
		struct obs_video_info ovi;
		const bool have_video = obs_get_video_info(&ovi);
		cx = have_video && ovi.base_width ? ovi.base_width : 1920;
		cy = have_video && ovi.base_height ? ovi.base_height : 1080;
	}

	ctx->card_sent_ns = 0;
	if (loss_video_is_video_file(image)) {
		if (loss_video_play(ctx->card_video, image, cx, cy, name)) {
			/* the video's own thread keeps sending frames: nothing to resend */
			bfree(name);
			bfree(image);
			return;
		}
		/* it can't be played (and said why): the built-in card, drawn afresh */
		image[0] = '\0';
		loss_card_free(&ctx->card);
	} else {
		loss_video_stop(ctx->card_video);
	}

	if (dirty || !ctx->card.pixels || ctx->card.width != cx || ctx->card.height != cy ||
	    ctx->card_waiting != waiting) {
		loss_card_build(&ctx->card, cx, cy, waiting ? CARD_TITLE_WAITING : CARD_TITLE_LOST, name, image);
		ctx->card_waiting = waiting;
	}
	bfree(name);
	bfree(image);
	if (!ctx->card.pixels)
		return;

	struct obs_source_frame frame = {0};
	frame.format = VIDEO_FORMAT_BGRA;
	frame.width = ctx->card.width;
	frame.height = ctx->card.height;
	frame.data[0] = ctx->card.pixels;
	frame.linesize[0] = ctx->card.width * 4;
	frame.full_range = true;
	frame.timestamp = os_gettime_ns();
	obs_source_output_video(ctx->source, &frame);
	ctx->card_sent_ns = frame.timestamp;
}

/* ------------------------------------------------------- engine callbacks -- */

static uint64_t engine_now(void)
{
	return os_gettime_ns();
}

static void engine_log(void *opaque, enum ctd_log_level level, const char *message)
{
	static const int levels[] = {LOG_ERROR, LOG_WARNING, LOG_INFO, LOG_DEBUG};
	ctd_log(opaque, level >= CTD_LOG_ERROR && level <= CTD_LOG_DEBUG ? levels[level] : LOG_INFO, "%s", message);
}

static enum video_format obs_format_from_av(enum AVPixelFormat format)
{
	switch (format) {
	case AV_PIX_FMT_YUV420P:
	case AV_PIX_FMT_YUVJ420P:
		return VIDEO_FORMAT_I420;
	case AV_PIX_FMT_NV12:
		return VIDEO_FORMAT_NV12;
	case AV_PIX_FMT_P010LE:
		return VIDEO_FORMAT_P010; /* 10-bit video copied back from the graphics card */
	case AV_PIX_FMT_YUV422P:
	case AV_PIX_FMT_YUVJ422P:
		return VIDEO_FORMAT_I422;
	case AV_PIX_FMT_YUV444P:
	case AV_PIX_FMT_YUVJ444P:
		return VIDEO_FORMAT_I444;
	case AV_PIX_FMT_BGRA:
		return VIDEO_FORMAT_BGRA;
	default:
		return VIDEO_FORMAT_NONE;
	}
}

static bool engine_video(void *opaque, const AVFrame *picture, uint64_t timestamp_ns)
{
	struct ctd_source *ctx = opaque;
	const enum video_format format = obs_format_from_av(picture->format);
	if (format == VIDEO_FORMAT_NONE)
		return false;

	struct obs_source_frame out = {0};
	out.format = format;
	out.width = (uint32_t)picture->width;
	out.height = (uint32_t)picture->height;
	out.timestamp = timestamp_ns;
	for (int p = 0; p < AV_NUM_DATA_POINTERS; p++) {
		out.data[p] = picture->data[p];
		out.linesize[p] = (uint32_t)abs(picture->linesize[p]);
	}
	const bool full = picture->color_range == AVCOL_RANGE_JPEG;
	video_format_get_parameters_for_format(VIDEO_CS_DEFAULT, full ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL, format,
					       out.color_matrix, out.color_range_min, out.color_range_max);
	out.full_range = full;
	/* the picture is back: no card video frame may land on top of it, and the card isn't resent */
	loss_video_stop(ctx->card_video);
	ctx->card_sent_ns = 0;
	obs_source_output_video(ctx->source, &out);
	os_atomic_set_long(&ctx->video_cx, (long)out.width);
	os_atomic_set_long(&ctx->video_cy, (long)out.height);
	return true;
}

static void engine_audio(void *opaque, const float *const planes[2], int frames, uint64_t timestamp_ns)
{
	struct ctd_source *ctx = opaque;
	struct obs_source_audio out = {0};
	out.data[0] = (const uint8_t *)planes[0];
	out.data[1] = (const uint8_t *)planes[1];
	out.frames = (uint32_t)frames;
	out.speakers = SPEAKERS_STEREO;
	out.format = AUDIO_FORMAT_FLOAT_PLANAR;
	out.samples_per_sec = 48000;
	out.timestamp = timestamp_ns;
	obs_source_output_audio(ctx->source, &out);
}

/* SPEC R11: the engine says the hold time has passed (or the mode changed, or the card needs drawing again) */
static void engine_loss(void *opaque, enum ctd_loss_mode mode, bool waiting)
{
	struct ctd_source *ctx = opaque;
	ctx->card_sent_ns = 0;
	switch (mode) {
	case CTD_LOSS_TRANSPARENT:
		loss_video_stop(ctx->card_video);
		obs_source_output_video(ctx->source, NULL);
		break;
	case CTD_LOSS_HOLD:
		loss_video_stop(ctx->card_video);
		break; /* the last frame stays up */
	default:
		show_card(ctx, waiting);
		break;
	}
}

/* the source as CTD Relay's obs.feed.* events name it: {name, path, server} */
static obs_data_t *event_source(struct ctd_source *ctx, bool full)
{
	obs_data_t *source = obs_data_create();
	char *name = source_name(ctx);
	obs_data_set_string(source, "name", name);
	bfree(name);
	if (full) {
		pthread_mutex_lock(&ctx->settings_mutex);
		obs_data_set_string(source, "path", ctx->path ? ctx->path : "");
		obs_data_set_string(source, "server", ctx->host ? ctx->host : "");
		pthread_mutex_unlock(&ctx->settings_mutex);
	}
	return source;
}

/* API v1.4 obs.feed.stats, every 5 s while LIVE */
#define FEED_STATS_EVERY_NS (5ull * 1000000000ull)

static void post_feed_stats(struct ctd_source *ctx)
{
	struct ctd_engine_stats st;
	ctd_engine_get_stats(ctx->engine, &st); /* the engine's own thread: it can't be going away */
	pthread_mutex_lock(&ctx->input_mutex);
	const struct ctd_input in = ctx->input;
	pthread_mutex_unlock(&ctx->input_mutex);
	const char *device = av_hwdevice_get_type_name((enum AVHWDeviceType)st.video_device);

	obs_data_t *data = obs_data_create();
	obs_data_t *source = event_source(ctx, false);
	obs_data_set_obj(data, "source", source);
	obs_data_release(source);
	obs_data_set_double(data, "fps", st.fps_x100 / 100.0);
	obs_data_set_int(data, "width", in.width);
	obs_data_set_int(data, "height", in.height);
	obs_data_set_int(data, "latenessMs", st.lateness_ms);
	obs_data_set_int(data, "resyncs", st.resyncs);
	obs_data_set_int(data, "reconnects", st.reconnects);
	obs_data_set_string(data, "decoder", ctx->audio_only ? "none" : device ? device : "cpu");
	ms_post_event("obs.feed.stats", data);
	obs_data_release(data);
}

static void engine_tick(void *opaque)
{
	struct ctd_source *ctx = opaque;
	const uint64_t now = os_gettime_ns();
	if (ctx->card_sent_ns && now - ctx->card_sent_ns > CARD_RESEND_NS)
		show_card(ctx, ctx->card_waiting); /* the same picture again, already built */
	if (ctx->last_state == CTD_FEED_LIVE && now - ctx->stats_sent_ns >= FEED_STATS_EVERY_NS) {
		ctx->stats_sent_ns = now;
		post_feed_stats(ctx);
	}
}

/* API v1.4 obs.feed.state: what CTD Live mobile shows as "signal lost" and "back live" */
static void engine_state(void *opaque, enum ctd_feed_state state)
{
	struct ctd_source *ctx = opaque;
	const uint64_t now = os_gettime_ns();
	const enum ctd_feed_state previous = ctx->last_state;
	ctx->last_state = state;

	obs_data_t *data = obs_data_create();
	obs_data_t *source = event_source(ctx, true);
	obs_data_set_obj(data, "source", source);
	obs_data_release(source);
	obs_data_set_string(data, "state", ctd_feed_state_name(state));
	if (previous != state)
		obs_data_set_string(data, "previous", ctd_feed_state_name(previous));
	if (state == CTD_FEED_LIVE) {
		if (ctx->left_live_ns)
			obs_data_set_int(data, "gapMs", (long long)((now - ctx->left_live_ns) / 1000000));
		ctx->left_live_ns = 0;
		ctx->stats_sent_ns = now; /* the first stats once fps and size have been measured */
	} else if (previous == CTD_FEED_LIVE) {
		ctx->left_live_ns = now;
	}
	/* CONNECTING → CONNECTING again on a new engine (a settings change) still says where it reads from */
	ms_post_event("obs.feed.state", data);
	obs_data_release(data);

	/* I3: the same for obs-websocket clients */
	obs_data_t *ws = obs_data_create();
	char *name = source_name(ctx);
	obs_data_set_string(ws, "sourceName", name);
	bfree(name);
	pthread_mutex_lock(&ctx->settings_mutex);
	obs_data_set_string(ws, "path", ctx->path ? ctx->path : "");
	obs_data_set_string(ws, "serverId", ctx->server_id ? ctx->server_id : ""); /* I4: routes the webhook */
	pthread_mutex_unlock(&ctx->settings_mutex);
	obs_data_set_string(ws, "state", ctd_feed_state_name(state));
	obs_data_set_string(ws, "previous", ctd_feed_state_name(previous));
	ws_vendor_emit("FeedStateChanged", ws);
	obs_data_release(ws);
}

static void engine_input(void *opaque, const struct ctd_input *input)
{
	struct ctd_source *ctx = opaque;
	pthread_mutex_lock(&ctx->input_mutex);
	ctx->input = *input;
	pthread_mutex_unlock(&ctx->input_mutex);
}

/* ---------------------------------------------------------------- worker --- */

static void *worker_thread(void *opaque)
{
	os_set_thread_name("ctd-live-source");
	ctd_engine_run(opaque);
	return NULL;
}

static void stop_worker(struct ctd_source *ctx)
{
	if (!ctx->thread_running)
		return;
	pthread_mutex_lock(&ctx->engine_mutex);
	struct ctd_engine *engine = ctx->engine;
	pthread_mutex_unlock(&ctx->engine_mutex);
	ctd_engine_stop(engine);
	pthread_join(ctx->thread, NULL);
	ctx->thread_running = false;
	pthread_mutex_lock(&ctx->engine_mutex);
	ctx->engine = NULL;
	pthread_mutex_unlock(&ctx->engine_mutex);
	ctd_engine_destroy(engine);
}

static void start_worker(struct ctd_source *ctx)
{
	if (ctx->thread_running || ctx->paused)
		return;

	/* the graphics card OBS itself draws on */
	char adapter[16] = "";
	struct obs_video_info ovi;
	if (obs_get_video_info(&ovi))
		snprintf(adapter, sizeof(adapter), "%u", ovi.adapter);

	pthread_mutex_lock(&ctx->settings_mutex);
	const struct ctd_engine_config config = {
		.host = ctx->host,
		.port = ctx->port,
		.path = ctx->path,
		.latency_ms = ctx->latency_ms,
		.drift_cap_ms = ctx->drift_cap_ms,
		.reconnect_s = ctx->reconnect_s,
		.stall_s = ctx->stall_s,
		.decoder = (enum ctd_decoder_mode)ctx->decoder,
		.gpu_adapter = adapter,
		.read_user = ctx->read_user,
		.read_pass = ctx->read_pass,
		.passphrase = ctx->passphrase,
		.audio_only = ctx->audio_only,
		.no_audio = ctx->video_only,
	};
	const struct ctd_engine_callbacks callbacks = {
		.opaque = ctx,
		.now_ns = engine_now,
		.video = engine_video,
		.audio = engine_audio,
		.loss = engine_loss,
		.input = engine_input,
		.log = engine_log,
		.tick = engine_tick,
		.state = engine_state,
	};
	struct ctd_engine *engine = ctd_engine_create(&config, &callbacks);
	pthread_mutex_unlock(&ctx->settings_mutex);
	if (!engine)
		return;
	ctd_engine_set_loss(engine, (enum ctd_loss_mode)os_atomic_load_long(&ctx->loss_mode),
			    (int)os_atomic_load_long(&ctx->hold_ms));
	ctd_engine_set_soft_correction(engine, os_atomic_load_bool(&ctx->soft_correction));
	ctd_engine_set_audio_repair(engine, (enum ctd_channels)os_atomic_load_long(&ctx->channels),
				    os_atomic_load_bool(&ctx->fill_gaps), os_atomic_load_bool(&ctx->lock_audio),
				    (int)os_atomic_load_long(&ctx->rate_override));
	ctd_engine_set_timecode(engine, (int)os_atomic_load_long(&ctx->timecode_source));
	ctd_engine_set_group_delay(engine, (int)os_atomic_load_long(&ctx->sync_delay_ms), false); /* S13 */
	ctd_engine_set_delay(engine, (int)os_atomic_load_long(&ctx->video_delay_ms),
			     (int)os_atomic_load_long(&ctx->audio_delay_ms),
			     (int)os_atomic_load_long(&ctx->delay_cap_mb));

	pthread_mutex_lock(&ctx->engine_mutex);
	ctx->engine = engine;
	pthread_mutex_unlock(&ctx->engine_mutex);
	if (pthread_create(&ctx->thread, NULL, worker_thread, engine) == 0) {
		ctx->thread_running = true;
	} else {
		pthread_mutex_lock(&ctx->engine_mutex);
		ctx->engine = NULL;
		pthread_mutex_unlock(&ctx->engine_mutex);
		ctd_engine_destroy(engine);
	}
}

/* ------------------------------------------------------- MediaMTX API --- */

struct api_interrupt {
	struct ctd_source *ctx;
	uint64_t deadline_ns;
};

static int api_interrupt_cb(void *opaque)
{
	struct api_interrupt *ai = opaque;
	return os_atomic_load_bool(&ai->ctx->destroying) || os_gettime_ns() > ai->deadline_ns;
}

static size_t count_live(const struct mtx_path_list *list)
{
	size_t live = 0;
	for (size_t i = 0; i < list->count; i++)
		live += list->items[i].live;
	return live;
}

static void log_api_result(struct ctd_source *ctx, const char *host, int port, enum mtx_api_status status,
			   const struct mtx_path_list *list)
{
	const size_t live = count_live(list);
	if (status == ctx->api_logged_status && list->count == ctx->api_logged_total && live == ctx->api_logged_live)
		return;
	ctx->api_logged_status = status;
	ctx->api_logged_total = list->count;
	ctx->api_logged_live = live;

	switch (status) {
	case MTX_API_OK:
		ctd_log(ctx, LOG_INFO, "MediaMTX API %s:%d: %zu streams, %zu live", host, port, list->count, live);
		break;
	case MTX_API_UNAUTHORIZED:
		ctd_log(ctx, LOG_WARNING, "MediaMTX API %s:%d refused the login", host, port);
		break;
	case MTX_API_NOT_FOUND:
		ctd_log(ctx, LOG_WARNING, "MediaMTX API %s:%d: no v3 API there", host, port);
		break;
	case MTX_API_BAD_RESPONSE:
		ctd_log(ctx, LOG_WARNING, "MediaMTX API %s:%d: unexpected reply", host, port);
		break;
	default:
		ctd_log(ctx, LOG_INFO, "MediaMTX API %s:%d unreachable", host, port);
		break;
	}
}

/* Creates or deletes a path. Returns the outcome in words for the properties (bmalloc'd), and logs it. */
static char *run_path_op(struct ctd_source *ctx, const struct mtx_api_server *server, enum path_op op, const char *path,
			 bool slate)
{
	const bool create = op == PATH_OP_CREATE;
	const enum mtx_api_status status = create ? mtx_api_add_path(server, path, slate)
						  : mtx_api_delete_path(server, path);
	struct dstr message = {0};
	switch (status) {
	case MTX_API_OK:
		if (create)
			dstr_printf(&message, "Created \"%s\" on the server. It lasts until MediaMTX restarts.", path);
		else
			dstr_printf(&message, "Deleted \"%s\" from the server.", path);
		break;
	case MTX_API_REJECTED:
		if (create)
			dstr_printf(&message, "The server refused \"%s\": the name is taken or not allowed.", path);
		else
			dstr_printf(&message, "The server refused to delete \"%s\".", path);
		break;
	case MTX_API_NOT_FOUND:
		if (create)
			dstr_copy(&message, "No MediaMTX API answered on that port.");
		else
			dstr_printf(&message,
				    "\"%s\" isn't in the server's config: a path that only exists while something "
				    "is publishing to it can't be deleted.",
				    path);
		break;
	case MTX_API_UNAUTHORIZED:
		dstr_copy(&message, "The server refused: this API login can't change its config.");
		break;
	default:
		dstr_copy(&message, "Couldn't reach the MediaMTX API.");
		break;
	}

	if (status == MTX_API_OK)
		ctd_log(ctx, LOG_INFO, "MediaMTX %s:%d: %s path '%s'", server->host, server->port,
			create ? "created" : "deleted", path);
	else
		ctd_log(ctx, LOG_WARNING, "MediaMTX %s:%d: couldn't %s path '%s'", server->host, server->port,
			create ? "create" : "delete", path);
	return message.array;
}

static void *api_thread(void *opaque)
{
	struct ctd_source *ctx = opaque;
	os_set_thread_name("ctd-live-api");

	for (;;) {
		os_event_wait(ctx->api_wake);
		if (os_atomic_load_bool(&ctx->destroying))
			return NULL;

		pthread_mutex_lock(&ctx->api_mutex);
		ctx->api_fetching = true;
		const enum path_op op = ctx->pending_op;
		char *op_path = ctx->pending_op_path;
		const bool op_slate = ctx->pending_op_slate;
		ctx->pending_op = PATH_OP_NONE;
		ctx->pending_op_path = NULL;
		pthread_mutex_unlock(&ctx->api_mutex);

		pthread_mutex_lock(&ctx->settings_mutex);
		char *host = bstrdup(ctx->api_host && *ctx->api_host ? ctx->api_host : "127.0.0.1");
		const int port = ctx->api_port;
		char *user = bstrdup(ctx->api_user ? ctx->api_user : "");
		char *pass = bstrdup(ctx->api_pass ? ctx->api_pass : "");
		char *token = bstrdup(ctx->api_token ? ctx->api_token : "");
		char *server_id = bstrdup(ctx->server_id ? ctx->server_id : "");
		pthread_mutex_unlock(&ctx->settings_mutex);
		/* H15: with a profile, the stream list comes from its shared poller (a reference of this thread's own, so a
		 * settings change can't free it underneath) */
		struct mtx_poller *poller = mtx_poller_acquire(server_id);
		bfree(server_id);

		struct api_interrupt ai = {ctx, os_gettime_ns() + API_TIMEOUT_NS};
		const struct mtx_api_server server = {host, port, user, pass, api_interrupt_cb, &ai, token};

		if (op != PATH_OP_NONE) {
			char *message = run_path_op(ctx, &server, op, op_path, op_slate);
			bfree(op_path);
			pthread_mutex_lock(&ctx->api_mutex);
			bfree(ctx->path_op_message);
			ctx->path_op_message = message;
			pthread_mutex_unlock(&ctx->api_mutex);
			ai.deadline_ns = os_gettime_ns() + API_TIMEOUT_NS;
		}

		/* then the list, so the dropdown shows the change */
		struct mtx_path_list list = {0};
		enum mtx_api_status status = MTX_API_UNKNOWN;
		if (poller) {
			if (op != PATH_OP_NONE) {
				mtx_poller_refresh(poller); /* the change shows at the poller's next look */
				os_sleep_ms(300);
			}
			struct mtx_poll_snapshot snapshot;
			mtx_poller_snapshot(poller, &snapshot);
			if (snapshot.updated_ns) {
				status = snapshot.status;
				list.items = snapshot.count ? bzalloc(snapshot.count * sizeof(*list.items)) : NULL;
				list.count = snapshot.count;
				for (size_t i = 0; i < snapshot.count; i++) {
					list.items[i].name = bstrdup(snapshot.items[i].name);
					list.items[i].live = snapshot.items[i].live;
				}
			}
			mtx_poll_snapshot_free(&snapshot);
			mtx_poller_release(poller);
		}
		/* without a profile, or before its poller's first look: ask the API directly (port 0: no API) */
		if (status == MTX_API_UNKNOWN) {
			status = port ? mtx_api_list_paths(&server, &list) : MTX_API_NOT_FOUND;
			if (port)
				log_api_result(ctx, host, port, status, &list);
		}
		bfree(host);
		bfree(user);
		bfree(pass);
		bfree(token);

		pthread_mutex_lock(&ctx->api_mutex);
		struct mtx_path_list old = ctx->api_paths;
		ctx->api_paths = list;
		ctx->api_status = status;
		ctx->api_fetched_ns = os_gettime_ns();
		ctx->api_fetching = false;
		pthread_mutex_unlock(&ctx->api_mutex);

		mtx_path_list_free(&old);
		/* reloads an open properties window; no lock is held, so its handlers may call back into this source */
		if (!os_atomic_load_bool(&ctx->destroying))
			obs_source_update_properties(ctx->source);
	}
}

/* Fetches the stream list in the background. Without force, only when the last list is stale. */
static void request_api_refresh(struct ctd_source *ctx, bool force)
{
	pthread_mutex_lock(&ctx->api_mutex);
	const bool stale = !ctx->api_fetched_ns || os_gettime_ns() - ctx->api_fetched_ns > API_FRESH_NS;
	const bool wake = force || (stale && !ctx->api_fetching);
	pthread_mutex_unlock(&ctx->api_mutex);
	if (wake)
		os_event_signal(ctx->api_wake);
}

static void start_api_thread(struct ctd_source *ctx)
{
	pthread_mutex_init(&ctx->api_mutex, NULL);
	os_event_init(&ctx->api_wake, OS_EVENT_TYPE_AUTO);
	ctx->api_thread_running = pthread_create(&ctx->api_thread, NULL, api_thread, ctx) == 0;
}

static void stop_api_thread(struct ctd_source *ctx)
{
	os_atomic_set_bool(&ctx->destroying, true); /* also aborts a request in flight */
	if (ctx->api_thread_running) {
		os_event_signal(ctx->api_wake);
		pthread_join(ctx->api_thread, NULL);
	}
	os_event_destroy(ctx->api_wake);
	pthread_mutex_destroy(&ctx->api_mutex);
	mtx_path_list_free(&ctx->api_paths);
	bfree(ctx->pending_op_path);
	bfree(ctx->path_op_message);
	bfree(ctx->delete_armed_path);
}

static bool api_refresh_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	request_api_refresh(data, true);
	return false; /* the fetch reloads the properties when it finishes */
}

/* --------------------------------------------------- path create/delete --- */

/* A string setting as currently typed in the properties, trimmed. NULL when empty. */
static char *current_setting(struct ctd_source *ctx, const char *key)
{
	obs_data_t *settings = obs_source_get_settings(ctx->source);
	struct dstr value = {0};
	dstr_copy(&value, obs_data_get_string(settings, key));
	dstr_depad(&value);
	obs_data_release(settings);
	return value.array;
}

static void set_path_op_message(struct ctd_source *ctx, char *message)
{
	pthread_mutex_lock(&ctx->api_mutex);
	bfree(ctx->path_op_message);
	ctx->path_op_message = message;
	pthread_mutex_unlock(&ctx->api_mutex);
}

/* Writes to the server are opt-in; this also covers presses that don't come from the window (obs-websocket). */
static bool path_edits_allowed(struct ctd_source *ctx)
{
	obs_data_t *settings = obs_source_get_settings(ctx->source);
	const bool allowed = obs_data_get_bool(settings, S_ALLOW_EDITS);
	obs_data_release(settings);
	if (!allowed)
		set_path_op_message(ctx, bstrdup("Turn on \"Allow this source to create and delete paths\" first."));
	return allowed;
}

/* Hands a create/delete to the API thread; takes ownership of path. */
static void queue_path_op(struct ctd_source *ctx, enum path_op op, char *path, bool slate)
{
	struct dstr message = {0};
	dstr_printf(&message, op == PATH_OP_CREATE ? "Creating \"%s\"…" : "Deleting \"%s\"…", path);

	pthread_mutex_lock(&ctx->api_mutex);
	bfree(ctx->pending_op_path);
	ctx->pending_op = op;
	ctx->pending_op_path = path;
	ctx->pending_op_slate = slate;
	bfree(ctx->path_op_message);
	ctx->path_op_message = message.array;
	pthread_mutex_unlock(&ctx->api_mutex);
	os_event_signal(ctx->api_wake);
}

static bool create_path_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	struct ctd_source *ctx = data;
	if (!path_edits_allowed(ctx))
		return true;

	char *path = current_setting(ctx, S_NEW_PATH);
	if (!mtx_path_name_valid(path)) {
		bfree(path);
		set_path_op_message(ctx, bstrdup("Type a name for the new path: letters, digits and _ - . ~ / only."));
		return true;
	}
	obs_data_t *settings = obs_source_get_settings(ctx->source);
	const bool slate = obs_data_get_bool(settings, S_NEW_PATH_SLATE);
	obs_data_release(settings);

	queue_path_op(ctx, PATH_OP_CREATE, path, slate);
	return true; /* shows "Creating…"; the API thread reloads the properties when it's done */
}

/* Deletes the path picked in the Stream list. The first click arms it; a second click within 5 s confirms. */
static bool delete_path_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	struct ctd_source *ctx = data;
	if (!path_edits_allowed(ctx))
		return true;

	char *path = current_setting(ctx, S_PATH);
	if (!mtx_path_name_valid(path)) {
		bfree(path);
		set_path_op_message(ctx, bstrdup("Pick the path to delete in the Stream list first."));
		return true;
	}

	pthread_mutex_lock(&ctx->api_mutex);
	const bool confirmed = ctx->delete_armed_path && strcmp(ctx->delete_armed_path, path) == 0 &&
			       os_gettime_ns() - ctx->delete_armed_ns < DELETE_CONFIRM_NS;
	bfree(ctx->delete_armed_path);
	ctx->delete_armed_path = confirmed ? NULL : bstrdup(path);
	ctx->delete_armed_ns = os_gettime_ns();
	pthread_mutex_unlock(&ctx->api_mutex);

	if (confirmed) {
		queue_path_op(ctx, PATH_OP_DELETE, path, false);
	} else {
		struct dstr message = {0};
		dstr_printf(&message, "Click again within 5 seconds to delete \"%s\" from the server.", path);
		set_path_op_message(ctx, message.array);
		bfree(path);
	}
	return true;
}

static const char *const path_edit_properties[] = {S_NEW_PATH, S_NEW_PATH_SLATE, "create_path", "delete_path",
						   "path_op_status"};

static void show_path_edits(obs_properties_t *props, bool visible)
{
	for (size_t i = 0; i < sizeof(path_edit_properties) / sizeof(*path_edit_properties); i++) {
		obs_property_t *property = obs_properties_get(props, path_edit_properties[i]);
		if (property)
			obs_property_set_visible(property, visible);
	}
}

static bool allow_edits_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);
	show_path_edits(props, obs_data_get_bool(settings, S_ALLOW_EDITS));
	return true;
}

/* Fills the stream dropdown and returns the status line under it. Call with api_mutex held. */
static void describe_api_paths(struct ctd_source *ctx, obs_property_t *list, int api_port, struct dstr *status)
{
	for (size_t i = 0; i < ctx->api_paths.count; i++)
		obs_property_list_add_string(list, ctx->api_paths.items[i].name, ctx->api_paths.items[i].name);

	switch (ctx->api_status) {
	case MTX_API_UNKNOWN:
		dstr_copy(status, "Looking for streams on the server…");
		break;
	case MTX_API_OK: {
		const size_t live = count_live(&ctx->api_paths);
		if (!ctx->api_paths.count) {
			dstr_copy(status, "No streams on the server yet");
			break;
		}
		dstr_printf(status, "Live now: %s", live ? "" : "none");
		for (size_t i = 0; i < live; i++)
			dstr_catf(status, "%s%s", i ? ", " : "", ctx->api_paths.items[i].name);
		if (ctx->api_paths.count > live)
			dstr_catf(status, " · %zu more not live", ctx->api_paths.count - live);
		break;
	}
	case MTX_API_UNAUTHORIZED:
		dstr_copy(status, "The MediaMTX API needs a login: fill in the API username and password below");
		break;
	case MTX_API_NOT_FOUND:
		if (api_port)
			dstr_printf(status, "No MediaMTX API on port %d (MediaMTX 1.0 or newer is needed)", api_port);
		else
			dstr_copy(status,
				  "This server profile has no API URL, so there's no stream list (Manage servers…)");
		break;
	case MTX_API_REJECTED:
	case MTX_API_BAD_RESPONSE:
		dstr_copy(status, "The MediaMTX API sent an unexpected reply");
		break;
	default:
		dstr_printf(status, "Can't reach the MediaMTX API on port %d (turn it on with \"api: yes\")", api_port);
		break;
	}
}

/* ------------------------------------------------------------------ stats -- */

/*
 * The stats text in the properties (SPEC R16), for example:
 *   LIVE · video decoded on d3d12va
 *   Input: h264 1920x1080 at 29.97 fps · aac 48 kHz, 2 channels
 *   Lateness 12 ms (snaps to live past 700 ms) · resyncs 0 · reconnects 1
 *   Frames: video 51234 · audio 98765
 */
static void describe_stats(struct ctd_source *ctx, struct dstr *stats)
{
	struct ctd_engine_stats st = {0};
	bool running = false;
	pthread_mutex_lock(&ctx->engine_mutex);
	if (ctx->engine) {
		ctd_engine_get_stats(ctx->engine, &st);
		running = true;
	}
	pthread_mutex_unlock(&ctx->engine_mutex);

	dstr_copy(stats, running ? ctd_feed_state_name(st.state) : "STOPPED");

	/* H14: the profile's poller can tell when the path is gone from the server (the source keeps retrying) */
	pthread_mutex_lock(&ctx->settings_mutex);
	char *server_id = bstrdup(ctx->server_id ? ctx->server_id : "");
	char *path = bstrdup(ctx->path ? ctx->path : "");
	pthread_mutex_unlock(&ctx->settings_mutex);
	struct mtx_poller *poller = mtx_poller_acquire(server_id);
	if (poller) {
		struct mtx_poll_snapshot snapshot;
		mtx_poller_snapshot(poller, &snapshot);
		bool listed = false;
		for (size_t i = 0; i < snapshot.count && !listed; i++)
			listed = strcmp(snapshot.items[i].name, path) == 0;
		if (snapshot.status == MTX_API_OK && *path && !listed)
			dstr_cat(stats, " · Path not on server");
		mtx_poll_snapshot_free(&snapshot);
		mtx_poller_release(poller);
	}
	/* D2: this source's issues, and its path's */
	struct issue_list issues;
	issues_get(&issues);
	char *name = source_name(ctx);
	bool listed_issue = false;
	for (size_t i = 0; i < issues.count; i++) {
		const struct issue *x = &issues.items[i];
		const bool mine = *x->source ? strcmp(x->source, name) == 0
					     : *path && strcmp(x->path, path) == 0 &&
						       (!*server_id || strcmp(x->server_id, server_id) == 0);
		if (!mine)
			continue;
		dstr_catf(stats, "%s%s", listed_issue ? "; " : "\nIssues: ", x->title);
		listed_issue = true;
	}
	bfree(name);
	issue_list_free(&issues);
	bfree(server_id);
	bfree(path);
	if (!ctx->audio_only) {
		const char *device = av_hwdevice_get_type_name((enum AVHWDeviceType)st.video_device);
		dstr_catf(stats, " · video decoded on %s", device ? device : "the CPU");
	}

	pthread_mutex_lock(&ctx->input_mutex);
	const struct ctd_input in = ctx->input;
	pthread_mutex_unlock(&ctx->input_mutex);
	dstr_cat(stats, "\nInput: ");
	bool any = false;
	if (!ctx->audio_only && *in.video_codec) {
		dstr_cat(stats, in.video_codec);
		if (in.width && in.height)
			dstr_catf(stats, " %dx%d at %ld.%02ld fps", in.width, in.height, st.fps_x100 / 100,
				  st.fps_x100 % 100);
		any = true;
	}
	if (*in.audio_codec) {
		dstr_catf(stats, "%s%s", any ? " · " : "", in.audio_codec);
		if (in.sample_rate)
			dstr_catf(stats, " %g kHz, %d channel%s", in.sample_rate / 1000.0, in.channels,
				  in.channels == 1 ? "" : "s");
		any = true;
	}
	if (!any)
		dstr_cat(stats, "nothing yet");

	pthread_mutex_lock(&ctx->settings_mutex);
	const int drift_cap_ms = ctx->drift_cap_ms;
	pthread_mutex_unlock(&ctx->settings_mutex);
	/* R6: lateness, target and the soft correction being applied */
	dstr_catf(stats, "\nLateness %ld ms", st.lateness_ms);
	if (st.target_set) {
		const long ppm = st.correction_ppm;
		dstr_catf(stats, ", target %ld ms, correcting %s%ld.%02ld%%", st.target_ms, ppm < 0 ? "-" : "+",
			  labs(ppm) / 10000, labs(ppm) % 10000 / 100);
	}
	dstr_catf(stats, " (snaps to live past %d ms)", drift_cap_ms);
	dstr_catf(stats, "\nResyncs %ld · discontinuities %ld · reconnects %ld", st.resyncs, st.discontinuities,
		  st.reconnects);
	if (ctx->audio_only)
		dstr_catf(stats, "\nFrames: audio %ld", st.audio_frames);
	else
		dstr_catf(stats, "\nFrames: video %ld · audio %ld", st.video_frames, st.audio_frames);
	if (st.gaps_filled)
		dstr_catf(stats, "\nAudio gaps filled %ld (%ld ms)", st.gaps_filled, st.gap_ms_filled);
	if (st.audio_lock_ppm)
		dstr_catf(stats, "\nAudio locked to video, correcting %+ld ppm", st.audio_lock_ppm);
	/* S5: the timecode, or why there is none */
	const struct ctd_timecode *tc = &st.timecode;
	const long timecode = os_atomic_load_long(&ctx->timecode_source);
	if (tc->locked) {
		dstr_catf(stats, "\nTimecode %02d:%02d:%02d%c%02d", tc->hours, tc->minutes, tc->seconds,
			  tc->drop_frame ? ';' : ':', tc->frames);
		if (tc->fps > 0)
			dstr_catf(stats, " at %.3g fps%s", tc->fps, tc->drop_frame ? " drop frame" : "");
		if (timecode != CTD_TIMECODE_SEI)
			dstr_catf(stats, " · level %.0f dB · %.2f%% lost", tc->level_db, tc->error_percent);
		if (tc->user_bits)
			dstr_catf(stats, " · user bits %08X", (unsigned)tc->user_bits);
	} else if (timecode == CTD_TIMECODE_SEI) {
		dstr_cat(stats, "\nTimecode: none in the video yet");
	} else if (timecode) {
		dstr_catf(stats, "\nTimecode: no LTC on channel %ld (level %.0f dB)", timecode, tc->level_db);
	} else if (tc->detected_channel) {
		dstr_catf(stats, "\nLTC found on channel %d: set it as the timecode input to keep it out of the mix",
			  tc->detected_channel);
	}
	/* S15: the sync group */
	const long sync_state = os_atomic_load_long(&ctx->sync_state);
	if (sync_state >= 0) {
		pthread_mutex_lock(&ctx->settings_mutex);
		dstr_catf(stats, "\nSync group \"%s\": %s", ctx->sync_group ? ctx->sync_group : "",
			  sync_state_name((enum sync_state)sync_state));
		pthread_mutex_unlock(&ctx->settings_mutex);
		if (os_atomic_load_bool(&ctx->sync_reference))
			dstr_cat(stats, " (the reference)");
		dstr_catf(stats, " · offset %.0f ms · group delay %ld ms",
			  os_atomic_load_long(&ctx->sync_offset_x10) / 10.0, os_atomic_load_long(&ctx->sync_delay_ms));
	}
	/* S3 */
	if (st.video_delay_ms || st.audio_delay_ms || st.video_delay_target_ms || st.audio_delay_target_ms) {
		dstr_catf(stats, "\nDelay: video %ld ms, audio %ld ms", st.video_delay_ms, st.audio_delay_ms);
		const bool limited = st.video_delay_limit_ms && st.video_delay_target_ms > st.video_delay_limit_ms;
		const long video_to = limited ? st.video_delay_limit_ms : st.video_delay_target_ms;
		if (labs(st.video_delay_ms - video_to) > 20 || st.audio_delay_ms != st.audio_delay_target_ms)
			dstr_catf(stats, " (moving to %ld and %ld)", video_to, st.audio_delay_target_ms);
		dstr_catf(stats, " · holding %.0f MB", st.delay_memory_mb);
		if (limited)
			dstr_catf(stats, "\n⚠ Only %ld ms of video delay fits in the memory set at this resolution",
				  st.video_delay_limit_ms);
	}
}

static bool stats_refresh_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	UNUSED_PARAMETER(data);
	return true; /* rebuilds the properties, stats included */
}

/* SPEC R17: drops the connection and dials again at once, without waiting out the reconnect delay. */
static bool reconnect_now_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	struct ctd_source *ctx = data;
	ctd_log(ctx, LOG_INFO, "reconnect asked for");
	WITH_ENGINE(ctx, ctd_engine_reconnect_now(ctx->engine));
	return false;
}

/* A5: rebuilds the audio decoder and resampler without touching the video */
static bool reset_audio_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	struct ctd_source *ctx = data;
	ctd_log(ctx, LOG_INFO, "audio reset asked for");
	WITH_ENGINE(ctx, ctd_engine_reset_audio(ctx->engine));
	return false;
}

/* section 7: audio repair (A1 to A6) */
static void add_audio_settings(obs_properties_t *props, struct ctd_source *ctx)
{
	obs_properties_t *audio = obs_properties_create();
	obs_property_t *channels =
		obs_properties_add_list(audio, S_CHANNELS, "Channels", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(channels, "Stereo (as sent)", CTD_CHANNELS_STEREO);
	obs_property_list_add_int(channels, "Left channel on both sides", CTD_CHANNELS_LEFT);
	obs_property_list_add_int(channels, "Right channel on both sides", CTD_CHANNELS_RIGHT);
	obs_property_list_add_int(channels, "Mono (both mixed)", CTD_CHANNELS_MONO);
	obs_property_list_add_int(channels, "Swap left and right", CTD_CHANNELS_SWAP);
	obs_property_list_add_int(channels, "Invert the right channel's polarity", CTD_CHANNELS_INVERT_RIGHT);
	obs_properties_add_bool(audio, S_FILL_GAPS, "Fill short gaps in the audio with silence (no clicks)");
	obs_properties_add_bool(audio, S_LOCK_AUDIO,
				"Lock audio to video (nudges only the audio when it drifts from the picture)");
	obs_property_t *rate = obs_properties_add_list(audio, S_RATE_OVERRIDE, "Incoming sample rate",
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(rate, "As the stream says", 0);
	obs_property_list_add_int(rate, "Treat as 44.1 kHz", 44100);
	obs_property_list_add_int(rate, "Treat as 48 kHz", 48000);
	if (!ctx || !ctx->audio_only)
		obs_properties_add_bool(audio, S_VIDEO_ONLY, "Video only (ignore the stream's audio)");
	if (ctx)
		obs_properties_add_button2(audio, "reset_audio", "Reset audio", reset_audio_clicked, ctx);
	obs_properties_add_group(props, "audio_repair", "Audio repair", OBS_GROUP_NORMAL, audio);
}

/* S16: the delays that undo the sync check's offset; false if there is no answer to apply */
static bool apply_sync_check(struct ctd_source *ctx)
{
	struct ctd_engine_stats st;
	bool have = false;
	pthread_mutex_lock(&ctx->engine_mutex);
	if (ctx->engine) {
		ctd_engine_get_stats(ctx->engine, &st);
		have = st.sync_check.state == CTD_SYNC_CHECK_DONE;
	}
	pthread_mutex_unlock(&ctx->engine_mutex);
	if (!have)
		return false;
	/* sound late: hold the picture back by as much; sound early: hold the sound back */
	const long offset = lround(st.sync_check.offset_ms);
	const long video = offset > 0 ? (offset > 2000 ? 2000 : offset) : 0;
	const long audio = offset < 0 ? (-offset > 2000 ? 2000 : -offset) : 0;
	obs_data_t *changes = obs_data_create();
	obs_data_set_int(changes, S_VIDEO_DELAY_MS, ctx->audio_only ? 0 : video);
	obs_data_set_int(changes, S_AUDIO_DELAY_MS, audio);
	obs_source_update(ctx->source, changes);
	obs_data_release(changes);
	ctd_log(ctx, LOG_INFO, "sync check applied: video delay %ld ms, audio delay %ld ms", video, audio);
	return true;
}

static bool sync_check_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	struct ctd_source *ctx = data;
	WITH_ENGINE(ctx, ctd_engine_start_sync_check(ctx->engine));
	return true;
}

static bool sync_check_apply_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	return apply_sync_check(data);
}

/* S16: the sync check, with its answer and the fix */
static void add_sync_check(obs_properties_t *props, struct ctd_source *ctx)
{
	if (!ctx || ctx->audio_only)
		return; /* it needs the picture */
	obs_properties_t *check = obs_properties_create();
	obs_properties_add_text(check, "sync_check_how",
				"Play the sync clip (CTD Hub ▸ Sync clip, a white flash and a beep once a second) on a "
				"phone in front of the camera, then start the check: it watches and listens for 10 s.",
				OBS_TEXT_INFO);
	obs_properties_add_button2(check, "sync_check_start", "Start the sync check", sync_check_clicked, ctx);
	struct ctd_engine_stats st = {0};
	pthread_mutex_lock(&ctx->engine_mutex);
	if (ctx->engine)
		ctd_engine_get_stats(ctx->engine, &st);
	pthread_mutex_unlock(&ctx->engine_mutex);
	char result[256] = "";
	switch (st.sync_check.state) {
	case CTD_SYNC_CHECK_RUNNING:
		snprintf(result, sizeof(result), "Checking… (press Refresh stats in 10 s)");
		break;
	case CTD_SYNC_CHECK_FAILED:
		snprintf(result, sizeof(result),
			 "Only %d flash and beep pairs: keep the clip in view, the sound up, and try again.",
			 st.sync_check.pairs);
		break;
	case CTD_SYNC_CHECK_DONE:
		if (fabs(st.sync_check.offset_ms) < 20)
			snprintf(result, sizeof(result), "In sync: the sound and the picture are within %.0f ms.",
				 fabs(st.sync_check.offset_ms));
		else
			snprintf(result, sizeof(result), "The sound comes %.0f ms %s the picture (%d pairs).",
				 fabs(st.sync_check.offset_ms), st.sync_check.offset_ms > 0 ? "after" : "before",
				 st.sync_check.pairs);
		break;
	default:
		break;
	}
	if (*result)
		obs_properties_add_text(check, "sync_check_result", result, OBS_TEXT_INFO);
	if (st.sync_check.state == CTD_SYNC_CHECK_DONE && fabs(st.sync_check.offset_ms) >= 20)
		obs_properties_add_button2(check, "sync_check_apply",
					   st.sync_check.offset_ms > 0 ? "Delay the video to match"
								       : "Delay the audio to match",
					   sync_check_apply_clicked, ctx);
	obs_properties_add_group(props, "sync_check", "Sync check", OBS_GROUP_NORMAL, check);
}

/* section 8.2: timecode input (S4) */
static void add_timecode_settings(obs_properties_t *props, struct ctd_source *ctx)
{
	obs_property_t *list =
		obs_properties_add_list(props, S_TIMECODE, "Timecode input", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(list, "None", CTD_TIMECODE_NONE);
	obs_property_list_add_int(list, "LTC on the left channel (taken out of the program audio)",
				  CTD_TIMECODE_LTC_LEFT);
	obs_property_list_add_int(list, "LTC on the right channel (taken out of the program audio)",
				  CTD_TIMECODE_LTC_RIGHT);
	for (int channel = 3; channel <= 8; channel++) {
		char label[64];
		snprintf(label, sizeof(label), "LTC on channel %d", channel);
		obs_property_list_add_int(list, label, channel);
	}
	if (!ctx || !ctx->audio_only)
		obs_property_list_add_int(list, "Timecode in the video (H.264/HEVC SEI)", CTD_TIMECODE_SEI);
	obs_property_set_long_description(list,
					  "Timecode lines this feed up with others in a sync group. LTC is a loud "
					  "tone, so its channel never reaches the program audio.");
	obs_property_t *group = obs_properties_add_text(props, S_SYNC_GROUP, "Sync group", OBS_TEXT_DEFAULT);
	obs_property_set_long_description(group,
					  "Sources with the same group name and working timecode are lined up, "
					  "so the same moment is on screen and in the mix at once. Empty for none.");
}

/* section 8.1: manual delay (S1 to S3) */
static void add_delay_settings(obs_properties_t *props, struct ctd_source *ctx)
{
	obs_properties_t *delay = obs_properties_create();
	if (!ctx || !ctx->audio_only)
		obs_properties_add_int_slider(delay, S_VIDEO_DELAY_MS,
					      "Delay video by (ms): when the sound comes first", 0, 2000, 10);
	obs_properties_add_int_slider(delay, S_AUDIO_DELAY_MS, "Delay audio by (ms): when the lips come first", 0, 2000,
				      10);
	if (!ctx || !ctx->audio_only)
		obs_properties_add_int(delay, S_DELAY_CAP_MB, "Memory for delayed video (MB)", 64, 4096, 64);
	obs_properties_add_group(props, "delay", "Delay (changes apply smoothly, without a jump)", OBS_GROUP_NORMAL,
				 delay);
}

/* -------------------------------------------------------------- callbacks -- */

static const char *ctd_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "CTD Live Source (SRT)";
}

static const char *ctd_audio_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "CTD Live Audio (SRT)";
}

static bool string_changed(const char *old, const char *new_value)
{
	return !old || strcmp(old, new_value) != 0;
}

static void replace_string(char **field, const char *value)
{
	bfree(*field);
	*field = bstrdup(value);
}

/*
 * Copies the settings in. Returns true when anything the stream connection depends on changed; *api_changed says
 * whether the stream list needs fetching again.
 */
static bool apply_settings(struct ctd_source *ctx, obs_data_t *settings, bool *api_changed)
{
	const char *host = obs_data_get_string(settings, S_HOST);
	const char *path = obs_data_get_string(settings, S_PATH);
	const char *api_user = obs_data_get_string(settings, S_API_USER);
	const char *api_pass = obs_data_get_string(settings, S_API_PASS);
	const int port = (int)obs_data_get_int(settings, S_PORT);
	const int latency_ms = (int)obs_data_get_int(settings, S_LATENCY_MS);
	const int drift_cap_ms = (int)obs_data_get_int(settings, S_DRIFT_CAP_MS);
	const int reconnect_s = (int)obs_data_get_int(settings, S_RECONNECT_S);
	const int stall_s = (int)obs_data_get_int(settings, S_STALL_S);
	const int api_port = (int)obs_data_get_int(settings, S_API_PORT);
	const int decoder = (int)obs_data_get_int(settings, S_DECODER);
	const bool video_only = !ctx->audio_only && obs_data_get_bool(settings, S_VIDEO_ONLY);
	const char *loss_image = obs_data_get_string(settings, S_LOSS_IMAGE);

	/* the loss settings apply at the next check of the signal, without reconnecting */
	os_atomic_set_long(&ctx->loss_mode, (long)obs_data_get_int(settings, S_LOSS_MODE));
	os_atomic_set_long(&ctx->hold_ms, (long)obs_data_get_int(settings, S_HOLD_MS));
	os_atomic_set_bool(&ctx->soft_correction, obs_data_get_bool(settings, S_SOFT_CORRECTION));
	os_atomic_set_bool(&ctx->disconnect_inactive, obs_data_get_bool(settings, S_DISCONNECT_INACTIVE));
	/* section 7: applies without reconnecting */
	os_atomic_set_long(&ctx->channels, (long)obs_data_get_int(settings, S_CHANNELS));
	os_atomic_set_bool(&ctx->fill_gaps, obs_data_get_bool(settings, S_FILL_GAPS));
	os_atomic_set_bool(&ctx->lock_audio, obs_data_get_bool(settings, S_LOCK_AUDIO));
	os_atomic_set_long(&ctx->rate_override, (long)obs_data_get_int(settings, S_RATE_OVERRIDE));
	WITH_ENGINE(ctx, ctd_engine_set_audio_repair(
				 ctx->engine, (enum ctd_channels)os_atomic_load_long(&ctx->channels),
				 os_atomic_load_bool(&ctx->fill_gaps), os_atomic_load_bool(&ctx->lock_audio),
				 (int)os_atomic_load_long(&ctx->rate_override)));
	os_atomic_set_long(&ctx->video_delay_ms,
			   ctx->audio_only ? 0 : (long)obs_data_get_int(settings, S_VIDEO_DELAY_MS));
	os_atomic_set_long(&ctx->audio_delay_ms, (long)obs_data_get_int(settings, S_AUDIO_DELAY_MS));
	os_atomic_set_long(&ctx->delay_cap_mb, (long)obs_data_get_int(settings, S_DELAY_CAP_MB));
	long timecode = (long)obs_data_get_int(settings, S_TIMECODE);
	if (ctx->audio_only && timecode == CTD_TIMECODE_SEI)
		timecode = CTD_TIMECODE_NONE; /* no pictures, no SEI */
	os_atomic_set_long(&ctx->timecode_source, timecode);
	WITH_ENGINE(ctx, ctd_engine_set_timecode(ctx->engine, (int)os_atomic_load_long(&ctx->timecode_source)));
	/* S9: the group (the manager picks it up at its next round) */
	pthread_mutex_lock(&ctx->settings_mutex);
	replace_string(&ctx->sync_group, obs_data_get_string(settings, S_SYNC_GROUP));
	pthread_mutex_unlock(&ctx->settings_mutex);
	WITH_ENGINE(ctx, ctd_engine_set_delay(ctx->engine, (int)os_atomic_load_long(&ctx->video_delay_ms),
					      (int)os_atomic_load_long(&ctx->audio_delay_ms),
					      (int)os_atomic_load_long(&ctx->delay_cap_mb)));
	WITH_ENGINE(ctx, ctd_engine_set_loss(ctx->engine, (enum ctd_loss_mode)os_atomic_load_long(&ctx->loss_mode),
					     (int)os_atomic_load_long(&ctx->hold_ms)));
	WITH_ENGINE(ctx, ctd_engine_set_soft_correction(ctx->engine, os_atomic_load_bool(&ctx->soft_correction)));

	/* H4: a server profile supplies the address, the API and the credentials (one that was removed: the default
	 * profile); without one, the source's own fields, as before profiles existed */
	const char *server_id = obs_data_get_string(settings, S_SERVER);
	struct ctd_server profile = {0};
	const bool use_profile = *server_id && (servers_get(server_id, &profile) || servers_get(NULL, &profile));
	const char *srt_host = host, *api_host = host, *api_token = "";
	const char *read_user = "", *read_pass = "", *passphrase = "";
	int srt_port = port, api_port_used = api_port;
	char profile_api_host[256] = "";
	if (use_profile) {
		srt_host = profile.host;
		srt_port = profile.srt_port;
		if (!ctd_server_api_address(&profile, profile_api_host, sizeof(profile_api_host), &api_port_used))
			api_port_used = 0; /* the profile has no API */
		api_host = profile_api_host;
		api_user = profile.api_user;
		api_pass = profile.api_pass;
		api_token = profile.api_token;
		read_user = profile.read_user;
		read_pass = profile.read_pass;
		passphrase = profile.passphrase;
	}

	pthread_mutex_lock(&ctx->settings_mutex);
	const bool image_changed = string_changed(ctx->loss_image, loss_image);
	if (image_changed)
		ctx->card_dirty = true;
	replace_string(&ctx->loss_image, loss_image);
	const bool connection_changed =
		string_changed(ctx->host, srt_host) || string_changed(ctx->path, path) || ctx->port != srt_port ||
		string_changed(ctx->read_user, read_user) || string_changed(ctx->read_pass, read_pass) ||
		string_changed(ctx->passphrase, passphrase) || ctx->latency_ms != latency_ms ||
		ctx->drift_cap_ms != drift_cap_ms || ctx->reconnect_s != reconnect_s || ctx->stall_s != stall_s ||
		ctx->decoder != decoder || ctx->video_only != video_only;
	*api_changed = string_changed(ctx->api_host, api_host) || ctx->api_port != api_port_used ||
		       string_changed(ctx->api_user, api_user) || string_changed(ctx->api_pass, api_pass) ||
		       string_changed(ctx->api_token, api_token);

	struct mtx_poller *old_poller = NULL;
	if (string_changed(ctx->server_id, use_profile ? profile.id : "")) {
		old_poller = ctx->poller;
		ctx->poller = use_profile ? mtx_poller_acquire(profile.id) : NULL;
	}
	replace_string(&ctx->server_id, use_profile ? profile.id : "");
	replace_string(&ctx->host, srt_host);
	replace_string(&ctx->path, path);
	replace_string(&ctx->read_user, read_user);
	replace_string(&ctx->read_pass, read_pass);
	replace_string(&ctx->passphrase, passphrase);
	replace_string(&ctx->api_host, api_host);
	replace_string(&ctx->api_user, api_user);
	replace_string(&ctx->api_pass, api_pass);
	replace_string(&ctx->api_token, api_token);
	ctx->port = srt_port;
	ctx->latency_ms = latency_ms;
	ctx->drift_cap_ms = drift_cap_ms;
	ctx->reconnect_s = reconnect_s;
	ctx->stall_s = stall_s;
	ctx->decoder = decoder;
	ctx->video_only = video_only;
	ctx->api_port = api_port_used;
	pthread_mutex_unlock(&ctx->settings_mutex);
	ctd_server_free(&profile);
	mtx_poller_release(old_poller); /* outside the lock: the last one out stops its thread */
	if (image_changed)
		WITH_ENGINE(ctx, ctd_engine_redraw_loss(ctx->engine));
	return connection_changed;
}

static void ctd_update(void *data, obs_data_t *settings)
{
	struct ctd_source *ctx = data;
	bool api_changed;
	/* only a change to the stream itself reconnects; the API settings never touch playback */
	const bool restart = apply_settings(ctx, settings, &api_changed);
	pthread_mutex_lock(&ctx->worker_mutex);
	/* H17 turned off while disconnected: connect again */
	const bool resume = ctx->paused && !os_atomic_load_bool(&ctx->disconnect_inactive);
	if (resume)
		ctx->paused = false;
	if (restart || resume) {
		stop_worker(ctx);
		start_worker(ctx);
	}
	pthread_mutex_unlock(&ctx->worker_mutex);
	if (api_changed)
		request_api_refresh(ctx, true);
}

/* H17, opt-in: not in any active scene, the source lets its connection go. The engine is only told to stop here (this
 * is OBS's graphics thread, which mustn't wait); its thread ends by itself and is joined at the next start. */
static void ctd_deactivate(void *data)
{
	struct ctd_source *ctx = data;
	if (!os_atomic_load_bool(&ctx->disconnect_inactive))
		return;
	pthread_mutex_lock(&ctx->worker_mutex);
	if (!ctx->paused) {
		ctx->paused = true;
		WITH_ENGINE(ctx, ctd_engine_stop(ctx->engine));
		ctd_log(ctx, LOG_INFO,
			"not in any active scene: disconnected (\"Disconnect when not in an active scene\")");
		obs_data_t *ws = obs_data_create();
		char *name = source_name(ctx);
		obs_data_set_string(ws, "sourceName", name);
		bfree(name);
		obs_data_set_string(ws, "state", "DISCONNECTED");
		obs_data_set_string(ws, "previous", ctd_feed_state_name(ctx->last_state));
		ws_vendor_emit("FeedStateChanged", ws);
		obs_data_release(ws);
	}
	pthread_mutex_unlock(&ctx->worker_mutex);
}

static void ctd_activate(void *data)
{
	struct ctd_source *ctx = data;
	pthread_mutex_lock(&ctx->worker_mutex);
	if (ctx->paused) {
		ctx->paused = false;
		stop_worker(ctx); /* joins the thread that already ended */
		start_worker(ctx);
		ctd_log(ctx, LOG_INFO, "in an active scene again: connecting");
	}
	pthread_mutex_unlock(&ctx->worker_mutex);
}

/* H4: a server profile was saved or removed. Applying the settings again picks up its new address or credentials and
 * reconnects only if they changed. */
static void server_changed(void *data, const char *id)
{
	UNUSED_PARAMETER(id);
	struct ctd_source *ctx = data;
	obs_data_t *settings = obs_source_get_settings(ctx->source);
	const bool uses_profile = *obs_data_get_string(settings, S_SERVER) != '\0';
	obs_data_release(settings);
	if (uses_profile)
		obs_source_update(ctx->source, NULL);
}

/* The log and the card show the source's name; other threads can't read it from OBS safely while a rename frees it. */
static void source_renamed(void *data, calldata_t *cd)
{
	struct ctd_source *ctx = data;
	pthread_mutex_lock(&ctx->name_mutex);
	replace_string(&ctx->name, calldata_string(cd, "new_name"));
	pthread_mutex_unlock(&ctx->name_mutex);
	pthread_mutex_lock(&ctx->settings_mutex);
	ctx->card_dirty = true;
	pthread_mutex_unlock(&ctx->settings_mutex);
	WITH_ENGINE(ctx, ctd_engine_redraw_loss(ctx->engine));
}

/* "void reconnect()" on the source's proc handler: the CTD Hub dock's Reconnect (R17, H11) */
static void proc_reconnect(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	struct ctd_source *ctx = data;
	ctd_log(ctx, LOG_INFO, "reconnect asked for");
	WITH_ENGINE(ctx, ctd_engine_reconnect_now(ctx->engine));
}

/* "void reset_audio()": A5 for the vendor request ResetAudio */
static void proc_reset_audio(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	struct ctd_source *ctx = data;
	ctd_log(ctx, LOG_INFO, "audio reset asked for");
	WITH_ENGINE(ctx, ctd_engine_reset_audio(ctx->engine));
}

/* "void get_sync(in ptr info)": what the sync group manager needs this round (S9) */
static void proc_get_sync(void *data, calldata_t *cd)
{
	struct ctd_source *ctx = data;
	struct ctd_sync_info *info = calldata_ptr(cd, "info");
	if (!info)
		return;
	memset(info, 0, sizeof(*info));
	pthread_mutex_lock(&ctx->settings_mutex);
	snprintf(info->group, sizeof(info->group), "%s", ctx->sync_group ? ctx->sync_group : "");
	pthread_mutex_unlock(&ctx->settings_mutex);
	info->delay_ms = (int)os_atomic_load_long(&ctx->sync_delay_ms);
	struct ctd_engine_stats st;
	bool running = false;
	pthread_mutex_lock(&ctx->engine_mutex);
	if (ctx->engine) {
		ctd_engine_get_stats(ctx->engine, &st);
		running = true;
	}
	pthread_mutex_unlock(&ctx->engine_mutex);
	if (!running)
		return;
	const struct ctd_timecode *tc = &st.timecode;
	info->engine = true;
	info->timecode = tc->locked && tc->clock_ns;
	info->pinned = tc->clock_ns != 0;
	const int base = tc->fps > 29 ? 30 : tc->fps > 24.5 ? 25 : 24;
	info->tc_seconds = tc->hours * 3600.0 + tc->minutes * 60.0 + tc->seconds + (double)tc->frames / base;
	info->pin_ns = tc->clock_ns;
	info->fps = tc->fps;
	info->settled = st.delay_settled;
	if (tc->locked)
		snprintf(info->timecode_text, sizeof(info->timecode_text), "%02d:%02d:%02d%c%02d", tc->hours,
			 tc->minutes, tc->seconds, tc->drop_frame ? ';' : ':', tc->frames);
}

/* "void set_sync(...)": the manager's word for this round: the group delay (applied when apply is set; at once when
 * jump is), and the state to show (-1: in no group) */
static void proc_set_sync(void *data, calldata_t *cd)
{
	struct ctd_source *ctx = data;
	const long delay_ms = (long)calldata_int(cd, "delay_ms");
	if (calldata_bool(cd, "apply")) {
		os_atomic_set_long(&ctx->sync_delay_ms, delay_ms);
		const bool jump = calldata_bool(cd, "jump");
		WITH_ENGINE(ctx, ctd_engine_set_group_delay(ctx->engine, (int)delay_ms, jump));
	}
	os_atomic_set_long(&ctx->sync_state, (long)calldata_int(cd, "state"));
	os_atomic_set_long(&ctx->sync_offset_x10, (long)lround(calldata_float(cd, "offset_ms") * 10.0));
	os_atomic_set_bool(&ctx->sync_reference, calldata_bool(cd, "reference"));
}

/* "void start_sync_check()" and "void apply_sync_check(out bool ok)": S16 for the vendor requests */
static void proc_start_sync_check(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	struct ctd_source *ctx = data;
	WITH_ENGINE(ctx, ctd_engine_start_sync_check(ctx->engine));
}

static void proc_apply_sync_check(void *data, calldata_t *cd)
{
	calldata_set_bool(cd, "ok", apply_sync_check(data));
}

/* "void get_feed(out string json)": the feed's state for the obs-websocket vendor (I1, I2 GetFeeds) */
static void proc_get_feed(void *data, calldata_t *cd)
{
	struct ctd_source *ctx = data;
	struct ctd_engine_stats st = {0};
	bool running = false;
	pthread_mutex_lock(&ctx->engine_mutex);
	if (ctx->engine) {
		ctd_engine_get_stats(ctx->engine, &st);
		running = true;
	}
	pthread_mutex_unlock(&ctx->engine_mutex);
	pthread_mutex_lock(&ctx->input_mutex);
	const struct ctd_input in = ctx->input;
	pthread_mutex_unlock(&ctx->input_mutex);

	obs_data_t *feed = obs_data_create();
	char *name = source_name(ctx);
	obs_data_set_string(feed, "sourceName", name);
	bfree(name);
	obs_data_set_string(feed, "kind", ctx->audio_only ? "audio" : "video");
	pthread_mutex_lock(&ctx->settings_mutex);
	obs_data_set_string(feed, "serverId", ctx->server_id ? ctx->server_id : "");
	obs_data_set_string(feed, "host", ctx->host ? ctx->host : "");
	obs_data_set_int(feed, "port", ctx->port);
	obs_data_set_string(feed, "path", ctx->path ? ctx->path : "");
	pthread_mutex_unlock(&ctx->settings_mutex);
	pthread_mutex_lock(&ctx->worker_mutex);
	const bool paused = ctx->paused;
	pthread_mutex_unlock(&ctx->worker_mutex);
	/* H17: a stopped engine still holds its last state */
	obs_data_set_string(feed, "state",
			    paused    ? "DISCONNECTED"
			    : running ? ctd_feed_state_name(st.state)
				      : "STOPPED");
	obs_data_set_int(feed, "latenessMs", st.lateness_ms);
	obs_data_set_int(feed, "correctionPpm", st.correction_ppm);
	obs_data_set_double(feed, "peakDbLeft", st.peak_db[0]);
	obs_data_set_double(feed, "peakDbRight", st.peak_db[1]);
	obs_data_set_double(feed, "clippedPercent", st.clipped_percent);
	obs_data_set_int(feed, "frozenMs", st.frozen_ms);
	obs_data_set_double(feed, "decodeMs", st.decode_ms);
	obs_data_set_double(feed, "nominalFps", st.nominal_fps);
	obs_data_set_int(feed, "decoderSetting", ctx->decoder);
	obs_data_set_int(feed, "latencyMs", ctx->latency_ms);
	obs_data_t *own = obs_source_get_settings(ctx->source);
	obs_data_set_bool(feed, "autoFix", obs_data_get_bool(own, S_AUTO_FIX));
	obs_data_set_int(feed, "audioChannels", obs_data_get_int(own, S_CHANNELS));
	obs_data_set_bool(feed, "fillGaps", obs_data_get_bool(own, S_FILL_GAPS));
	obs_data_set_bool(feed, "lockAudio", obs_data_get_bool(own, S_LOCK_AUDIO));
	obs_data_set_int(feed, "rateOverride", obs_data_get_int(own, S_RATE_OVERRIDE));
	obs_data_set_bool(feed, "videoOnly", ctx->video_only);
	obs_data_set_int(feed, "gapsFilled", st.gaps_filled);
	obs_data_set_int(feed, "gapMsFilled", st.gap_ms_filled);
	obs_data_set_int(feed, "audioLockPpm", st.audio_lock_ppm);
	/* S3: the delays, applied and set */
	obs_data_set_int(feed, "videoDelayMs", st.video_delay_ms);
	obs_data_set_int(feed, "audioDelayMs", st.audio_delay_ms);
	obs_data_set_int(feed, "videoDelaySetMs", obs_data_get_int(own, S_VIDEO_DELAY_MS));
	obs_data_set_int(feed, "audioDelaySetMs", obs_data_get_int(own, S_AUDIO_DELAY_MS));
	obs_data_set_int(feed, "videoDelayLimitMs", st.video_delay_limit_ms);
	obs_data_set_double(feed, "delayMemoryMb", st.delay_memory_mb);
	/* section 8.2 */
	const struct ctd_timecode *tc = &st.timecode;
	obs_data_set_int(feed, "timecodeSource", os_atomic_load_long(&ctx->timecode_source));
	obs_data_set_bool(feed, "timecodeLocked", tc->locked);
	char timecode[16] = "";
	if (tc->locked)
		snprintf(timecode, sizeof(timecode), "%02d:%02d:%02d%c%02d", tc->hours, tc->minutes, tc->seconds,
			 tc->drop_frame ? ';' : ':', tc->frames);
	obs_data_set_string(feed, "timecode", timecode);
	obs_data_set_double(feed, "timecodeFps", tc->fps);
	obs_data_set_bool(feed, "timecodeDrop", tc->drop_frame);
	obs_data_set_int(feed, "timecodeUserBits", tc->user_bits);
	obs_data_set_double(feed, "timecodeLevelDb", tc->level_db);
	obs_data_set_double(feed, "timecodeErrors", tc->error_percent);
	obs_data_set_int(feed, "timecodeDetectedChannel", tc->detected_channel);
	/* section 8.3 */
	pthread_mutex_lock(&ctx->settings_mutex);
	obs_data_set_string(feed, "syncGroup", ctx->sync_group ? ctx->sync_group : "");
	pthread_mutex_unlock(&ctx->settings_mutex);
	const long sync_state = os_atomic_load_long(&ctx->sync_state);
	obs_data_set_string(feed, "syncState", sync_state >= 0 ? sync_state_name((enum sync_state)sync_state) : "");
	obs_data_set_double(feed, "syncOffsetMs", os_atomic_load_long(&ctx->sync_offset_x10) / 10.0);
	obs_data_set_int(feed, "syncDelayMs", os_atomic_load_long(&ctx->sync_delay_ms));
	obs_data_set_bool(feed, "syncReference", os_atomic_load_bool(&ctx->sync_reference));
	obs_data_set_int(feed, "groupDelayMs", st.group_delay_ms);
	/* S16 */
	static const char *const check_states[] = {"idle", "running", "done", "failed"};
	obs_data_set_string(feed, "syncCheckState", check_states[st.sync_check.state & 3]);
	obs_data_set_double(feed, "syncCheckOffsetMs", st.sync_check.offset_ms);
	obs_data_set_int(feed, "syncCheckPairs", st.sync_check.pairs);
	obs_data_release(own);
	obs_data_set_int(feed, "sampleRate", in.sample_rate);
	obs_data_set_int(feed, "channels", in.channels);
	obs_data_set_double(feed, "fps", st.fps_x100 / 100.0);
	obs_data_set_int(feed, "width", in.width);
	obs_data_set_int(feed, "height", in.height);
	obs_data_set_string(feed, "videoCodec", in.video_codec);
	obs_data_set_string(feed, "audioCodec", in.audio_codec);
	obs_data_set_int(feed, "reconnects", st.reconnects);
	obs_data_set_int(feed, "resyncs", st.resyncs);
	const char *device = av_hwdevice_get_type_name((enum AVHWDeviceType)st.video_device);
	obs_data_set_string(feed, "decoder", ctx->audio_only ? "none" : device ? device : "cpu");
	calldata_set_string(cd, "json", obs_data_get_json(feed));
	obs_data_release(feed);
}

static void *create_source(obs_data_t *settings, obs_source_t *source, bool audio_only)
{
	struct ctd_source *ctx = bzalloc(sizeof(*ctx));
	ctx->source = source;
	ctx->audio_only = audio_only;
	ctx->sync_state = -1; /* in no group */
	pthread_mutex_init(&ctx->name_mutex, NULL);
	pthread_mutex_init(&ctx->input_mutex, NULL);
	pthread_mutex_init(&ctx->engine_mutex, NULL);
	pthread_mutex_init(&ctx->worker_mutex, NULL);
	ctx->name = bstrdup(obs_source_get_name(source));
	if (!audio_only)
		ctx->card_video = loss_video_create(source);
	os_atomic_set_long(&ctx->video_cx, (long)obs_data_get_int(settings, S_LAST_WIDTH));
	os_atomic_set_long(&ctx->video_cy, (long)obs_data_get_int(settings, S_LAST_HEIGHT));
	pthread_mutex_init(&ctx->settings_mutex, NULL);
	signal_handler_connect(obs_source_get_signal_handler(source), "rename", source_renamed, ctx);
	proc_handler_add(obs_source_get_proc_handler(source), "void reconnect()", proc_reconnect, ctx);
	start_api_thread(ctx); /* idle until the stream list is wanted */
	bool api_changed;
	apply_settings(ctx, settings, &api_changed);
	/* H17: a new source isn't in an active scene yet; with the option on, activation connects it */
	ctx->paused = os_atomic_load_bool(&ctx->disconnect_inactive);
	start_worker(ctx);
	servers_watch(server_changed, ctx);
	proc_handler_add(obs_source_get_proc_handler(source), "void get_feed(out string json)", proc_get_feed, ctx);
	proc_handler_add(obs_source_get_proc_handler(source), "void reset_audio()", proc_reset_audio, ctx);
	proc_handler_add(obs_source_get_proc_handler(source), "void get_sync(in ptr info)", proc_get_sync, ctx);
	proc_handler_add(obs_source_get_proc_handler(source), "void start_sync_check()", proc_start_sync_check, ctx);
	proc_handler_add(obs_source_get_proc_handler(source), "void apply_sync_check(out bool ok)",
			 proc_apply_sync_check, ctx);
	proc_handler_add(
		obs_source_get_proc_handler(source),
		"void set_sync(in int delay_ms, in bool apply, in bool jump, in int state, in float offset_ms, "
		"in bool reference)",
		proc_set_sync, ctx);
	return ctx;
}

/* Saves the picture size with the source, so after a restart the card matches the camera before it reconnects. */
static void ctd_save(void *data, obs_data_t *settings)
{
	struct ctd_source *ctx = data;
	const long cx = os_atomic_load_long(&ctx->video_cx);
	const long cy = os_atomic_load_long(&ctx->video_cy);
	if (cx && cy) {
		obs_data_set_int(settings, S_LAST_WIDTH, cx);
		obs_data_set_int(settings, S_LAST_HEIGHT, cy);
	}
}

static void *ctd_create(obs_data_t *settings, obs_source_t *source)
{
	return create_source(settings, source, false);
}

static void *ctd_audio_create(obs_data_t *settings, obs_source_t *source)
{
	return create_source(settings, source, true);
}

static void ctd_destroy(void *data)
{
	struct ctd_source *ctx = data;
	servers_unwatch(server_changed, ctx); /* waits for a profile change being applied to this source */
	signal_handler_disconnect(obs_source_get_signal_handler(ctx->source), "rename", source_renamed, ctx);
	stop_worker(ctx);
	loss_video_destroy(ctx->card_video); /* after the worker, the only one that starts it */
	stop_api_thread(ctx);
	mtx_poller_release(ctx->poller);
	pthread_mutex_destroy(&ctx->settings_mutex);
	pthread_mutex_destroy(&ctx->engine_mutex);
	pthread_mutex_destroy(&ctx->worker_mutex);
	loss_card_free(&ctx->card);
	bfree(ctx->server_id);
	bfree(ctx->host);
	bfree(ctx->path);
	bfree(ctx->sync_group);
	bfree(ctx->read_user);
	bfree(ctx->read_pass);
	bfree(ctx->passphrase);
	bfree(ctx->api_host);
	bfree(ctx->api_token);
	bfree(ctx->api_user);
	bfree(ctx->api_pass);
	bfree(ctx->loss_image);
	bfree(ctx->name);
	pthread_mutex_destroy(&ctx->name_mutex);
	pthread_mutex_destroy(&ctx->input_mutex);
	bfree(ctx);
}

/* The hold time matters unless the last frame is held for good; the image only for the card. */
static void show_loss_settings(obs_properties_t *props, int mode)
{
	obs_property_t *hold = obs_properties_get(props, S_HOLD_MS);
	obs_property_t *image = obs_properties_get(props, S_LOSS_IMAGE);
	if (hold)
		obs_property_set_visible(hold, mode != CTD_LOSS_HOLD);
	if (image)
		obs_property_set_visible(image, mode == CTD_LOSS_CARD);
}

static bool loss_mode_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);
	show_loss_settings(props, (int)obs_data_get_int(settings, S_LOSS_MODE));
	return true;
}

static void add_loss_settings(obs_properties_t *props, struct ctd_source *ctx)
{
	obs_property_t *mode = obs_properties_add_list(props, S_LOSS_MODE, "When the signal drops", OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(mode, "Hold the last frame, then show a reconnecting card", CTD_LOSS_CARD);
	obs_property_list_add_int(mode, "Hold the last frame", CTD_LOSS_HOLD);
	obs_property_list_add_int(mode, "See-through (show what's underneath in the scene)", CTD_LOSS_TRANSPARENT);
	obs_property_set_modified_callback(mode, loss_mode_modified);
	obs_properties_add_int_slider(props, S_HOLD_MS, "Hold the last frame for (ms)", 0, 10000, 100);
	obs_properties_add_path(
		props, S_LOSS_IMAGE, "Reconnecting card picture or video (optional; blank for the built-in card)",
		OBS_PATH_FILE,
		"Pictures and videos (*.png *.jpg *.jpeg *.bmp *.gif *.webp *.mp4 *.m4v *.mov *.mkv *.webm);;"
		"Pictures (*.png *.jpg *.jpeg *.bmp *.gif *.webp);;Videos (*.mp4 *.m4v *.mov *.mkv *.webm)",
		NULL);
	show_loss_settings(props, ctx ? (int)os_atomic_load_long(&ctx->loss_mode) : CTD_LOSS_CARD);
}

/* With a server profile, its address and API come from the profile: the source's own fields are hidden. */
static void show_address_fields(obs_properties_t *props, bool custom)
{
	const char *const own[] = {S_HOST, S_PORT};
	for (size_t i = 0; i < sizeof(own) / sizeof(*own); i++) {
		obs_property_t *property = obs_properties_get(props, own[i]);
		if (property)
			obs_property_set_visible(property, custom);
	}
	obs_property_t *group = obs_properties_get(props, "api");
	obs_properties_t *api = group ? obs_property_group_content(group) : NULL;
	const char *const api_own[] = {S_API_PORT, S_API_USER, S_API_PASS};
	for (size_t i = 0; api && i < sizeof(api_own) / sizeof(*api_own); i++) {
		obs_property_t *property = obs_properties_get(api, api_own[i]);
		if (property)
			obs_property_set_visible(property, custom);
	}
}

static bool server_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);
	show_address_fields(props, !*obs_data_get_string(settings, S_SERVER));
	return true;
}

static bool manage_servers_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	UNUSED_PARAMETER(data);
	ctd_servers_dialog_open(); /* modal: when it closes, the Server list is rebuilt with any new profile */
	return true;
}

static obs_properties_t *ctd_properties(void *data)
{
	struct ctd_source *ctx = data;
	obs_properties_t *props = obs_properties_create();

	/* H4: a saved server profile, or this source's own address */
	obs_property_t *server =
		obs_properties_add_list(props, S_SERVER, "Server", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(server, "Custom (enter the address below)", "");
	for (size_t i = 0; i < servers_count(); i++) {
		struct ctd_server s;
		if (!servers_get_index(i, &s))
			continue;
		struct dstr label = {0};
		dstr_printf(&label, "%s (%s)%s", s.name, s.host, s.is_default ? " · default" : "");
		obs_property_list_add_string(server, label.array, s.id);
		dstr_free(&label);
		ctd_server_free(&s);
	}
	obs_property_set_modified_callback(server, server_modified);
	obs_properties_add_button2(props, "manage_servers", "Manage servers…", manage_servers_clicked, ctx);

	obs_properties_add_text(props, S_HOST, "Server address (the machine running MediaMTX)", OBS_TEXT_DEFAULT);
	obs_properties_add_int(props, S_PORT, "SRT port", 1024, 65535, 1);

	/* editable: a path that isn't live (or listed) yet can still be typed */
	obs_property_t *paths = obs_properties_add_list(props, S_PATH, "Stream (pick one, or type a path)",
							OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);
	if (ctx) {
		pthread_mutex_lock(&ctx->settings_mutex);
		const int api_port = ctx->api_port;
		pthread_mutex_unlock(&ctx->settings_mutex);

		struct dstr status = {0};
		pthread_mutex_lock(&ctx->api_mutex);
		describe_api_paths(ctx, paths, api_port, &status);
		pthread_mutex_unlock(&ctx->api_mutex);
		obs_properties_add_text(props, "api_status", status.array, OBS_TEXT_INFO);
		dstr_free(&status);
		obs_properties_add_button2(props, "api_refresh", "Refresh stream list", api_refresh_clicked, ctx);

		/* the list loads in the background and reloads this window when it arrives */
		request_api_refresh(ctx, false);
	}

	/* an audio-only source has no picture to manage */
	if (!ctx || !ctx->audio_only) {
		add_loss_settings(props, ctx);
		obs_property_t *decoder = obs_properties_add_list(props, S_DECODER, "Video decoding",
								  OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
		obs_property_list_add_int(decoder, "Auto (graphics card, or the CPU if it can't)", CTD_DECODER_AUTO);
		obs_property_list_add_int(decoder, "CPU only", CTD_DECODER_SOFTWARE);
		obs_property_list_add_int(decoder, "Graphics card (keeps retrying it after a failure)",
					  CTD_DECODER_HARDWARE);
	}

	obs_properties_add_int_slider(props, S_LATENCY_MS, "SRT latency (ms)", 20, 2000, 10);
	obs_properties_add_int_slider(props, S_DRIFT_CAP_MS, "Max drift before snapping to live (ms)", 100, 5000, 50);
	obs_properties_add_bool(props, S_SOFT_CORRECTION,
				"Soft clock correction (steer drift back gently, so it rarely needs a snap)");
	obs_properties_add_int_slider(props, S_RECONNECT_S, "Reconnect delay (s)", 1, 30, 1);
	obs_properties_add_int_slider(props, S_STALL_S, "Reconnect after stalled for (s)", 2, 60, 1);
	obs_properties_add_bool(props, S_DISCONNECT_INACTIVE,
				"Disconnect when not in an active scene (saves bandwidth; reconnects when shown)");
	obs_properties_add_bool(
		props, S_AUTO_FIX,
		"Auto-fix: apply safe fixes for this source's issues by itself (graphics-card decoding, a "
		"higher SRT latency), and log them");
	add_audio_settings(props, ctx);
	add_delay_settings(props, ctx);
	add_timecode_settings(props, ctx);
	add_sync_check(props, ctx);

	obs_properties_t *api = obs_properties_create();
	obs_properties_add_int(api, S_API_PORT, "API port", 1, 65535, 1);
	obs_properties_add_text(api, S_API_USER, "API username (if your server needs one)", OBS_TEXT_DEFAULT);
	obs_properties_add_text(api, S_API_PASS, "API password", OBS_TEXT_PASSWORD);

	/* changing the server is opt-in (SPEC H8) */
	obs_property_t *allow = obs_properties_add_bool(api, S_ALLOW_EDITS,
							"Allow this source to create and delete paths on the server");
	obs_property_set_modified_callback(allow, allow_edits_modified);
	obs_properties_add_text(api, S_NEW_PATH, "New path name", OBS_TEXT_DEFAULT);
	obs_properties_add_bool(api, S_NEW_PATH_SLATE, "Show an offline picture on it while nothing is publishing");
	obs_properties_add_button2(api, "create_path", "Create path", create_path_clicked, ctx);

	bool allow_edits = false;
	if (ctx) {
		obs_data_t *settings = obs_source_get_settings(ctx->source);
		allow_edits = obs_data_get_bool(settings, S_ALLOW_EDITS);
		obs_data_release(settings);

		pthread_mutex_lock(&ctx->api_mutex);
		const bool armed = ctx->delete_armed_path && os_gettime_ns() - ctx->delete_armed_ns < DELETE_CONFIRM_NS;
		struct dstr label = {0};
		if (armed)
			dstr_printf(&label, "Click again to delete \"%s\"", ctx->delete_armed_path);
		else
			dstr_copy(&label, "Delete the path picked in the Stream list");
		if (ctx->path_op_message)
			obs_properties_add_text(api, "path_op_status", ctx->path_op_message, OBS_TEXT_INFO);
		pthread_mutex_unlock(&ctx->api_mutex);
		obs_properties_add_button2(api, "delete_path", label.array, delete_path_clicked, ctx);
		dstr_free(&label);
	}
	obs_properties_add_group(props, "api", "MediaMTX server (stream list and paths)", OBS_GROUP_NORMAL, api);
	show_path_edits(api, allow_edits);
	if (ctx) {
		obs_data_t *settings = obs_source_get_settings(ctx->source);
		show_address_fields(props, !*obs_data_get_string(settings, S_SERVER));
		obs_data_release(settings);
	}

	if (ctx) {
		struct dstr stats = {0};
		describe_stats(ctx, &stats);
		obs_properties_add_text(props, "stats", stats.array, OBS_TEXT_INFO);
		dstr_free(&stats);
		obs_properties_add_button2(props, "stats_refresh", "Refresh stats", stats_refresh_clicked, ctx);
		obs_properties_add_button2(props, "reconnect_now", "Reconnect now", reconnect_now_clicked, ctx);
	}
	return props;
}

static void ctd_defaults(obs_data_t *settings)
{
	/* existing sources keep their own address: no profile unless one is picked */
	obs_data_set_default_string(settings, S_SERVER, "");
	obs_data_set_default_string(settings, S_HOST, "127.0.0.1");
	obs_data_set_default_int(settings, S_PORT, 8890);
	obs_data_set_default_string(settings, S_PATH, "");
	obs_data_set_default_int(settings, S_LATENCY_MS, 120);
	obs_data_set_default_int(settings, S_DRIFT_CAP_MS, 700);
	/* SPEC 4.8: retry every second, give a silent connection 3 s, hold the last frame 1.5 s */
	obs_data_set_default_int(settings, S_RECONNECT_S, 1);
	obs_data_set_default_int(settings, S_STALL_S, 3);
	obs_data_set_default_int(settings, S_LOSS_MODE, CTD_LOSS_CARD);
	obs_data_set_default_int(settings, S_HOLD_MS, 1500);
	obs_data_set_default_string(settings, S_LOSS_IMAGE, "");
	obs_data_set_default_int(settings, S_DECODER, CTD_DECODER_AUTO);
	obs_data_set_default_bool(settings, S_SOFT_CORRECTION, true);
	obs_data_set_default_bool(settings, S_DISCONNECT_INACTIVE, false);
	obs_data_set_default_bool(settings, S_AUTO_FIX, false);
	obs_data_set_default_int(settings, S_CHANNELS, CTD_CHANNELS_STEREO);
	obs_data_set_default_bool(settings, S_FILL_GAPS, true);
	obs_data_set_default_bool(settings, S_LOCK_AUDIO, false);
	obs_data_set_default_int(settings, S_RATE_OVERRIDE, 0);
	obs_data_set_default_int(settings, S_VIDEO_DELAY_MS, 0);
	obs_data_set_default_int(settings, S_AUDIO_DELAY_MS, 0);
	obs_data_set_default_int(settings, S_DELAY_CAP_MB, 512);
	obs_data_set_default_int(settings, S_TIMECODE, CTD_TIMECODE_NONE);
	obs_data_set_default_string(settings, S_SYNC_GROUP, "");
	obs_data_set_default_bool(settings, S_VIDEO_ONLY, false);
	obs_data_set_default_int(settings, S_API_PORT, 9997);
	obs_data_set_default_string(settings, S_API_USER, "");
	obs_data_set_default_string(settings, S_API_PASS, "");
	obs_data_set_default_bool(settings, S_ALLOW_EDITS, false);
	obs_data_set_default_string(settings, S_NEW_PATH, "");
	obs_data_set_default_bool(settings, S_NEW_PATH_SLATE, false);
}

struct obs_source_info ctd_live_source_info = {
	.id = "ctd_live_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE,
	.get_name = ctd_get_name,
	.create = ctd_create,
	.destroy = ctd_destroy,
	.update = ctd_update,
	.save = ctd_save,
	.activate = ctd_activate,
	.deactivate = ctd_deactivate,
	.get_properties = ctd_properties,
	.get_defaults = ctd_defaults,
};

/* Same source, audio only: shows up as an audio input, takes no canvas space, and never decodes video. */
struct obs_source_info ctd_live_audio_source_info = {
	.id = "ctd_live_audio_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE,
	.get_name = ctd_audio_get_name,
	.create = ctd_audio_create,
	.destroy = ctd_destroy,
	.update = ctd_update,
	.activate = ctd_activate,
	.deactivate = ctd_deactivate,
	.get_properties = ctd_properties,
	.get_defaults = ctd_defaults,
	.icon_type = OBS_ICON_TYPE_AUDIO_INPUT,
};
