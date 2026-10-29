/*
 * The CTD Live engine (SPEC 10.1, E1): one SRT feed from MediaMTX, from connecting to timed, decoded frames.
 *
 * C and FFmpeg only, no libobs, so the OBS source and the ctd-live-probe CLI run the same code. It owns no threads
 * and no locks: ctd_engine_run() is the worker loop and blocks until ctd_engine_stop(); the host runs it on a thread
 * of its own. Everything comes out through callbacks, all called on that thread:
 *
 * - video: a decoded picture (CPU memory) and its timestamp on the host's clock;
 * - audio: always 48 kHz stereo float planar (R7), timestamped on the same clock;
 * - loss: the feed has been silent for the hold time, so the host should apply the loss mode (R10 to R13);
 * - state, input and log: for the host's stats and log;
 * - tick: called often while the engine waits, so the host can do small periodic work on the same thread.
 *
 * Timestamps are on the host's clock (now_ns), which for OBS must be os_gettime_ns(): OBS schedules frames by it.
 */

#pragma once

#include <libavutil/frame.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SPEC R10 */
enum ctd_feed_state {
	CTD_FEED_CONNECTING,   /* no media yet since the engine started */
	CTD_FEED_LIVE,         /* media flowing */
	CTD_FEED_STALLED,      /* no media for longer than the hold time; the connection may still be open */
	CTD_FEED_RECONNECTING, /* the connection ended; dialing again */
	CTD_FEED_NO_SIGNAL,    /* still nothing after the hold time plus the stall threshold */
};

/* where the video is decoded (SPEC R19) */
enum ctd_decoder_mode {
	CTD_DECODER_AUTO,     /* the graphics card, until it fails once; then the CPU */
	CTD_DECODER_SOFTWARE, /* the CPU */
	CTD_DECODER_HARDWARE, /* the graphics card; after a failure one connection on the CPU, then the card again */
};

/* what the picture does once the feed has been silent for the hold time (SPEC R11); the host carries it out */
enum ctd_loss_mode {
	CTD_LOSS_CARD,        /* hold the last frame, then show the reconnecting card */
	CTD_LOSS_HOLD,        /* keep holding the last frame */
	CTD_LOSS_TRANSPARENT, /* go see-through */
};

/* A1: what the two output channels carry */
enum ctd_channels {
	CTD_CHANNELS_STEREO,       /* as is */
	CTD_CHANNELS_LEFT,         /* left on both */
	CTD_CHANNELS_RIGHT,        /* right on both */
	CTD_CHANNELS_MONO,         /* (left + right) / 2 on both */
	CTD_CHANNELS_SWAP,         /* left and right swapped */
	CTD_CHANNELS_INVERT_RIGHT, /* right channel's polarity inverted */
};

/* S4: where a source's timecode comes from. 1 to 8 are LTC on that audio channel (1 is left, 2 right). */
enum ctd_timecode_source {
	CTD_TIMECODE_NONE = 0,
	CTD_TIMECODE_LTC_LEFT = 1,
	CTD_TIMECODE_LTC_RIGHT = 2,
	CTD_TIMECODE_LTC_LAST = 8,
	CTD_TIMECODE_SEI = 9, /* H.264/HEVC SEI timecode (SMPTE 12M), when the stream carries it */
};

/* the feed's timecode (S5, S6), for the stats and the sync groups */
struct ctd_timecode {
	bool locked; /* a timecode decoded within the last second */
	int hours, minutes, seconds, frames;
	bool drop_frame;
	double fps; /* detected: 23.976, 24, 25, 29.97 or 30; 0 until known */
	uint32_t user_bits;
	double level_db;      /* the LTC signal's peak (-120 for SEI or none) */
	double error_percent; /* LTC words that didn't decode, since it locked */
	/* S6: the stream time it is pinned to (where the LTC sync word ended, or the picture carrying the SEI), and that
	 * moment on the host's clock before any delay (0 if the clock wasn't running yet) */
	int64_t stream_ns;
	uint64_t clock_ns;
	int detected_channel; /* S7: with no timecode input set, the channel (1 to 8) carrying LTC, or 0 */
};

/* S16, the sync check */
enum ctd_sync_check_state {
	CTD_SYNC_CHECK_IDLE,
	CTD_SYNC_CHECK_RUNNING, /* 10 s of listening and watching */
	CTD_SYNC_CHECK_DONE,    /* offset_ms is the answer */
	CTD_SYNC_CHECK_FAILED,  /* too few flash and beep pairs */
};

struct ctd_sync_check {
	enum ctd_sync_check_state state;
	double offset_ms; /* the sound comes this much after the picture (negative: before) */
	int pairs;        /* flashes matched to beeps */
};

enum ctd_log_level {
	CTD_LOG_ERROR,
	CTD_LOG_WARNING,
	CTD_LOG_INFO,
	CTD_LOG_DEBUG,
};

const char *ctd_feed_state_name(enum ctd_feed_state state);

struct ctd_engine_config {
	const char *host; /* the MediaMTX server; copied */
	int port;         /* its SRT port */
	const char *path; /* the stream (path) to read; copied. Empty: the engine idles */
	int latency_ms;   /* SRT latency */
	int drift_cap_ms; /* R4: lateness past this snaps to the live edge */
	int reconnect_s;  /* wait between connection attempts */
	int stall_s;      /* a read silent this long ends the session */
	enum ctd_decoder_mode decoder;
	const char *gpu_adapter; /* the graphics card to decode on (d3d12va's adapter index), or NULL; copied */
	/* H2: MediaMTX read credentials (streamid read:<path>:<user>:<pass>) and the SRT passphrase; NULL or empty for
	 * none. Copied, never logged. */
	const char *read_user;
	const char *read_pass;
	const char *passphrase;
	bool audio_only; /* never read, decode or output video */
	bool no_audio;   /* A6: never read, decode or output audio (video only) */
};

/* what the feed carries, for the host's stats (R16) */
struct ctd_input {
	char video_codec[32];
	int width;
	int height;
	char audio_codec[32];
	int sample_rate;
	int channels;
};

struct ctd_engine_callbacks {
	void *opaque;
	/* required: the host's monotonic clock, in nanoseconds */
	uint64_t (*now_ns)(void);
	/* a decoded picture (in CPU memory) to show at timestamp_ns. Return false if it wasn't used (an unsupported pixel
	 * format), so it doesn't count as media. */
	bool (*video)(void *opaque, const AVFrame *picture, uint64_t timestamp_ns);
	/* frames samples of 48 kHz stereo float planar audio to play at timestamp_ns */
	void (*audio)(void *opaque, const float *const planes[2], int frames, uint64_t timestamp_ns);
	/* the feed has been silent for the hold time (or the loss mode changed, or a redraw was asked for): apply mode.
	 * waiting: nothing has arrived since the engine started (a "waiting" card rather than a "lost" one). */
	void (*loss)(void *opaque, enum ctd_loss_mode mode, bool waiting);
	void (*state)(void *opaque, enum ctd_feed_state state);
	void (*input)(void *opaque, const struct ctd_input *input);
	void (*log)(void *opaque, enum ctd_log_level level, const char *message);
	void (*tick)(void *opaque);
};

/* numbers for the stats line (R6, R16); every field is read atomically, together only roughly */
struct ctd_engine_stats {
	enum ctd_feed_state state;
	int video_device;     /* the AVHWDeviceType decoding the video, or 0 for the CPU */
	long reconnects;      /* sessions ended (or attempts failed) */
	long resyncs;         /* hard snaps to the live edge (R4) */
	long discontinuities; /* timeline jumps handled without reconnecting (R2) */
	long video_frames;
	long audio_frames;
	long lateness_ms; /* averaged distance from the live clock; positive means late */
	bool target_set;
	long target_ms;         /* the lateness soft correction steers to (R5) */
	long correction_ppm;    /* soft correction's rate, parts per million; positive slows playback */
	long fps_x100;          /* video frames a second, measured, times 100 */
	uint64_t last_media_ns; /* host clock: the last frame handed out, or when the engine started */
	/* SPEC 6.2 measurements, over the last second unless said otherwise */
	double peak_db[2];      /* each output channel's peak, dBFS (-120 for silence or no audio) */
	double clipped_percent; /* samples at full scale */
	long frozen_ms;         /* how long the picture has been identical while frames kept coming (0: changing) */
	double decode_ms;       /* average time to decode one video frame */
	double nominal_fps;     /* the stream's own frame rate, or 0 when it doesn't say */
	long gaps_filled;       /* A2: holes in the audio filled with silence, since the engine started */
	long gap_ms_filled;     /* their total length */
	long audio_lock_ppm;    /* A3: the audio-only correction toward video, parts per million */
	/* S1, S2: the delays applied now (they move toward the settings without a jump), the settings, the most video
	 * delay the memory cap holds at the current picture size (0 until a picture has been seen) and what the delay
	 * queue holds */
	long video_delay_ms;
	long audio_delay_ms;
	long video_delay_target_ms;
	long audio_delay_target_ms;
	long video_delay_limit_ms;
	double delay_memory_mb;
	long group_delay_ms;              /* S14: the sync group's delay, included in the delays applied */
	bool delay_settled;               /* the delays applied have reached where they are heading */
	struct ctd_timecode timecode;     /* section 8.2 */
	struct ctd_sync_check sync_check; /* S16 */
};

struct ctd_engine;

struct ctd_engine *ctd_engine_create(const struct ctd_engine_config *config, const struct ctd_engine_callbacks *cb);
void ctd_engine_destroy(struct ctd_engine *engine);

/* The worker loop: connect, play, reconnect, until ctd_engine_stop(). Run it on its own thread; once only. */
void ctd_engine_run(struct ctd_engine *engine);
/* Any thread: makes ctd_engine_run() return soon (within about 100 ms, or when FFmpeg's I/O next checks in). */
void ctd_engine_stop(struct ctd_engine *engine);
/* Any thread: ends the session and dials again at once, without the reconnect wait (R17). */
void ctd_engine_reconnect_now(struct ctd_engine *engine);

/* Any thread: the settings that apply without reconnecting. */
void ctd_engine_set_loss(struct ctd_engine *engine, enum ctd_loss_mode mode, int hold_ms);
void ctd_engine_set_soft_correction(struct ctd_engine *engine, bool on);
/* Any thread, section 7: audio repair that applies without reconnecting. fill_gaps (A2) is on by default;
 * rate_override (A4) treats the incoming audio as that sample rate (0: as labelled). */
void ctd_engine_set_audio_repair(struct ctd_engine *engine, enum ctd_channels channels, bool fill_gaps,
				 bool lock_to_video, int rate_override);
/* Any thread, S1 and S2: delay the video and the audio by 0 to 2000 ms each, holding at most cap_mb of pictures
 * (512 by default). While the feed is on screen they move there smoothly, never with a jump. */
void ctd_engine_set_delay(struct ctd_engine *engine, int video_ms, int audio_ms, int cap_mb);
/* Any thread, S9 to S14: the sync group's delay (0 to 5000 ms), on top of both delays. jump: the first lock, applied
 * at once even on air (the picture holds, the audio gap is filled with faded silence); otherwise it ramps like S1. */
void ctd_engine_set_group_delay(struct ctd_engine *engine, int ms, bool jump);
/* Any thread, S16: a 10 s sync check while the sync clip plays in front of the camera; the answer is in the stats. */
void ctd_engine_start_sync_check(struct ctd_engine *engine);
/* Any thread, S4: the timecode input (enum ctd_timecode_source). An LTC channel is taken out of the program audio: the
 * other channel plays on both sides. */
void ctd_engine_set_timecode(struct ctd_engine *engine, int source);
/* Any thread, A5: rebuilds the audio decoder and resampler at the next audio packet, without touching video. */
void ctd_engine_reset_audio(struct ctd_engine *engine);
/* Any thread: the loss picture should be applied again at the next check (the host's card changed). */
void ctd_engine_redraw_loss(struct ctd_engine *engine);

void ctd_engine_get_stats(struct ctd_engine *engine, struct ctd_engine_stats *out);

#ifdef __cplusplus
}
#endif
