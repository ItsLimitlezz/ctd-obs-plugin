/*
 * The CTD Live engine. See engine.h.
 *
 * Why this exists: OBS's stock Media Source is a file player wearing a network coat. It trusts stream timestamps,
 * buffers without bound, and never drops audio, so after one network transient video goes choppy-but-live while
 * audio drifts minutes behind, and it reports itself healthy the whole time. This engine makes the opposite promises:
 *
 *   1. LIVE EDGE IS LAW. Frames are timed against the wall clock from the moment they arrive. If presentation falls
 *      further behind than the configured drift cap, the clock is re-based and playback snaps back to live. Delay
 *      cannot accumulate, on either track.
 *   2. NOTHING IS HOARDED. There are no elastic queues here; packets are decoded as they arrive and handed straight
 *      out with corrected timestamps. The only buffer is SRT's own latency window, plus the delay the user asked for
 *      (S1), which is fixed, bounded in memory, and never grows by itself.
 *   3. IT RECONNECTS ITSELF. Any error, EOF, or read stall tears the session down and dials again with backoff.
 *
 * The transport/demux/decode layers are the FFmpeg libraries OBS already ships (which include libsrt: that's how
 * srt:// works). Only the policy layer is ours.
 */

#include "engine.h"
#include "eng-atomic.h"
#include "audio-norm.h"
#include "delay-queue.h"
#include "hw-decode.h"
#include "ltc.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
#include <libavutil/time.h>

#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NS_PER_S 1000000000ull

/* graphics-card decode errors in a row before a session gives the card up */
#define GPU_ERROR_LIMIT 30

static const char *const feed_state_names[] = {"CONNECTING", "LIVE", "STALLED", "RECONNECTING", "NO SIGNAL"};

const char *ctd_feed_state_name(enum ctd_feed_state state)
{
	return state >= CTD_FEED_CONNECTING && state <= CTD_FEED_NO_SIGNAL ? feed_state_names[state] : "?";
}

struct ctd_engine {
	/* fixed for the engine's life */
	char *host;
	int port;
	char *path;
	int latency_ms;
	int64_t drift_cap_ns;
	int reconnect_s;
	int stall_s;
	enum ctd_decoder_mode decoder;
	char *gpu_adapter;
	char *read_user;
	char *read_pass;
	char *passphrase;
	bool audio_only;
	struct ctd_engine_callbacks cb;

	/* set from any thread */
	volatile long stopping;
	volatile long reconnect_now;
	volatile long loss_mode; /* enum ctd_loss_mode */
	volatile long hold_ms;
	volatile long soft_correction;
	volatile long redraw_loss;

	/* signal loss, owned by the worker (SPEC 4.5) */
	uint64_t signal_ns; /* the last frame handed out, or when the worker started if none yet */
	uint64_t stall_ns;  /* the stall threshold */
	bool had_signal;    /* media has flowed since the worker started */
	bool loss_applied;  /* the loss picture is up for the current gap */
	long applied_mode;  /* the loss mode it was put up with */

	/* hardware decode (SPEC R19), owned by the worker */
	struct hw_decode hw;
	bool hw_given_up;    /* Auto: the graphics card failed, so this worker decodes on the CPU from now on */
	bool hw_skip_next;   /* Hardware: it failed last connection, so this one decodes on the CPU */
	char hw_logged[128]; /* the last graphics-card problem logged, so each is logged once */

	/* input, owned by the worker: what it last reported, so it only calls back on a change */
	struct ctd_input input;
	uint64_t fps_window_ns;
	long fps_window_frames;

	/* stats, written by the worker, read from any thread */
	volatile long state;
	volatile long video_device;
	volatile long reconnects;
	volatile long resyncs;
	volatile long discontinuities;
	volatile long video_frames;
	volatile long audio_frames;
	volatile long lateness_ms;
	volatile long target_ms; /* LONG_MIN until set */
	volatile long correction_ppm;
	volatile long fps_x100;
	volatile int64_t last_media_ns;

	/* SPEC 6.2 measurements: the worker's windows, and what it publishes */
	float level_peak[2];
	uint64_t level_samples;
	uint64_t level_clipped;
	uint64_t level_window_ns;
	volatile long peak_db_x100[2];
	volatile long clipped_x10000; /* percent × 100 */
	volatile int64_t last_audio_ns;
	uint32_t picture_checksum;
	bool have_checksum;
	uint64_t picture_same_since_ns; /* 0 while the picture changes */
	volatile long frozen_ms;
	double decode_avg_ms;
	volatile long decode_us;
	volatile long nominal_fps_x100;

	/* section 7, audio repair: set from any thread */
	bool no_audio;               /* A6 */
	volatile long channels;      /* A1, enum ctd_channels */
	volatile long fill_gaps;     /* A2 */
	volatile long lock_to_video; /* A3 */
	volatile long rate_override; /* A4 */
	volatile long reset_audio;   /* A5 */
	/* A2: the last 5 ms of audio, held back until the next chunk shows whether a hole follows */
	float hold[2][240];
	int hold_count;
	uint64_t hold_ts;
	float *work[2]; /* the chunk after the channel mode */
	int work_capacity;
	long seen_resyncs; /* a snap starts a new audio timeline: nothing to fill across it */
	volatile long gaps_filled;
	volatile long gap_ms_filled;
	/* A3: how late video and audio each run, and the audio-only offset steering audio to video */
	double late_video_ns;
	double late_audio_ns;
	bool late_video_set;
	bool late_audio_set;
	double lock_offset_ns;
	double lock_rate_ns_per_s;
	double lock_pending_ns;
	volatile long lock_ppm;

	/* section 8.1, delay (S1, S2): the settings, from any thread */
	volatile long video_delay_target_ms;
	volatile long audio_delay_target_ms;
	volatile long delay_cap_mb;
	volatile long group_delay_ms;   /* S9 to S14: the sync group's delay, on top of both */
	volatile long group_jump_video; /* the first lock: apply it at once */
	volatile long group_jump_audio;
	/* owned by the worker */
	struct delay_queue delay;
	double video_delay_ns;         /* applied: moves toward the setting a frame at a time */
	double audio_delay_ns;         /* applied: moves toward the setting by stretching the audio */
	double audio_delay_pending_ns; /* what the frame being stretched adds, before the next one is stamped */
	int video_step_wait;           /* frames until the next single-frame step */
	long limit_logged_ms;
	/* published */
	volatile long video_delay_ms;
	volatile long audio_delay_ms;
	volatile long video_delay_limit_ms; /* the most the memory cap holds at this resolution; 0 until known */
	volatile long delay_kb;             /* what the delay queue holds */
	volatile long delay_settled;        /* both delays have arrived */

	/* section 8.2, timecode: the setting, from any thread */
	volatile long timecode_source; /* enum ctd_timecode_source */
	/* owned by the worker */
	long timecode_seen;        /* the setting the decoders were set up for */
	struct ltc_decoder ltc[8]; /* one per channel: the timecode channel, or every channel to detect LTC (S7) */
	struct ltc_rate ltc_rate;  /* the timecode channel's rate and error count */
	float *ltc_samples;        /* one channel as float */
	int ltc_capacity;
	long detect_words[8]; /* S7: words each channel decoded this second */
	uint64_t detect_window_ns;
	uint64_t ltc_level_ns;
	int channel_override; /* S4: the program channel mode while an LTC channel is taken out, or -1 */
	/* published together under tc_seq (odd while being written), so the time and where it is pinned match */
	volatile long tc_seq;
	volatile long tc_packed; /* hours << 24 | minutes << 16 | seconds << 8 | frames */
	volatile long tc_drop;
	volatile long tc_user_bits;
	volatile long tc_fps_x1000;
	volatile int64_t tc_stream_ns;
	volatile int64_t tc_clock_ns;
	volatile int64_t tc_seen_ns; /* host clock: when the last timecode was decoded */
	volatile long tc_level_x100;
	volatile long tc_error_x10000;
	volatile long tc_detected; /* S7: 1 to 8, or 0 */
	volatile int64_t tc_detected_ns;

	/* S16, the sync check: asked for from any thread, run by the worker */
	volatile long check_asked;
	volatile long check_state; /* enum ctd_sync_check_state */
	volatile long check_offset_x10;
	volatile long check_pairs;
	uint64_t check_started_ns;
	uint8_t check_cells[256]; /* the last picture's 16 × 16 grid */
	bool check_have_cells;
	uint64_t check_flashes[64]; /* live clock: when the picture flashed, and when the beep started */
	int check_flash_count;
	uint64_t check_beeps[64];
	int check_beep_count;
	double check_floor; /* the 1 kHz level between beeps */
	bool check_in_beep;
};

static void release_delayed(struct ctd_engine *e);
static void check_tick(struct ctd_engine *e);

/* --------------------------------------------------------------- helpers -- */

static char *copy_string(const char *value)
{
	return av_strdup(value ? value : "");
}

static uint64_t now_ns(struct ctd_engine *e)
{
	return e->cb.now_ns();
}

/*
 * The SRT URL, the stream id and how to show them in the log. The stream id (MediaMTX's read:<path>[:<user>:<pass>])
 * and the passphrase go to FFmpeg as options, never in the URL: FFmpeg's own srt code logs the URL when a connection
 * fails, and a password must not reach any log (H2). Latency is in microseconds for FFmpeg.
 */
static void build_urls(struct ctd_engine *e, char *url, size_t size, char *streamid, size_t streamid_size, char *shown,
		       size_t shown_size)
{
	snprintf(url, size, "srt://%s:%d?latency=%d&transtype=live", e->host, e->port, e->latency_ms * 1000);
	if (*e->read_user)
		snprintf(streamid, streamid_size, "read:%s:%s:%s", e->path, e->read_user, e->read_pass);
	else
		snprintf(streamid, streamid_size, "read:%s", e->path);
	snprintf(shown, shown_size, "srt://%s:%d?streamid=read:%s%s%s", e->host, e->port, e->path,
		 *e->read_user ? ":<login>" : "", *e->passphrase ? " (encrypted)" : "");
}

#ifdef __GNUC__
__attribute__((format(printf, 3, 4)))
#endif
static void
eng_log(struct ctd_engine *e, enum ctd_log_level level, const char *format, ...)
{
	if (!e->cb.log)
		return;
	char message[1024];
	va_list args;
	va_start(args, format);
	vsnprintf(message, sizeof(message), format, args);
	va_end(args);
	e->cb.log(e->cb.opaque, level, message);
}

static void set_state(struct ctd_engine *e, enum ctd_feed_state state)
{
	if (eng_exchange_long(&e->state, (long)state) != (long)state && e->cb.state)
		e->cb.state(e->cb.opaque, state);
}

static bool stopping(struct ctd_engine *e)
{
	return eng_load_bool(&e->stopping);
}

/* ------------------------------------------------------------------ clock -- */

/*
 * The clock policy in one place.
 *
 * offset_ns maps stream PTS onto the host's clock. It is established on the first frame after (re)connect, then held.
 * Every outgoing frame computes its lateness = now - (pts + offset). Small lateness is normal jitter and is left for
 * the host to smooth. Lateness beyond the drift cap means a backlog built up somewhere upstream; instead of playing it
 * out (the stock source's mistake), the offset is re-based so the CURRENT frame is "now": a hard snap to the live edge
 * (SPEC R4, the safety net). Audio and video share the offset, so they can never drift apart by more than the cap.
 *
 * Inside the cap, soft correction (R5) keeps a sender clock that runs a little fast or slow from ever reaching it: the
 * lateness, averaged over about 2 s, is steered back to the target (the average just after connecting) by moving the
 * offset at most 2 ms per second. Audio is stretched or squeezed by the same amount in the resampler, so the host's
 * audio timeline follows without a jump. Outside half the cap the correction stands aside and the hard snap decides.
 */
#define SOFT_AVERAGE_S       2.0       /* the lateness average's time constant */
#define SOFT_MAX_NS_PER_S    2000000.0 /* the most the offset moves: 2 ms per second (audio: ±0.2%) */
#define SOFT_GAIN_PER_S      0.2       /* fraction of the error corrected per second */
#define SOFT_TARGET_AFTER_NS 2000000000ull

struct live_clock {
	bool established;
	int64_t offset_ns;
	uint64_t established_ns;
	uint64_t last_ns;
	bool soft; /* soft correction on (setting) */
	bool average_set;
	double average_ns; /* lateness, averaged */
	bool target_set;
	double target_ns;
	double correction_ns_per_s; /* the offset's current rate of change; positive slows playback down */
};

/* Maps pts onto the clock. *lateness is how late the frame was before any snap; *resynced says it snapped. */
static uint64_t live_clock_stamp(struct live_clock *clock, uint64_t now, int64_t pts_ns, int64_t drift_cap_ns,
				 int64_t *lateness, bool *resynced)
{
	*resynced = false;
	*lateness = 0;

	if (!clock->established) {
		clock->established = true;
		clock->offset_ns = (int64_t)now - pts_ns;
		clock->established_ns = now;
		clock->last_ns = now;
		clock->average_set = false;
		clock->target_set = false;
		clock->correction_ns_per_s = 0.0;
		return now;
	}

	int64_t mapped = pts_ns + clock->offset_ns;
	*lateness = (int64_t)now - mapped;

	if (*lateness > drift_cap_ns || *lateness < -drift_cap_ns) {
		/* Fell behind (or the stream clock jumped): snap to live. The target stays; the average starts again. */
		clock->offset_ns = (int64_t)now - pts_ns;
		mapped = (int64_t)now;
		*resynced = true;
		clock->average_set = false;
		clock->correction_ns_per_s = 0.0;
		clock->last_ns = now;
		return (uint64_t)mapped;
	}

	/* R5: soft correction inside the cap */
	double dt = (double)(now - clock->last_ns) / 1e9;
	clock->last_ns = now;
	if (dt > 1.0)
		dt = 1.0;
	if (!clock->average_set) {
		clock->average_ns = (double)*lateness;
		clock->average_set = true;
	} else {
		const double alpha = dt / (SOFT_AVERAGE_S + dt);
		clock->average_ns += alpha * ((double)*lateness - clock->average_ns);
	}
	if (!clock->target_set && now - clock->established_ns >= SOFT_TARGET_AFTER_NS) {
		clock->target_ns = clock->average_ns;
		clock->target_set = true;
	}
	clock->correction_ns_per_s = 0.0;
	if (clock->soft && clock->target_set) {
		const double error = clock->average_ns - clock->target_ns;
		if (error < (double)drift_cap_ns / 2.0 && error > -(double)drift_cap_ns / 2.0) {
			double rate = error * SOFT_GAIN_PER_S;
			if (rate > SOFT_MAX_NS_PER_S)
				rate = SOFT_MAX_NS_PER_S;
			if (rate < -SOFT_MAX_NS_PER_S)
				rate = -SOFT_MAX_NS_PER_S;
			clock->correction_ns_per_s = rate;
			const double step = rate * dt;
			clock->offset_ns += (int64_t)step;
			clock->average_ns -= step; /* the offset moved: so did the lateness it measures */
		}
	}
	return (uint64_t)mapped;
}

/* Stamps one outgoing frame, logging and counting a snap. F15: a snap is the moment delay would have started to
 * accumulate in the stock source, so the log tells that story afterwards. */
static uint64_t stamp_frame(struct ctd_engine *e, struct live_clock *clock, int64_t pts_ns)
{
	int64_t lateness;
	bool resynced;
	const uint64_t timestamp = live_clock_stamp(clock, now_ns(e), pts_ns, e->drift_cap_ns, &lateness, &resynced);
	if (resynced) {
		eng_log(e, CTD_LOG_INFO, "drift %+lld ms exceeded cap — snapping to live edge",
			(long long)(lateness / 1000000));
		eng_inc_long(&e->resyncs);
	}
	/* R6: what the stats show */
	eng_store_long(&e->lateness_ms, (long)((clock->average_set ? clock->average_ns : 0.0) / 1e6));
	eng_store_long(&e->target_ms, clock->target_set ? (long)(clock->target_ns / 1e6) : LONG_MIN);
	eng_store_long(&e->correction_ppm, (long)(clock->correction_ns_per_s / 1e3)); /* ns/s → parts per million */
	return timestamp;
}

/* ------------------------------------------------------------ signal loss -- */

/*
 * What the picture does while the feed is gone (SPEC R10–R13). The worker notes every frame it hands out; whenever it
 * is waiting (inside FFmpeg's blocking calls, or between connection attempts) it checks how long the feed has been
 * silent. Past the hold time it asks the host, once, to apply the loss mode: put up the card, keep the last frame, or
 * go see-through. Audio simply stops (R12). The gap is measured across reconnects, so a quick reconnect doesn't flash
 * the card.
 */

/* Every frame handed out. */
static void note_media(struct ctd_engine *e)
{
	const uint64_t now = now_ns(e);
	if (eng_load_long(&e->state) != CTD_FEED_LIVE) {
		if (e->had_signal)
			eng_log(e, CTD_LOG_INFO, "back live after %.1f s without signal",
				(double)(now - e->signal_ns) / 1e9);
		set_state(e, CTD_FEED_LIVE);
	}
	e->signal_ns = now;
	eng_store_i64(&e->last_media_ns, (int64_t)now);
	e->had_signal = true;
	e->loss_applied = false;
}

static const char *loss_mode_action(long mode)
{
	switch (mode) {
	case CTD_LOSS_TRANSPARENT:
		return "going see-through";
	case CTD_LOSS_HOLD:
		return "holding the last frame";
	default:
		return "showing the reconnecting card";
	}
}

/* Called often from the worker while it waits; cheap while the feed is live. */
static void check_signal(struct ctd_engine *e)
{
	release_delayed(e); /* S2: delayed media keeps playing out while the worker waits */
	check_tick(e);      /* S16 */
	const uint64_t now = now_ns(e);
	const uint64_t gap = now - e->signal_ns;
	const uint64_t hold_ns = (uint64_t)eng_load_long(&e->hold_ms) * 1000000ull;
	if (gap > hold_ns) {
		const long state = eng_load_long(&e->state);
		const long mode = eng_load_long(&e->loss_mode);
		const bool redraw = e->loss_applied && eng_exchange_long(&e->redraw_loss, 0) != 0;
		if (!e->loss_applied || mode != e->applied_mode || redraw) {
			if (!e->loss_applied && e->had_signal)
				eng_log(e, CTD_LOG_INFO, "no signal for %.1f s: %s", (double)gap / 1e9,
					loss_mode_action(mode));
			e->loss_applied = true;
			e->applied_mode = mode;
			if (!e->audio_only && e->cb.loss)
				e->cb.loss(e->cb.opaque, (enum ctd_loss_mode)mode, !e->had_signal);
			if (state == CTD_FEED_LIVE)
				set_state(e, CTD_FEED_STALLED);
		}
		if (gap > hold_ns + e->stall_ns && state != CTD_FEED_NO_SIGNAL && state != CTD_FEED_LIVE)
			set_state(e, CTD_FEED_NO_SIGNAL);
	}
	if (e->cb.tick)
		e->cb.tick(e->cb.opaque);
}

/* ------------------------------------------------------------- interrupts -- */

struct interrupt_ctx {
	struct ctd_engine *e;
	uint64_t deadline_ns; /* read must complete before this */
	bool timed_out;       /* the deadline cut a read: anything the demuxer still hands over is stale */
};

static int interrupt_cb(void *opaque)
{
	struct interrupt_ctx *ic = opaque;
	/* FFmpeg calls this often while it blocks, which is exactly when the feed may have gone quiet */
	check_signal(ic->e);
	if (stopping(ic->e))
		return 1; /* the host is stopping the engine: abort I/O */
	if (eng_load_bool(&ic->e->reconnect_now))
		return 1; /* "Reconnect now": drop this connection */
	if (ic->deadline_ns && now_ns(ic->e) > ic->deadline_ns) {
		ic->timed_out = true;
		return 1; /* read stalled: abort so the outer loop reconnects */
	}
	return 0;
}

/* ------------------------------------------------------------ input stats --- */

static void report_input(struct ctd_engine *e)
{
	if (e->cb.input)
		e->cb.input(e->cb.opaque, &e->input);
}

static void note_connected_input(struct ctd_engine *e, const AVCodecContext *vdec, const AVCodecContext *adec)
{
	memset(&e->input, 0, sizeof(e->input));
	snprintf(e->input.video_codec, sizeof(e->input.video_codec), "%s", vdec ? vdec->codec->name : "");
	snprintf(e->input.audio_codec, sizeof(e->input.audio_codec), "%s", adec ? adec->codec->name : "");
	report_input(e);
	eng_store_long(&e->target_ms, LONG_MIN); /* a new clock: no target until it has settled */
	eng_store_long(&e->correction_ppm, 0);
	e->fps_window_ns = now_ns(e);
	e->fps_window_frames = 0;
	eng_store_long(&e->fps_x100, 0);
}

static void note_video_frame(struct ctd_engine *e, int width, int height)
{
	if (e->input.width != width || e->input.height != height) {
		e->input.width = width;
		e->input.height = height;
		report_input(e);
	}
	/* frames a second, measured over windows of at least a second */
	e->fps_window_frames++;
	const uint64_t now = now_ns(e);
	if (now - e->fps_window_ns >= NS_PER_S) {
		eng_store_long(&e->fps_x100,
			       (long)((uint64_t)e->fps_window_frames * 100000000000ull / (now - e->fps_window_ns)));
		e->fps_window_ns = now;
		e->fps_window_frames = 0;
	}
}

static void note_audio_frame(struct ctd_engine *e, int sample_rate, int channels)
{
	if (e->input.sample_rate != sample_rate || e->input.channels != channels) {
		e->input.sample_rate = sample_rate;
		e->input.channels = channels;
		report_input(e);
	}
}

/* ---------------------------------------------------------- measurements --- */

/* SPEC 6.2: audio levels on what goes out (48 kHz stereo float), published once a second */
static void measure_audio(struct ctd_engine *e, const float *const planes[2], int frames)
{
	for (int c = 0; c < 2; c++) {
		float peak = e->level_peak[c];
		uint64_t clipped = 0;
		for (int i = 0; i < frames; i++) {
			const float v = fabsf(planes[c][i]);
			if (v > peak)
				peak = v;
			clipped += v >= 0.999f;
		}
		e->level_peak[c] = peak;
		e->level_clipped += clipped;
	}
	e->level_samples += (uint64_t)frames * 2;
	const uint64_t now = now_ns(e);
	eng_store_i64(&e->last_audio_ns, (int64_t)now);
	if (!e->level_window_ns)
		e->level_window_ns = now;
	if (now - e->level_window_ns < NS_PER_S)
		return;
	for (int c = 0; c < 2; c++) {
		const double db = e->level_peak[c] > 1e-6f ? 20.0 * log10((double)e->level_peak[c]) : -120.0;
		eng_store_long(&e->peak_db_x100[c], (long)(db * 100.0));
		e->level_peak[c] = 0.0f;
	}
	eng_store_long(&e->clipped_x10000,
		       e->level_samples ? (long)(e->level_clipped * 1000000ull / e->level_samples) : 0);
	e->level_samples = e->level_clipped = 0;
	e->level_window_ns = now;
}

/* SPEC 6.2 "frozen picture": a cheap checksum over a 16 × 16 grid of the first plane (luma, or BGRA's bytes), and how
 * long it has stayed the same while frames keep arriving. A live camera's noise changes it every frame. */
static void measure_picture(struct ctd_engine *e, const AVFrame *picture)
{
	if (!picture->data[0] || picture->width < 32 || picture->height < 32)
		return;
	const int step = picture->format == AV_PIX_FMT_BGRA ? 4 : picture->format == AV_PIX_FMT_P010LE ? 2 : 1;
	uint32_t hash = 2166136261u; /* FNV-1a */
	for (int y = 0; y < 16; y++) {
		const uint8_t *row =
			picture->data[0] + (size_t)((2 * y + 1) * picture->height / 32) * picture->linesize[0];
		for (int x = 0; x < 16; x++) {
			const uint8_t *px = row + (size_t)((2 * x + 1) * picture->width / 32) * step;
			for (int b = 0; b < step && b < 3; b++) {
				hash ^= px[b];
				hash *= 16777619u;
			}
		}
	}
	const uint64_t now = now_ns(e);
	if (e->have_checksum && hash == e->picture_checksum) {
		if (!e->picture_same_since_ns)
			e->picture_same_since_ns = now;
		eng_store_long(&e->frozen_ms, (long)((now - e->picture_same_since_ns) / 1000000));
	} else {
		e->picture_same_since_ns = 0;
		eng_store_long(&e->frozen_ms, 0);
	}
	e->picture_checksum = hash;
	e->have_checksum = true;
}

/* ------------------------------------------------------------- sync check --- */

/*
 * S16: the user plays the sync clip (a white flash and a 1 kHz beep at the same moment, once a second) in front of the
 * camera. For 10 s the worker notes when the picture flashes (a jump in brightness over at least 4% of a 16 × 16 grid,
 * so a phone filling part of the frame is enough) and when the beep starts (1 kHz, Goertzel over 5 ms blocks, well
 * above its level between beeps), both on the live clock before any delay. Each flash is paired with the beep nearest
 * it (within 500 ms); the median of beep minus flash is the feed's A/V offset: positive, the sound comes late.
 */
#define CHECK_NS (10ull * NS_PER_S)
#define CHECK_MIN_PAIRS 5
#define CHECK_DEBOUNCE_NS 500000000ull

static void check_start(struct ctd_engine *e)
{
	e->check_started_ns = now_ns(e);
	e->check_have_cells = false;
	e->check_flash_count = e->check_beep_count = 0;
	e->check_floor = 0;
	e->check_in_beep = false;
	eng_store_long(&e->check_pairs, 0);
	eng_store_long(&e->check_state, CTD_SYNC_CHECK_RUNNING);
	eng_log(e, CTD_LOG_INFO, "sync check: play the sync clip in front of the camera for 10 s");
}

static int compare_long_long(const void *a, const void *b)
{
	const long long x = *(const long long *)a, y = *(const long long *)b;
	return x < y ? -1 : x > y;
}

/* at the end of the 10 s: pair, take the median */
static void check_finish(struct ctd_engine *e)
{
	long long offsets[64];
	int pairs = 0;
	for (int f = 0; f < e->check_flash_count; f++) {
		long long best = 0;
		bool found = false;
		for (int b = 0; b < e->check_beep_count; b++) {
			const long long d = (long long)e->check_beeps[b] - (long long)e->check_flashes[f];
			if (llabs(d) < 500000000ll && (!found || llabs(d) < llabs(best))) {
				best = d;
				found = true;
			}
		}
		if (found)
			offsets[pairs++] = best;
	}
	eng_store_long(&e->check_pairs, pairs);
	if (pairs < CHECK_MIN_PAIRS) {
		eng_store_long(&e->check_state, CTD_SYNC_CHECK_FAILED);
		eng_log(e, CTD_LOG_WARNING,
			"sync check: only %d flash and beep pairs (%d flashes, %d beeps): is the clip "
			"in view and audible?",
			pairs, e->check_flash_count, e->check_beep_count);
		return;
	}
	qsort(offsets, (size_t)pairs, sizeof(offsets[0]), compare_long_long);
	const double median_ms = (pairs % 2 ? (double)offsets[pairs / 2]
					    : ((double)offsets[pairs / 2 - 1] + (double)offsets[pairs / 2]) / 2.0) /
				 1e6;
	eng_store_long(&e->check_offset_x10, lround(median_ms * 10.0));
	eng_store_long(&e->check_state, CTD_SYNC_CHECK_DONE);
	eng_log(e, CTD_LOG_INFO, "sync check: the sound comes %.0f ms %s the picture (%d pairs)", fabs(median_ms),
		median_ms >= 0 ? "after" : "before", pairs);
}

/* called often from the worker: starts a check that was asked for, finishes one whose time is up */
static void check_tick(struct ctd_engine *e)
{
	if (eng_exchange_long(&e->check_asked, 0))
		check_start(e);
	if (eng_load_long(&e->check_state) == CTD_SYNC_CHECK_RUNNING && now_ns(e) - e->check_started_ns >= CHECK_NS)
		check_finish(e);
}

/* a decoded picture at live clock time ts: did it flash? */
static void check_picture(struct ctd_engine *e, const AVFrame *picture, uint64_t ts)
{
	if (eng_load_long(&e->check_state) != CTD_SYNC_CHECK_RUNNING || !picture->data[0] || picture->width < 32 ||
	    picture->height < 32)
		return;
	const int step = picture->format == AV_PIX_FMT_BGRA ? 4 : picture->format == AV_PIX_FMT_P010LE ? 2 : 1;
	const int byte = picture->format == AV_PIX_FMT_BGRA ? 1 : step == 2 ? 1 : 0; /* green, or P010's high byte */
	uint8_t cells[256];
	int brighter = 0;
	for (int y = 0; y < 16; y++) {
		const uint8_t *row =
			picture->data[0] + (size_t)((2 * y + 1) * picture->height / 32) * picture->linesize[0];
		for (int x = 0; x < 16; x++) {
			const uint8_t v = row[(size_t)((2 * x + 1) * picture->width / 32) * step + byte];
			cells[y * 16 + x] = v;
			if (e->check_have_cells && v > e->check_cells[y * 16 + x] + 40)
				brighter++;
		}
	}
	memcpy(e->check_cells, cells, sizeof(cells));
	const bool had = e->check_have_cells;
	e->check_have_cells = true;
	if (!had || brighter < 10)
		return;
	if (e->check_flash_count && ts - e->check_flashes[e->check_flash_count - 1] < CHECK_DEBOUNCE_NS)
		return;
	if (e->check_flash_count < 64)
		e->check_flashes[e->check_flash_count++] = ts;
}

/* 48 kHz stereo audio starting at live clock time ts: did the beep start? */
static void check_sound(struct ctd_engine *e, const float *left, const float *right, int frames, uint64_t ts)
{
	if (eng_load_long(&e->check_state) != CTD_SYNC_CHECK_RUNNING)
		return;
	const double coefficient = 2.0 * cos(2.0 * 3.14159265358979323846 * 1000.0 / 48000.0);
	for (int start = 0; start + 240 <= frames; start += 240) {
		double s1 = 0, s2 = 0;
		for (int i = start; i < start + 240; i++) {
			const double s = (left[i] + right[i]) * 0.5 + coefficient * s1 - s2;
			s2 = s1;
			s1 = s;
		}
		/* the 1 kHz amplitude over the block */
		const double level = sqrt(fmax(s1 * s1 + s2 * s2 - coefficient * s1 * s2, 0.0)) * 2.0 / 240.0;
		const double threshold = fmax(0.01, 8.0 * e->check_floor);
		if (level > threshold) {
			const uint64_t at = ts + (uint64_t)start * 1000000000ull / 48000ull;
			if (!e->check_in_beep &&
			    (!e->check_beep_count ||
			     at - e->check_beeps[e->check_beep_count - 1] >= CHECK_DEBOUNCE_NS) &&
			    e->check_beep_count < 64)
				e->check_beeps[e->check_beep_count++] = at;
			e->check_in_beep = true;
		} else {
			e->check_in_beep = false;
			e->check_floor = e->check_floor * 0.98 + level * 0.02;
		}
	}
}

/* ------------------------------------------------------------------ delay --- */

/*
 * Manual delay (SPEC S1, S2). Each track gets its own offset on top of the live clock's timestamps. A delayed picture or
 * sound waits in the plugin's own queue and goes to the host just ahead of its time, so OBS only ever sees what it
 * would see without a delay. Changes while the feed is on screen never jump: the video offset moves one frame at a
 * time (a frame shown twice to add delay, one skipped to take it away), at most every third frame; the audio offset
 * moves at most 50 ms a second, the audio stretched or squeezed to match (up to 5%). With nothing on screen the new
 * delay applies at once. Pictures are capped in memory (S2), and the video delay with them.
 */
#define DELAY_LEAD_NS         150000000ull /* handed out this long before they are due */
#define DELAY_MAX_MS          2000
#define AUDIO_DELAY_MAX_NS_S  50000000.0 /* 50 ms a second */
#define AUDIO_DELAY_GAIN_PER_S 2.0
#define VIDEO_STEP_EVERY      3

static void hand_out_picture(struct ctd_engine *e, const AVFrame *picture, uint64_t timestamp)
{
	if (e->cb.video && e->cb.video(e->cb.opaque, picture, timestamp)) {
		note_media(e);
		note_video_frame(e, picture->width, picture->height);
		eng_inc_long(&e->video_frames);
	}
}

static void hand_out_sound(struct ctd_engine *e, const float *const planes[2], int frames, uint64_t timestamp)
{
	if (e->cb.audio)
		e->cb.audio(e->cb.opaque, planes, frames, timestamp);
	measure_audio(e, planes, frames);
	note_media(e);
}

static double frame_ns(struct ctd_engine *e);

static void publish_delay(struct ctd_engine *e)
{
	eng_store_long(&e->video_delay_ms, (long)(e->video_delay_ns / 1e6 + 0.5));
	eng_store_long(&e->audio_delay_ms, (long)(e->audio_delay_ns / 1e6 + 0.5));
	eng_store_long(&e->delay_kb, (long)((e->delay.picture_bytes + e->delay.sound_bytes) / 1024));
	/* settled: both delays where the settings (plus the group, within the memory cap) put them */
	const long group = eng_load_long(&e->group_delay_ms);
	double video_goal = (double)(eng_load_long(&e->video_delay_target_ms) + group) * 1e6;
	const long limit = eng_load_long(&e->video_delay_limit_ms);
	if (limit > 0 && video_goal > (double)limit * 1e6)
		video_goal = (double)limit * 1e6;
	const double audio_goal = (double)(eng_load_long(&e->audio_delay_target_ms) + group) * 1e6;
	const bool video_there = e->audio_only || fabs(e->video_delay_ns - video_goal) <= frame_ns(e) / 2;
	const bool audio_there = e->no_audio || fabs(e->audio_delay_ns - audio_goal) <= 1000000.0;
	eng_store_bool(&e->delay_settled, video_there && audio_there);
}

/* hands out everything in the queue that is (nearly) due */
static void release_delayed(struct ctd_engine *e)
{
	if (delay_queue_empty(&e->delay))
		return;
	const uint64_t due = now_ns(e) + DELAY_LEAD_NS;
	struct delay_picture *p;
	while ((p = delay_pop_picture(&e->delay, due))) {
		hand_out_picture(e, p->frame, p->timestamp_ns);
		delay_picture_free(p);
	}
	struct delay_sound *s;
	while ((s = delay_pop_sound(&e->delay, due))) {
		const float *const planes[2] = {s->planes[0], s->planes[1]};
		hand_out_sound(e, planes, s->frames, s->timestamp_ns);
		delay_sound_free(s);
	}
	publish_delay(e);
}

static bool on_air(struct ctd_engine *e)
{
	return eng_load_long(&e->state) == CTD_FEED_LIVE;
}

static double frame_ns(struct ctd_engine *e)
{
	long fps_x100 = eng_load_long(&e->nominal_fps_x100);
	if (fps_x100 < 100)
		fps_x100 = eng_load_long(&e->fps_x100);
	return fps_x100 >= 100 ? 1e11 / (double)fps_x100 : 1e9 / 30.0;
}

static size_t delay_cap_bytes(struct ctd_engine *e)
{
	return (size_t)eng_load_long(&e->delay_cap_mb) * 1024 * 1024;
}

/* S1, S2: moves the video delay a step toward the setting (within what the memory cap holds at this picture's size).
 * False when this picture is the one skipped. */
static bool step_video_delay(struct ctd_engine *e, const AVFrame *picture)
{
	const double frame = frame_ns(e);
	const size_t bytes = delay_picture_size(picture);
	double target =
		(double)(eng_load_long(&e->video_delay_target_ms) + eng_load_long(&e->group_delay_ms)) * 1e6; /* S14 */
	if (bytes) {
		const double held = (double)(delay_cap_bytes(e) / bytes) * frame - (double)DELAY_LEAD_NS - frame;
		const long limit_ms = held > 0 ? (long)(held / 1e6) : 0;
		eng_store_long(&e->video_delay_limit_ms, limit_ms);
		if (target > (double)limit_ms * 1e6) {
			if (e->limit_logged_ms != limit_ms)
				eng_log(e, CTD_LOG_WARNING,
					"a %ld ms video delay doesn't fit in %ld MB at %dx%d: delaying %ld ms",
					eng_load_long(&e->video_delay_target_ms), eng_load_long(&e->delay_cap_mb),
					picture->width, picture->height, limit_ms);
			e->limit_logged_ms = limit_ms;
			target = (double)limit_ms * 1e6;
		} else {
			e->limit_logged_ms = -1;
		}
	}
	const double diff = target - e->video_delay_ns;
	/* S11: a sync group's first lock lands at once (the picture holds for the difference) */
	const bool jump = eng_exchange_long(&e->group_jump_video, 0) && diff > 0;
	if (!on_air(e) || jump) {
		e->video_delay_ns = target;
		return true;
	}
	if (fabs(diff) < frame / 2 || e->video_step_wait-- > 0)
		return true;
	e->video_step_wait = VIDEO_STEP_EVERY - 1;
	if (diff > 0) {
		e->video_delay_ns += diff < frame ? diff : frame; /* the last picture stays up a frame longer */
		return true;
	}
	e->video_delay_ns -= -diff < frame ? -diff : frame; /* this picture is skipped */
	return false;
}

/* one decoded picture, stamped on the live clock, out now or into the delay queue */
static void put_picture(struct ctd_engine *e, const AVFrame *picture, uint64_t timestamp)
{
	if (!step_video_delay(e, picture)) {
		publish_delay(e);
		return;
	}
	timestamp += (uint64_t)e->video_delay_ns;
	if (e->video_delay_ns <= 0 && !e->delay.pictures) {
		publish_delay(e);
		hand_out_picture(e, picture, timestamp);
		return;
	}
	/* the cap is a hard limit: a picture that doesn't fit pushes the oldest out early */
	const size_t cap = delay_cap_bytes(e);
	while (e->delay.pictures && e->delay.picture_bytes + delay_picture_size(picture) > cap) {
		struct delay_picture *p = delay_pop_picture(&e->delay, UINT64_MAX);
		hand_out_picture(e, p->frame, p->timestamp_ns);
		delay_picture_free(p);
	}
	if (!delay_push_picture(&e->delay, picture, timestamp))
		hand_out_picture(e, picture, timestamp);
	release_delayed(e);
}

/*
 * S1: moves the audio delay toward the setting; returns how fast it is moving (ns a second), for the resampler. It
 * moves by stream time, not wall time: the previous frame was stretched by rate × its length, so exactly that much is
 * added before this one is stamped. (Audio arrives in bursts; by wall time the timestamps would jump ahead of the
 * stretched audio and leave holes.)
 */
static double step_audio_delay(struct ctd_engine *e, double frame_s)
{
	e->audio_delay_ns += e->audio_delay_pending_ns;
	e->audio_delay_pending_ns = 0.0;
	const double target =
		(double)(eng_load_long(&e->audio_delay_target_ms) + eng_load_long(&e->group_delay_ms)) * 1e6; /* S14 */
	const double error = target - e->audio_delay_ns;
	/* S11: a sync group's first lock lands at once: the hole it leaves is filled with silence, faded (A2) */
	const bool jump = eng_exchange_long(&e->group_jump_audio, 0) && error > 0;
	if (!on_air(e) || jump || fabs(error) < 500000.0) {
		e->audio_delay_ns = target;
		return 0.0;
	}
	double rate = error * AUDIO_DELAY_GAIN_PER_S;
	if (rate > AUDIO_DELAY_MAX_NS_S)
		rate = AUDIO_DELAY_MAX_NS_S;
	if (rate < -AUDIO_DELAY_MAX_NS_S)
		rate = -AUDIO_DELAY_MAX_NS_S;
	e->audio_delay_pending_ns = rate * frame_s;
	return rate;
}

/* ---------------------------------------------------------- audio repair --- */

#define AUDIO_RATE 48000
#define HOLD_SAMPLES 240                    /* 5 ms at 48 kHz: the fade length, and what is held back */
#define GAP_MIN_NS 5000000ll                /* A2: a hole longer than 5 ms is filled */
#define GAP_MAX_NS 1000000000ll             /* longer than 1 s is a new timeline, not a hole */
static const double ns_per_sample = 1e9 / AUDIO_RATE;

static void output_audio(struct ctd_engine *e, const float *left, const float *right, int frames, uint64_t ts)
{
	if (frames <= 0)
		return;
	const float *const out[2] = {left, right};
	/* S2: delayed audio waits in the queue, and so does everything after it until the queue has drained */
	if (e->audio_delay_ns > 0 || e->delay.sounds) {
		if (!delay_push_sound(&e->delay, out, frames, ts))
			hand_out_sound(e, out, frames, ts);
		release_delayed(e);
		return;
	}
	hand_out_sound(e, out, frames, ts);
}

/* A1: what goes out on each side */
static bool apply_channels(struct ctd_engine *e, const float *const in[2], int frames)
{
	if (frames > e->work_capacity) {
		for (int c = 0; c < 2; c++) {
			av_free(e->work[c]);
			e->work[c] = av_malloc((size_t)frames * sizeof(float));
		}
		e->work_capacity = e->work[0] && e->work[1] ? frames : 0;
		if (!e->work_capacity)
			return false;
	}
	/* S4: an LTC channel never reaches the program audio */
	const long mode = e->channel_override >= 0 ? e->channel_override : eng_load_long(&e->channels);
	for (int i = 0; i < frames; i++) {
		const float l = in[0][i], r = in[1][i];
		switch (mode) {
		case CTD_CHANNELS_LEFT:
			e->work[0][i] = e->work[1][i] = l;
			break;
		case CTD_CHANNELS_RIGHT:
			e->work[0][i] = e->work[1][i] = r;
			break;
		case CTD_CHANNELS_MONO:
			e->work[0][i] = e->work[1][i] = (l + r) * 0.5f;
			break;
		case CTD_CHANNELS_SWAP:
			e->work[0][i] = r;
			e->work[1][i] = l;
			break;
		case CTD_CHANNELS_INVERT_RIGHT:
			e->work[0][i] = l;
			e->work[1][i] = -r;
			break;
		default:
			e->work[0][i] = l;
			e->work[1][i] = r;
			break;
		}
	}
	return true;
}

/*
 * A2: one chunk of 48 kHz stereo out, with holes filled. The last 5 ms of every chunk is held back; when the next one
 * starts more than 5 ms after where the held part ends (on the stream's own timeline, so network timing doesn't
 * matter), the held part fades out, silence of exactly the missing length follows, and the new chunk fades in. Without
 * this, the next audio butts up against the last: sync shifts and it clicks, the "choppy network audio".
 */
static void emit_audio(struct ctd_engine *e, const float *const in[2], int frames, uint64_t ts, bool new_timeline)
{
	if (!apply_channels(e, in, frames))
		return;
	float *left = e->work[0], *right = e->work[1];
	if (!eng_load_bool(&e->fill_gaps)) {
		if (e->hold_count)
			output_audio(e, e->hold[0], e->hold[1], e->hold_count, e->hold_ts);
		e->hold_count = 0;
		output_audio(e, left, right, frames, ts);
		return;
	}

	if (e->hold_count) {
		const uint64_t expected = e->hold_ts + (uint64_t)(e->hold_count * ns_per_sample);
		const int64_t gap = (int64_t)ts - (int64_t)expected;
		if (!new_timeline && gap > GAP_MIN_NS && gap < GAP_MAX_NS) {
			for (int i = 0; i < e->hold_count; i++) {
				const float fade = 1.0f - (float)(i + 1) / (float)e->hold_count;
				e->hold[0][i] *= fade;
				e->hold[1][i] *= fade;
			}
			output_audio(e, e->hold[0], e->hold[1], e->hold_count, e->hold_ts);
			static const float silence[4800] = {0};
			int64_t missing = (int64_t)((double)gap / ns_per_sample + 0.5);
			uint64_t at = expected;
			while (missing > 0) {
				const int n = missing > 4800 ? 4800 : (int)missing;
				output_audio(e, silence, silence, n, at);
				at += (uint64_t)(n * ns_per_sample);
				missing -= n;
			}
			const int fade_in = frames < HOLD_SAMPLES ? frames : HOLD_SAMPLES;
			for (int i = 0; i < fade_in; i++) {
				const float fade = (float)(i + 1) / (float)fade_in;
				left[i] *= fade;
				right[i] *= fade;
			}
			eng_inc_long(&e->gaps_filled);
			eng_store_long(&e->gap_ms_filled, eng_load_long(&e->gap_ms_filled) + (long)(gap / 1000000));
		} else {
			output_audio(e, e->hold[0], e->hold[1], e->hold_count, e->hold_ts);
		}
		e->hold_count = 0;
	}

	const int keep = frames < HOLD_SAMPLES ? frames : HOLD_SAMPLES;
	const int now_out = frames - keep;
	output_audio(e, left, right, now_out, ts);
	memcpy(e->hold[0], left + now_out, (size_t)keep * sizeof(float));
	memcpy(e->hold[1], right + now_out, (size_t)keep * sizeof(float));
	e->hold_count = keep;
	e->hold_ts = ts + (uint64_t)(now_out * ns_per_sample);
}

/* A3: an average of how late each track runs, from every stamped frame */
static void note_lateness(double *average, bool *set, uint64_t now, uint64_t timestamp)
{
	const double late = (double)((int64_t)now - (int64_t)timestamp);
	*average = *set ? *average * 0.95 + late * 0.05 : late;
	*set = true;
}

/* A3: steers an audio-only offset (at most 2 ms a second) so audio runs as late as video; returns the offset. Like the
 * audio delay it moves by stream time: the rate set now stretches the next frame (about as long as this one), so that
 * frame's stretch is added before the one after it is stamped. */
static double lock_audio(struct ctd_engine *e, double frame_s)
{
	if (!eng_load_bool(&e->lock_to_video) || !e->late_video_set || !e->late_audio_set) {
		e->lock_offset_ns = 0.0;
		e->lock_rate_ns_per_s = 0.0;
		e->lock_pending_ns = 0.0;
		eng_store_long(&e->lock_ppm, 0);
		return 0.0;
	}
	e->lock_offset_ns += e->lock_pending_ns;
	/* audio later than video by this much: its timestamps move later by the same */
	const double error = (e->late_audio_ns - e->late_video_ns) - e->lock_offset_ns;
	double rate = error * 0.2;
	if (rate > 2000000.0)
		rate = 2000000.0;
	if (rate < -2000000.0)
		rate = -2000000.0;
	e->lock_rate_ns_per_s = rate;
	e->lock_pending_ns = rate * frame_s;
	eng_store_long(&e->lock_ppm, (long)(rate / 1e3));
	return e->lock_offset_ns;
}

/* --------------------------------------------------------------- timecode --- */

/*
 * Timecode (SPEC S4 to S7). LTC is read from the decoded audio before resampling, at its own rate, and each word is
 * pinned to the stream time where its sync word ended (S6), and to that moment on the live clock. With no timecode
 * input set, every channel is watched for LTC (S7), so the issue engine can offer to use it. SEI timecode comes from
 * the pictures that carry it.
 */

static void publish_timecode(struct ctd_engine *e, const struct ltc_word *w, double fps, int64_t stream_ns,
			     uint64_t clock_ns)
{
	eng_inc_long(&e->tc_seq); /* odd: being written */
	eng_store_long(&e->tc_packed,
		       (long)w->hours << 24 | (long)w->minutes << 16 | (long)w->seconds << 8 | w->frames);
	eng_store_long(&e->tc_drop, w->drop_frame);
	eng_store_long(&e->tc_user_bits, (long)w->user_bits);
	eng_store_long(&e->tc_fps_x1000, (long)(fps * 1000.0 + 0.5));
	eng_store_i64(&e->tc_stream_ns, stream_ns);
	eng_store_i64(&e->tc_clock_ns, (int64_t)clock_ns);
	eng_store_i64(&e->tc_seen_ns, (int64_t)now_ns(e));
	eng_inc_long(&e->tc_seq);
}

static void reset_timecode(struct ctd_engine *e)
{
	for (int c = 0; c < 8; c++) {
		ltc_decoder_reset(&e->ltc[c]);
		e->detect_words[c] = 0;
	}
	memset(&e->ltc_rate, 0, sizeof(e->ltc_rate));
	e->channel_override = -1;
}

/* one channel of a decoded audio frame, as float; NULL for a sample format it doesn't know */
static const float *channel_as_float(struct ctd_engine *e, const AVFrame *frame, int channel)
{
	const int n = frame->nb_samples;
	if (n > e->ltc_capacity) {
		av_free(e->ltc_samples);
		e->ltc_samples = av_malloc((size_t)n * sizeof(float));
		e->ltc_capacity = e->ltc_samples ? n : 0;
		if (!e->ltc_samples)
			return NULL;
	}
	const enum AVSampleFormat format = (enum AVSampleFormat)frame->format;
	const bool planar = av_sample_fmt_is_planar(format);
	const uint8_t *data = planar ? frame->extended_data[channel] : frame->extended_data[0];
	const int stride = planar ? 1 : frame->ch_layout.nb_channels;
	const int offset = planar ? 0 : channel;
	float *out = e->ltc_samples;
	switch (av_get_packed_sample_fmt(format)) {
	case AV_SAMPLE_FMT_FLT:
		for (int i = 0; i < n; i++)
			out[i] = ((const float *)data)[i * stride + offset];
		break;
	case AV_SAMPLE_FMT_DBL:
		for (int i = 0; i < n; i++)
			out[i] = (float)((const double *)data)[i * stride + offset];
		break;
	case AV_SAMPLE_FMT_S16:
		for (int i = 0; i < n; i++)
			out[i] = ((const int16_t *)data)[i * stride + offset] / 32768.0f;
		break;
	case AV_SAMPLE_FMT_S32:
		for (int i = 0; i < n; i++)
			out[i] = (float)(((const int32_t *)data)[i * stride + offset] / 2147483648.0);
		break;
	case AV_SAMPLE_FMT_U8:
		for (int i = 0; i < n; i++)
			out[i] = (data[i * stride + offset] - 128) / 128.0f;
		break;
	default:
		return NULL;
	}
	return out;
}

/* S4: silences one channel of a decoded audio frame */
static void silence_channel(AVFrame *frame, int channel)
{
	if (av_frame_make_writable(frame) < 0)
		return;
	const enum AVSampleFormat format = (enum AVSampleFormat)frame->format;
	const int size = av_get_bytes_per_sample(format);
	const int silence = av_get_packed_sample_fmt(format) == AV_SAMPLE_FMT_U8 ? 0x80 : 0;
	if (av_sample_fmt_is_planar(format)) {
		memset(frame->extended_data[channel], silence, (size_t)frame->nb_samples * size);
		return;
	}
	const int channels = frame->ch_layout.nb_channels;
	for (int i = 0; i < frame->nb_samples; i++)
		memset(frame->extended_data[0] + ((size_t)i * channels + channel) * size, silence, size);
}

/* S4 to S7: LTC from one decoded audio frame (stream time pts_ns), before it is resampled */
static void read_ltc(struct ctd_engine *e, AVFrame *frame, int64_t pts_ns, const struct live_clock *clock)
{
	const long source = eng_load_long(&e->timecode_source);
	if (source != e->timecode_seen) {
		reset_timecode(e);
		e->timecode_seen = source;
	}
	e->channel_override = -1;
	const int channels = frame->ch_layout.nb_channels;
	const int rate = frame->sample_rate;
	if (source == CTD_TIMECODE_SEI || rate <= 0 || channels <= 0)
		return;
	const int first = source ? (int)source - 1 : 0;
	const int last = source ? first : (channels < 8 ? channels : 8) - 1;
	const uint64_t now = now_ns(e);
	for (int c = first; c <= last && c < channels; c++) {
		const float *samples = channel_as_float(e, frame, c);
		if (!samples)
			return;
		struct ltc_word words[8];
		const int n = ltc_decode(&e->ltc[c], samples, frame->nb_samples, rate, words, 8);
		for (int i = 0; i < n; i++) {
			if (!source) {
				e->detect_words[c]++;
				continue;
			}
			const int64_t at = pts_ns + (int64_t)(words[i].end_offset * 1e9 / rate);
			ltc_rate_add(&e->ltc_rate, &words[i], at, ltc_bit_rate_fps(&e->ltc[c], rate));
			publish_timecode(e, &words[i], ltc_rate_fps(&e->ltc_rate), at,
					 clock->established ? (uint64_t)(at + clock->offset_ns) : 0);
		}
	}

	if (!source) {
		/* S7: a channel that decoded most of a second's words carries LTC */
		if (now - e->detect_window_ns >= NS_PER_S) {
			int best = -1;
			long best_words = 0;
			for (int c = 0; c < 8; c++) {
				if (e->detect_words[c] > best_words) {
					best = c;
					best_words = e->detect_words[c];
				}
				e->detect_words[c] = 0;
			}
			if (best >= 0 && best_words >= 15) {
				eng_store_long(&e->tc_detected, best + 1);
				eng_store_i64(&e->tc_detected_ns, (int64_t)now);
			}
			e->detect_window_ns = now;
		}
		return;
	}
	if (first >= channels)
		return;
	/* the signal level and error rate, once a second */
	if (now - e->ltc_level_ns >= NS_PER_S) {
		e->ltc_level_ns = now;
		eng_store_long(&e->tc_level_x100, (long)(ltc_take_level(&e->ltc[first]) * 100.0));
		eng_store_long(&e->tc_error_x10000, (long)(ltc_rate_error_percent(&e->ltc_rate) * 10000.0));
	}
	/* S4: LTC is a loud tone: out of the program audio, the other channel on both sides */
	if (channels == 2)
		e->channel_override = first == 0 ? CTD_CHANNELS_RIGHT : CTD_CHANNELS_LEFT;
	else
		silence_channel(frame, first);
}

static int bcd(unsigned value)
{
	return (int)((value >> 4) * 10 + (value & 0xF));
}

/* S4: the SMPTE 12M timecode a picture carries (H.264 picture timing SEI, HEVC time code SEI) */
static void read_sei_timecode(struct ctd_engine *e, const AVFrame *frame, int64_t pts_ns, uint64_t clock_ns)
{
	if (eng_load_long(&e->timecode_source) != CTD_TIMECODE_SEI)
		return;
	const AVFrameSideData *side = av_frame_get_side_data(frame, AV_FRAME_DATA_S12M_TIMECODE);
	if (!side || side->size < 2 * sizeof(uint32_t))
		return;
	const uint32_t *codes = (const uint32_t *)side->data;
	if (codes[0] < 1)
		return;
	const uint32_t t = codes[1];
	struct ltc_word w = {
		.hours = bcd(t & 0x3f),
		.minutes = bcd(t >> 8 & 0x7f),
		.seconds = bcd(t >> 16 & 0x7f),
		.frames = bcd(t >> 24 & 0x3f),
		.drop_frame = (t >> 30 & 1) != 0,
	};
	publish_timecode(e, &w, eng_load_long(&e->nominal_fps_x100) / 100.0, pts_ns, clock_ns);
}

/* -------------------------------------------------------- video decoder --- */

/* Logs e->hw.why once, and in Auto mode, if the machine is to blame (give_up), stops trying the graphics card. */
static void note_gpu_problem(struct ctd_engine *e, bool give_up)
{
	if (strcmp(e->hw_logged, e->hw.why) != 0) {
		eng_log(e, e->decoder == CTD_DECODER_HARDWARE ? CTD_LOG_WARNING : CTD_LOG_INFO,
			"%s: decoding the video on the CPU", e->hw.why);
		snprintf(e->hw_logged, sizeof(e->hw_logged), "%s", e->hw.why);
	}
	if (give_up && e->decoder == CTD_DECODER_AUTO)
		e->hw_given_up = true;
}

/*
 * Opens the video decoder: on the graphics card when the Decoder setting and the machine allow it (SPEC R19), on the
 * CPU otherwise. *on_gpu says which.
 */
static AVCodecContext *open_video_decoder(struct ctd_engine *e, const AVCodec *codec, const AVCodecParameters *par,
					  AVRational time_base, bool *on_gpu)
{
	*on_gpu = false;
	const bool try_gpu = e->decoder == CTD_DECODER_HARDWARE ? !e->hw_skip_next
								: e->decoder == CTD_DECODER_AUTO && !e->hw_given_up;
	e->hw_skip_next = false;

	if (try_gpu) {
		AVCodecContext *gpu = avcodec_alloc_context3(codec);
		enum hw_setup setup = HW_NO_DEVICE;
		snprintf(e->hw.why, sizeof(e->hw.why), "out of memory");
		if (gpu && avcodec_parameters_to_context(gpu, par) >= 0) {
			setup = hw_decode_setup(&e->hw, gpu, codec, *e->gpu_adapter ? e->gpu_adapter : NULL);
			if (setup == HW_READY) {
				gpu->thread_count = 1;
				gpu->pkt_timebase = time_base;
				if (avcodec_open2(gpu, codec, NULL) == 0) {
					*on_gpu = true;
					return gpu;
				}
				snprintf(e->hw.why, sizeof(e->hw.why), "the %s decoder for %s wouldn't open",
					 hw_decode_name(&e->hw), codec->name);
				setup = HW_NO_DEVICE;
			}
		}
		avcodec_free_context(&gpu);
		/* no GPU decoder for this codec is about the stream; a graphics card that won't work is about the machine */
		note_gpu_problem(e, setup == HW_NO_DEVICE);
	}

	AVCodecContext *cpu = avcodec_alloc_context3(codec);
	if (!cpu)
		return NULL;
	if (avcodec_parameters_to_context(cpu, par) < 0) {
		avcodec_free_context(&cpu);
		return NULL;
	}
	cpu->thread_count = 2;
	cpu->pkt_timebase = time_base;
	if (avcodec_open2(cpu, codec, NULL) < 0)
		avcodec_free_context(&cpu);
	return cpu;
}

/* ---------------------------------------------------------------- timing --- */

static const AVRational ns_base = {1, 1000000000};

/* SPEC R1: a decoded frame's own timestamp, never the packet just sent (B-frames and decoder delay reorder them):
 * best effort, then the frame's pts, then the packet's dts. */
static int64_t frame_time_ns(const AVFrame *frame, int64_t packet_dts, AVRational time_base)
{
	int64_t ts = frame->best_effort_timestamp;
	if (ts == AV_NOPTS_VALUE)
		ts = frame->pts;
	if (ts == AV_NOPTS_VALUE)
		ts = packet_dts;
	return ts == AV_NOPTS_VALUE ? 0 : av_rescale_q(ts, time_base, ns_base);
}

/* One track's timeline, to spot discontinuities (SPEC R2). */
struct timeline {
	int64_t last_dts_ns; /* AV_NOPTS_VALUE until the first packet */
};

/* Why this packet is a discontinuity, or NULL: a jump of more than 1 s forward or any jump backward in the decode
 * timeline (a publisher restart, a MediaMTX alwaysAvailable slate switch), or new codec parameters. */
static const char *discontinuity(struct timeline *timeline, const AVPacket *packet, AVRational time_base)
{
	const char *why = NULL;
	if (av_packet_get_side_data(packet, AV_PKT_DATA_NEW_EXTRADATA, NULL) ||
	    av_packet_get_side_data(packet, AV_PKT_DATA_PARAM_CHANGE, NULL))
		why = "codec parameters changed";

	const int64_t dts = packet->dts != AV_NOPTS_VALUE ? packet->dts : packet->pts;
	if (dts == AV_NOPTS_VALUE)
		return why;
	const int64_t dts_ns = av_rescale_q(dts, time_base, ns_base);
	if (!why && timeline->last_dts_ns != AV_NOPTS_VALUE) {
		if (dts_ns > timeline->last_dts_ns + 1000000000ll)
			why = "timestamps jumped forward";
		else if (dts_ns < timeline->last_dts_ns)
			why = "timestamps jumped backward";
	}
	timeline->last_dts_ns = dts_ns;
	return why;
}

/* --------------------------------------------------------------- session --- */

/*
 * One connect→play→fail cycle. Returns when the engine is stopped (final) or on any error (the caller reconnects),
 * and says whether it got as far as playing. Failures to connect are logged only when log_failures is set.
 * Everything allocated here is freed here.
 */
static bool run_session(struct ctd_engine *e, const char *url, const char *streamid, const char *shown_url,
			bool log_failures)
{
	AVFormatContext *format = avformat_alloc_context();
	AVCodecContext *vdec = NULL, *adec = NULL;
	AVPacket *packet = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	AVFrame *gpu_copy = av_frame_alloc(); /* a picture copied back from the graphics card */
	int video_index = -1, audio_index = -1;
	struct live_clock clock = {0};
	struct timeline timelines[2] = {{AV_NOPTS_VALUE}, {AV_NOPTS_VALUE}}; /* video, audio */
	bool need_keyframe = true;                                           /* SPEC R3 */
	struct audio_norm norm = {0};                                        /* SPEC R7 to R9 */
	bool connected = false;
	bool on_gpu = false;
	bool refusal_logged = false;
	unsigned gpu_errors = 0;       /* graphics-card decode errors in a row */
	const char *gpu_failed = NULL; /* why the graphics card was given up mid-session */

	if (!format || !packet || !frame || !gpu_copy)
		goto done;

	struct interrupt_ctx ic = {
		.e = e,
		.deadline_ns = now_ns(e) + 15ull * NS_PER_S,
	};
	format->interrupt_callback.callback = interrupt_cb;
	format->interrupt_callback.opaque = &ic;

	AVDictionary *options = NULL;
	/* low-latency demux: no probing pause, no buffering pause */
	av_dict_set(&options, "fflags", "nobuffer", 0);
	av_dict_set(&options, "flags", "low_delay", 0);
	/* MediaMTX's MPEG-TS names its codecs up front; probing for seconds only delays the picture after a reconnect.
	 * The decoders pick up the rest (sizes, SPS/PPS) from the stream itself. */
	av_dict_set(&options, "analyzeduration", "500000", 0); /* microseconds */
	av_dict_set(&options, "probesize", "500000", 0);
	/* the srt protocol's own options: kept out of the URL (see build_urls) */
	av_dict_set(&options, "streamid", streamid, 0);
	if (*e->passphrase)
		av_dict_set(&options, "passphrase", e->passphrase, 0);

	if (avformat_open_input(&format, url, NULL, &options) < 0) {
		av_dict_free(&options);
		if (log_failures)
			eng_log(e, CTD_LOG_WARNING, "couldn't open %s", shown_url); /* never the credentials */
		goto done;
	}
	av_dict_free(&options);

	if (avformat_find_stream_info(format, NULL) < 0)
		goto done;

	for (unsigned i = 0; i < format->nb_streams; i++) {
		const AVCodecParameters *par = format->streams[i]->codecpar;
		if (par->codec_type == AVMEDIA_TYPE_VIDEO && video_index < 0)
			video_index = (int)i;
		if (par->codec_type == AVMEDIA_TYPE_AUDIO && audio_index < 0)
			audio_index = (int)i;
	}
	if (e->audio_only) {
		/* the demuxer drops video packets, so they are never read or decoded */
		if (video_index >= 0)
			format->streams[video_index]->discard = AVDISCARD_ALL;
		video_index = -1;
		if (audio_index < 0 && log_failures)
			eng_log(e, CTD_LOG_WARNING, "stream has no audio track");
	}
	if (e->no_audio && audio_index >= 0) {
		/* A6, video only: the demuxer drops audio packets */
		format->streams[audio_index]->discard = AVDISCARD_ALL;
		audio_index = -1;
	}
	if (video_index < 0 && audio_index < 0)
		goto done;

	/* a new session is a new audio timeline (A2, A3), and LTC starts over (S5) */
	e->hold_count = 0;
	reset_timecode(e);
	e->late_video_set = e->late_audio_set = false;
	e->lock_offset_ns = e->lock_rate_ns_per_s = e->lock_pending_ns = 0.0;
	eng_store_long(&e->lock_ppm, 0);

	/* decoders */
	if (video_index >= 0) {
		const AVCodecParameters *par = format->streams[video_index]->codecpar;
		const AVCodec *codec = avcodec_find_decoder(par->codec_id);
		if (codec)
			vdec = open_video_decoder(e, codec, par, format->streams[video_index]->time_base, &on_gpu);
	}
	if (audio_index >= 0) {
		const AVCodecParameters *par = format->streams[audio_index]->codecpar;
		const AVCodec *codec = avcodec_find_decoder(par->codec_id);
		if (codec) {
			adec = avcodec_alloc_context3(codec);
			if (adec && avcodec_parameters_to_context(adec, par) >= 0) {
				adec->pkt_timebase = format->streams[audio_index]->time_base;
				if (avcodec_open2(adec, codec, NULL) < 0)
					avcodec_free_context(&adec);
			} else {
				avcodec_free_context(&adec);
			}
		}
	}
	if (!vdec && !adec)
		goto done;

	eng_log(e, CTD_LOG_INFO, "connected: video=%s%s%s audio=%s", vdec ? vdec->codec->name : "none",
		on_gpu ? " on " : "", on_gpu ? hw_decode_name(&e->hw) : "", adec ? adec->codec->name : "none");
	eng_store_long(&e->video_device, on_gpu ? (long)e->hw.type : 0);
	note_connected_input(e, vdec, adec);
	/* the stream's own frame rate, for "low frame rate" (6.2) */
	if (video_index >= 0) {
		AVRational rate = format->streams[video_index]->avg_frame_rate;
		if (!rate.num || !rate.den)
			rate = format->streams[video_index]->r_frame_rate;
		const double fps = rate.num && rate.den ? av_q2d(rate) : 0.0;
		eng_store_long(&e->nominal_fps_x100, fps > 1 && fps <= 240 ? (long)(fps * 100.0) : 0);
	}
	e->have_checksum = false;
	e->picture_same_since_ns = 0;
	eng_store_long(&e->frozen_ms, 0);
	connected = true;

	while (!stopping(e) && !eng_load_bool(&e->reconnect_now)) {
		/* every read gets a fresh stall deadline */
		ic.deadline_ns = now_ns(e) + e->stall_ns;
		int err = av_read_frame(format, packet);
		if (err < 0)
			break; /* EOF, error, or interrupt: reconnect */
		if (ic.timed_out) {
			/* After a stall the MPEG-TS demuxer flushes the half-assembled frame it was holding: seconds old,
			 * and it would replace the loss picture with a stale frame. */
			av_packet_unref(packet);
			break;
		}

		AVStream *stream = format->streams[packet->stream_index];
		clock.soft = eng_load_bool(&e->soft_correction); /* the setting applies without reconnecting */

		/* SPEC R2: a jump in the stream's timeline flushes the decoders and starts a new clock, without reconnecting */
		const int track = packet->stream_index == video_index   ? 0
				  : packet->stream_index == audio_index ? 1
									: -1;
		const char *why = track >= 0 ? discontinuity(&timelines[track], packet, stream->time_base) : NULL;
		if (why) {
			eng_log(e, CTD_LOG_INFO, "discontinuity on the %s (%s): flushing the decoders, new clock",
				track ? "audio" : "video", why);
			if (vdec)
				avcodec_flush_buffers(vdec);
			if (adec)
				avcodec_flush_buffers(adec);
			clock.established = false;
			need_keyframe = true;
			timelines[1 - track].last_dts_ns = AV_NOPTS_VALUE;
			eng_inc_long(&e->discontinuities);
			reset_timecode(e);
		}
		/* SPEC R3: video starts at a keyframe, so the picture never begins as a smear; audio plays at once */
		if (packet->stream_index == video_index && need_keyframe) {
			if (!(packet->flags & AV_PKT_FLAG_KEY)) {
				av_packet_unref(packet);
				continue;
			}
			need_keyframe = false;
		}

		if (packet->stream_index == video_index && vdec) {
			/* 6.2 "decoder falling behind": the time in the decoder (and the copy back from the graphics card),
			 * not in handing frames out */
			uint64_t t = now_ns(e);
			err = avcodec_send_packet(vdec, packet);
			uint64_t decode_ns = now_ns(e) - t;
			int decoded = 0;
			while (err >= 0) {
				t = now_ns(e);
				err = avcodec_receive_frame(vdec, frame);
				if (err < 0)
					break;
				decoded++;
				gpu_errors = 0;
				const AVFrame *picture = frame;
				if (on_gpu && hw_decode_on_gpu(&e->hw, frame)) {
					if (!hw_decode_download(gpu_copy, frame)) {
						gpu_failed = "copying a picture back from the graphics card failed";
						av_frame_unref(frame);
						break;
					}
					picture = gpu_copy;
				} else if (on_gpu && e->hw.refused && !refusal_logged) {
					/* the graphics card turned this stream down (a profile or size it can't do) */
					const char *profile = avcodec_profile_name(vdec->codec_id, vdec->profile);
					eng_log(e, CTD_LOG_INFO,
						"%s can't decode this stream (%s%s%s, %dx%d): decoding it on the CPU",
						hw_decode_name(&e->hw), vdec->codec->name, profile ? " " : "",
						profile ? profile : "", frame->width, frame->height);
					refusal_logged = true;
					eng_store_long(&e->video_device, 0);
				}

				decode_ns += now_ns(e) - t;
				measure_picture(e, picture);
				const int64_t pts_ns = frame_time_ns(frame, packet->dts, stream->time_base);
				const uint64_t timestamp = stamp_frame(e, &clock, pts_ns);
				note_lateness(&e->late_video_ns, &e->late_video_set, now_ns(e), timestamp); /* A3 */
				read_sei_timecode(e, frame, pts_ns, timestamp);                             /* S4 */
				check_picture(e, picture, timestamp);                                       /* S16 */
				put_picture(e, picture, timestamp); /* now, or after the delay (S1) */
				av_frame_unref(frame);
				av_frame_unref(gpu_copy);
			}
			if (decoded) {
				const double per_frame_ms = (double)decode_ns / 1e6 / decoded;
				e->decode_avg_ms = e->decode_avg_ms > 0 ? e->decode_avg_ms * 0.9 + per_frame_ms * 0.1
									: per_frame_ms;
				eng_store_long(&e->decode_us, (long)(e->decode_avg_ms * 1000.0));
			}
			/* errors other than "send more" mean the decoder failed; on the graphics card, too many end it */
			if (on_gpu && err < 0 && err != AVERROR(EAGAIN) && err != AVERROR_EOF &&
			    ++gpu_errors >= GPU_ERROR_LIMIT)
				gpu_failed = "the graphics card kept failing to decode";
		} else if (packet->stream_index == audio_index && adec) {
			/* A5: start the audio over (decoder, resampler, held-back audio) without touching video */
			if (eng_exchange_long(&e->reset_audio, 0)) {
				avcodec_flush_buffers(adec);
				audio_norm_free(&norm);
				e->hold_count = 0;
				e->late_audio_set = false;
				reset_timecode(e);
				eng_log(e, CTD_LOG_INFO, "audio reset, as asked");
			}
			if (avcodec_send_packet(adec, packet) == 0) {
				while (avcodec_receive_frame(adec, frame) == 0) {
					/* A4: a sender that labels 44.1 kHz audio as 48 kHz plays at the wrong pitch */
					const long override_rate = eng_load_long(&e->rate_override);
					if (override_rate > 0)
						frame->sample_rate = (int)override_rate;
					/* S5: LTC at the audio's own rate, before resampling (and out of the program audio) */
					read_ltc(e, frame, frame_time_ns(frame, packet->dts, stream->time_base),
						 &clock);
					/* SPEC R7: always 48 kHz stereo float planar, whatever arrives */
					const uint8_t *planes[2];
					bool changed;
					/* R5, plus A3's audio-only steering and S1's audio delay moving */
					const double frame_s = frame->sample_rate > 0
								       ? (double)frame->nb_samples / frame->sample_rate
								       : 0.0;
					const double delay_rate = step_audio_delay(e, frame_s);
					audio_norm_set_compensation(&norm, clock.correction_ns_per_s +
										   e->lock_rate_ns_per_s + delay_rate);
					const int samples = audio_norm_convert(&norm, frame, planes, &changed);
					if (changed)
						eng_log(e, CTD_LOG_INFO, "audio format changed %s → %s",
							norm.previous_description, norm.description);
					if (samples > 0) {
						const bool fresh_clock = !clock.established;
						const uint64_t stamped = stamp_frame(e, &clock,
										     frame_time_ns(frame, packet->dts,
												   stream->time_base));
						const uint64_t now = now_ns(e);
						note_lateness(&e->late_audio_ns, &e->late_audio_set, now, stamped);
						check_sound(e, (const float *)planes[0], (const float *)planes[1],
							    samples, stamped); /* S16 */
						const uint64_t timestamp =
							(uint64_t)((int64_t)stamped + (int64_t)lock_audio(e, frame_s) +
								   (int64_t)e->audio_delay_ns);
						/* a snap or a new clock is a new timeline: no hole to fill across it */
						const long resyncs = eng_load_long(&e->resyncs);
						const bool new_timeline = fresh_clock || resyncs != e->seen_resyncs;
						e->seen_resyncs = resyncs;
						const float *const out[2] = {(const float *)planes[0],
									     (const float *)planes[1]};
						emit_audio(e, out, samples, timestamp, new_timeline);
						note_audio_frame(e, frame->sample_rate, frame->ch_layout.nb_channels);
						eng_inc_long(&e->audio_frames);
					}
					av_frame_unref(frame);
				}
			}
		}
		av_packet_unref(packet);
		release_delayed(e); /* S2 */
		check_tick(e);      /* S16 */
		if (gpu_failed)
			break; /* reconnect, decoding on the CPU */
	}

done:
	if (vdec)
		avcodec_free_context(&vdec);
	if (gpu_failed) {
		snprintf(e->hw.why, sizeof(e->hw.why), "%s: %s", hw_decode_name(&e->hw), gpu_failed);
		note_gpu_problem(e, true);
		if (e->decoder == CTD_DECODER_HARDWARE)
			e->hw_skip_next = true;
		hw_decode_free(&e->hw); /* the device may be lost (a driver reset): open a fresh one next time */
	}
	av_frame_free(&gpu_copy);
	if (adec)
		avcodec_free_context(&adec);
	audio_norm_free(&norm);
	if (format)
		avformat_close_input(&format);
	av_packet_free(&packet);
	av_frame_free(&frame);
	/* the picture stays as it is: check_signal applies the loss mode once the hold time has passed */
	return connected;
}

/* ---------------------------------------------------------------- worker --- */

void ctd_engine_run(struct ctd_engine *e)
{
	unsigned failures = 0; /* failed connects in a row */

	/* the picture already up (a last frame, or the card) stays until the new stream arrives or the hold time passes */
	e->signal_ns = now_ns(e);
	eng_store_i64(&e->last_media_ns, (int64_t)e->signal_ns);
	e->had_signal = false;
	e->loss_applied = false;
	eng_store_long(&e->state, CTD_FEED_CONNECTING);
	if (e->cb.state)
		e->cb.state(e->cb.opaque, CTD_FEED_CONNECTING);
	/* a new engine (new settings, or the host started) gives the graphics card another chance */
	e->hw.format = AV_PIX_FMT_NONE;
	e->hw_given_up = false;
	e->hw_skip_next = false;
	e->hw_logged[0] = '\0';

	while (!stopping(e)) {
		bool retry_now = false;
		if (*e->path) {
			char url[1024], streamid[1024], shown[1024];
			build_urls(e, url, sizeof(url), streamid, sizeof(streamid), shown, sizeof(shown));
			/* a feed that isn't up yet is retried forever, quietly: its first failure is logged, then one line a
			 * minute until it connects */
			const unsigned per_minute =
				e->reconnect_s > 0 && e->reconnect_s < 60 ? 60u / (unsigned)e->reconnect_s : 1u;
			const bool log_failure = failures % per_minute == 0;
			const bool connected = run_session(e, url, streamid, shown, log_failure);
			memset(streamid, 0, sizeof(streamid)); /* it may hold a password */
			failures = connected ? 0 : failures + 1;
			/* The first try after a session ends, or after the first failure, goes at once. MediaMTX keeps a
			 * publisher that vanished (a phone losing Wi-Fi) on the path until it returns, and then cuts every
			 * reader, including one that just attached to the dead publisher: waiting out the reconnect delay
			 * there only adds a second to the outage. Failures after that wait, so a dead server isn't hammered. */
			retry_now = failures <= 1;

			if (!stopping(e)) {
				eng_inc_long(&e->reconnects);
				const long state = eng_load_long(&e->state);
				if (state == CTD_FEED_LIVE || state == CTD_FEED_STALLED)
					set_state(e, CTD_FEED_RECONNECTING);
				if (eng_load_bool(&e->reconnect_now))
					eng_log(e, CTD_LOG_INFO, "reconnecting now, as asked");
				else if (connected)
					eng_log(e, CTD_LOG_INFO, "reconnecting");
				else if (failures == 1)
					eng_log(e, CTD_LOG_INFO,
						"retrying every %ds; logging once a minute until it connects",
						e->reconnect_s);
				else if (log_failure)
					eng_log(e, CTD_LOG_INFO, "still retrying (%u attempts)", failures);
			}
		}

		/* backoff, cut short by stopping and skipped for "Reconnect now"; the loss picture keeps being applied
		 * meanwhile */
		eng_store_bool(&e->reconnect_now, false);
		for (int waited = 0; !retry_now && (waited < e->reconnect_s * 1000 || !*e->path); waited += 100) {
			if (stopping(e) || eng_exchange_long(&e->reconnect_now, 0))
				break;
			av_usleep(100000);
			check_signal(e);
		}
	}
	hw_decode_free(&e->hw);
	delay_queue_clear(&e->delay); /* stopping: what was still delayed never plays */
	publish_delay(e);
}

/* ------------------------------------------------------------------- API --- */

struct ctd_engine *ctd_engine_create(const struct ctd_engine_config *config, const struct ctd_engine_callbacks *cb)
{
	if (!cb || !cb->now_ns)
		return NULL;
	struct ctd_engine *e = av_mallocz(sizeof(*e));
	if (!e)
		return NULL;
	e->host = copy_string(config->host && *config->host ? config->host : "127.0.0.1");
	e->path = copy_string(config->path);
	e->gpu_adapter = copy_string(config->gpu_adapter);
	e->read_user = copy_string(config->read_user);
	e->read_pass = copy_string(config->read_pass);
	e->passphrase = copy_string(config->passphrase);
	if (!e->host || !e->path || !e->gpu_adapter || !e->read_user || !e->read_pass || !e->passphrase) {
		ctd_engine_destroy(e);
		return NULL;
	}
	e->port = config->port;
	e->latency_ms = config->latency_ms;
	e->drift_cap_ns = (int64_t)config->drift_cap_ms * 1000000;
	e->reconnect_s = config->reconnect_s > 0 ? config->reconnect_s : 1;
	e->stall_s = config->stall_s > 0 ? config->stall_s : 1;
	e->stall_ns = (uint64_t)e->stall_s * NS_PER_S;
	e->decoder = config->decoder;
	e->audio_only = config->audio_only;
	e->no_audio = config->no_audio && !config->audio_only;
	e->fill_gaps = 1;      /* A2: on by default */
	e->delay_cap_mb = 512; /* S2 */
	e->channel_override = -1;
	e->limit_logged_ms = -1;
	e->cb = *cb;
	e->hold_ms = 1500;
	e->soft_correction = 1;
	e->target_ms = LONG_MIN;
	e->hw.format = AV_PIX_FMT_NONE;
	return e;
}

void ctd_engine_destroy(struct ctd_engine *e)
{
	if (!e)
		return;
	hw_decode_free(&e->hw);
	delay_queue_clear(&e->delay);
	av_free(e->host);
	av_free(e->path);
	av_free(e->gpu_adapter);
	av_free(e->work[0]);
	av_free(e->work[1]);
	av_free(e->ltc_samples);
	av_free(e->read_user);
	if (e->read_pass)
		memset(e->read_pass, 0, strlen(e->read_pass));
	av_free(e->read_pass);
	if (e->passphrase)
		memset(e->passphrase, 0, strlen(e->passphrase));
	av_free(e->passphrase);
	av_free(e);
}

void ctd_engine_stop(struct ctd_engine *e)
{
	eng_store_bool(&e->stopping, true);
}

void ctd_engine_reconnect_now(struct ctd_engine *e)
{
	eng_store_bool(&e->reconnect_now, true);
}

void ctd_engine_set_loss(struct ctd_engine *e, enum ctd_loss_mode mode, int hold_ms)
{
	eng_store_long(&e->loss_mode, (long)mode);
	eng_store_long(&e->hold_ms, hold_ms > 0 ? hold_ms : 0);
}

void ctd_engine_set_soft_correction(struct ctd_engine *e, bool on)
{
	eng_store_bool(&e->soft_correction, on);
}

void ctd_engine_set_audio_repair(struct ctd_engine *e, enum ctd_channels channels, bool fill_gaps, bool lock_to_video,
				 int rate_override)
{
	eng_store_long(&e->channels, (long)channels);
	eng_store_bool(&e->fill_gaps, fill_gaps);
	eng_store_bool(&e->lock_to_video, lock_to_video);
	eng_store_long(&e->rate_override, rate_override > 0 ? rate_override : 0);
}

void ctd_engine_set_delay(struct ctd_engine *e, int video_ms, int audio_ms, int cap_mb)
{
	if (video_ms != eng_load_long(&e->video_delay_target_ms) ||
	    audio_ms != eng_load_long(&e->audio_delay_target_ms))
		eng_store_bool(&e->delay_settled, false);
	eng_store_long(&e->video_delay_target_ms, video_ms < 0 ? 0 : video_ms > DELAY_MAX_MS ? DELAY_MAX_MS : video_ms);
	eng_store_long(&e->audio_delay_target_ms, audio_ms < 0 ? 0 : audio_ms > DELAY_MAX_MS ? DELAY_MAX_MS : audio_ms);
	eng_store_long(&e->delay_cap_mb, cap_mb >= 16 ? cap_mb : 512);
}

void ctd_engine_set_group_delay(struct ctd_engine *e, int ms, bool jump)
{
	const long clamped = ms < 0 ? 0 : ms > 5000 ? 5000 : ms;
	/* not settled until the worker has moved (it says so again at the next frame) */
	if (eng_exchange_long(&e->group_delay_ms, clamped) != clamped)
		eng_store_bool(&e->delay_settled, false);
	if (jump) {
		eng_store_bool(&e->group_jump_video, true);
		eng_store_bool(&e->group_jump_audio, true);
	}
}

void ctd_engine_start_sync_check(struct ctd_engine *e)
{
	eng_store_bool(&e->check_asked, true);
}

void ctd_engine_set_timecode(struct ctd_engine *e, int source)
{
	eng_store_long(&e->timecode_source, source >= CTD_TIMECODE_NONE && source <= CTD_TIMECODE_SEI ? source : 0);
}

void ctd_engine_reset_audio(struct ctd_engine *e)
{
	eng_store_bool(&e->reset_audio, true);
}

void ctd_engine_redraw_loss(struct ctd_engine *e)
{
	eng_store_bool(&e->redraw_loss, true);
}

void ctd_engine_get_stats(struct ctd_engine *e, struct ctd_engine_stats *out)
{
	memset(out, 0, sizeof(*out));
	out->state = (enum ctd_feed_state)eng_load_long(&e->state);
	out->video_device = (int)eng_load_long(&e->video_device);
	out->reconnects = eng_load_long(&e->reconnects);
	out->resyncs = eng_load_long(&e->resyncs);
	out->discontinuities = eng_load_long(&e->discontinuities);
	out->video_frames = eng_load_long(&e->video_frames);
	out->audio_frames = eng_load_long(&e->audio_frames);
	out->lateness_ms = eng_load_long(&e->lateness_ms);
	const long target = eng_load_long(&e->target_ms);
	out->target_set = target != LONG_MIN;
	out->target_ms = out->target_set ? target : 0;
	out->correction_ppm = eng_load_long(&e->correction_ppm);
	out->fps_x100 = eng_load_long(&e->fps_x100);
	out->last_media_ns = (uint64_t)eng_load_i64(&e->last_media_ns);
	/* audio levels go stale when the audio stops: report silence after 2 s */
	const bool audio_recent = now_ns(e) - (uint64_t)eng_load_i64(&e->last_audio_ns) < 2 * NS_PER_S;
	for (int c = 0; c < 2; c++)
		out->peak_db[c] = audio_recent ? eng_load_long(&e->peak_db_x100[c]) / 100.0 : -120.0;
	out->clipped_percent = audio_recent ? eng_load_long(&e->clipped_x10000) / 10000.0 : 0.0;
	out->frozen_ms = eng_load_long(&e->frozen_ms);
	out->decode_ms = eng_load_long(&e->decode_us) / 1000.0;
	out->nominal_fps = eng_load_long(&e->nominal_fps_x100) / 100.0;
	out->gaps_filled = eng_load_long(&e->gaps_filled);
	out->gap_ms_filled = eng_load_long(&e->gap_ms_filled);
	out->audio_lock_ppm = eng_load_long(&e->lock_ppm);
	out->video_delay_ms = eng_load_long(&e->video_delay_ms);
	out->audio_delay_ms = eng_load_long(&e->audio_delay_ms);
	out->video_delay_target_ms = eng_load_long(&e->video_delay_target_ms);
	out->audio_delay_target_ms = eng_load_long(&e->audio_delay_target_ms);
	out->video_delay_limit_ms = eng_load_long(&e->video_delay_limit_ms);
	out->delay_memory_mb = eng_load_long(&e->delay_kb) / 1024.0;
	out->group_delay_ms = eng_load_long(&e->group_delay_ms);
	out->sync_check.state = (enum ctd_sync_check_state)eng_load_long(&e->check_state);
	out->sync_check.offset_ms = eng_load_long(&e->check_offset_x10) / 10.0;
	out->sync_check.pairs = (int)eng_load_long(&e->check_pairs);
	out->delay_settled = eng_load_bool(&e->delay_settled);

	/* section 8.2: the timecode fields as one set (the worker writes them under tc_seq) */
	struct ctd_timecode *tc = &out->timecode;
	int64_t seen_ns = 0;
	for (int tries = 0; tries < 1000; tries++) {
		const long seq = eng_load_long(&e->tc_seq);
		if (seq & 1)
			continue;
		const long packed = eng_load_long(&e->tc_packed);
		tc->hours = (int)(packed >> 24 & 0xff);
		tc->minutes = (int)(packed >> 16 & 0xff);
		tc->seconds = (int)(packed >> 8 & 0xff);
		tc->frames = (int)(packed & 0xff);
		tc->drop_frame = eng_load_long(&e->tc_drop) != 0;
		tc->user_bits = (uint32_t)eng_load_long(&e->tc_user_bits);
		tc->fps = eng_load_long(&e->tc_fps_x1000) / 1000.0;
		tc->stream_ns = eng_load_i64(&e->tc_stream_ns);
		tc->clock_ns = (uint64_t)eng_load_i64(&e->tc_clock_ns);
		seen_ns = eng_load_i64(&e->tc_seen_ns);
		if (eng_load_long(&e->tc_seq) == seq)
			break;
	}
	const uint64_t now = now_ns(e);
	const long source = eng_load_long(&e->timecode_source);
	tc->locked = source && seen_ns && now - (uint64_t)seen_ns < NS_PER_S;
	const bool ltc = source >= CTD_TIMECODE_LTC_LEFT && source <= CTD_TIMECODE_LTC_LAST;
	tc->level_db = ltc ? eng_load_long(&e->tc_level_x100) / 100.0 : -120.0;
	tc->error_percent = ltc ? eng_load_long(&e->tc_error_x10000) / 10000.0 : 0.0;
	const int64_t detected_ns = eng_load_i64(&e->tc_detected_ns);
	tc->detected_channel = !source && detected_ns && now - (uint64_t)detected_ns < 3 * NS_PER_S
				       ? (int)eng_load_long(&e->tc_detected)
				       : 0;
}
