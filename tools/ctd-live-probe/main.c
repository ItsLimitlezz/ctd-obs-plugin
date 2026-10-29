/*
 * ctd-live-probe (SPEC 10.1, E5): the CTD Live engine on the command line.
 *
 *   ctd-live-probe --host H --port P --path X [--seconds N] [--json] [bounds]
 *
 * Runs the same engine as the OBS source against one MediaMTX stream and prints, every second, the state, lateness,
 * soft correction, fps, audio rate, resyncs, reconnects and discontinuities; state changes and the engine's log as
 * they happen. With --json every line is one JSON object, for the test harness (test/).
 *
 * Bounds (any broken one makes the exit code 2):
 *   --live-within S      LIVE within S seconds of starting                     (T1, T2)
 *   --max-gap S          once LIVE, never out of LIVE for longer than S          (T6, T11)
 *   --max-lateness MS    the averaged lateness never beyond ±MS while LIVE
 *   --no-resyncs         no hard snaps to the live edge                         (T12)
 *   --monotonic          video timestamps strictly increasing                   (T3, R1)
 *   --min-gaps-filled N  at least N audio gaps filled with silence              (T15, A2)
 *   --reach-delays       by the end, the delays applied are the ones set        (T20, S1)
 *   --no-gaps-filled     no audio gap filled: a clean feed, or a delay change, never leaves holes (T20)
 *   --expect-tc-fps F    timecode locked at the end, at F fps (with --expect-drop: drop frame)  (T17, S5)
 *   --max-tc-errors PCT  LTC words that didn't decode, at most PCT percent                (T17, S8)
 *   --expect-detected N  LTC found on channel N while no timecode input is set          (T16, S7)
 *   --sync-check-at S --expect-av-offset MS [--av-tolerance MS]: a sync check started S seconds in measures the
 *                        publisher's A/V offset as MS (within 40 ms)                                   (T21, S16)
 * The delay queue never holds more than --delay-cap MB of pictures (T20, S2); that is always checked.
 * With --group PATH,PATH,... it runs those feeds as one sync group instead (group.c).
 * Exit codes: 0 pass, 1 usage or setup error, 2 a bound broken.
 */

#include "engine.h"
#include "eng-atomic.h"

#include <libavutil/time.h>

#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#else
#include <pthread.h>
#include <time.h>
#endif

/* ----------------------------------------------------------------- clock -- */

static uint64_t probe_now(void)
{
#ifdef _WIN32
	static LARGE_INTEGER frequency;
	if (!frequency.QuadPart)
		QueryPerformanceFrequency(&frequency);
	LARGE_INTEGER counter;
	QueryPerformanceCounter(&counter);
	return (uint64_t)((double)counter.QuadPart * 1e9 / (double)frequency.QuadPart);
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

/* ----------------------------------------------------------------- state -- */

static struct {
	bool json;
	uint64_t start_ns;
	volatile long stop; /* Ctrl+C */

	/* counted on the engine's thread, read on the main one */
	volatile long audio_samples;
	volatile long video_backwards; /* R1: a video timestamp not after the one before */
	int64_t last_video_ts;         /* engine thread only */

	/* bounds */
	double live_within_s; /* 0: not checked */
	long max_lateness_ms; /* 0: not checked */
	bool no_resyncs;
	long min_gaps_filled;
	bool reach_delays;
	bool no_gaps_filled;
	/* section 8.2 */
	double expect_tc_fps;
	bool expect_drop;
	double max_tc_errors; /* < 0: not checked */
	int expect_detected;
	/* S16 */
	double sync_check_at; /* 0: no sync check */
	double expect_av_offset_ms;
	double av_tolerance_ms;
	int detected_seen;
	struct ctd_timecode last_timecode; /* from the last stats line */
	double worst_delay_mb;
	bool monotonic;
	double max_gap_s;    /* 0: not checked */
	double first_live_s; /* -1 until LIVE */
	long worst_lateness_ms;
	/* gaps: from leaving LIVE (after the first LIVE) to LIVE again; engine thread writes, main thread reads at the end */
	double left_live_s; /* -1 while LIVE or before the first LIVE */
	double worst_gap_s;
	long gaps;
} probe = {.first_live_s = -1, .left_live_s = -1};

static double seconds_since_start(void)
{
	return (double)(probe_now() - probe.start_ns) / 1e9;
}

/* a JSON string body: quotes, backslashes and control characters escaped */
static void print_json_string(const char *s)
{
	putchar('"');
	for (; *s; s++) {
		const unsigned char c = (unsigned char)*s;
		if (c == '"' || c == '\\')
			printf("\\%c", c);
		else if (c < 0x20)
			printf("\\u%04x", c);
		else
			putchar(c);
	}
	putchar('"');
}

/* ------------------------------------------------------------- callbacks -- */

static bool on_video(void *opaque, const AVFrame *picture, uint64_t timestamp_ns)
{
	(void)opaque;
	(void)picture;
	if (probe.last_video_ts && (int64_t)timestamp_ns <= probe.last_video_ts)
		eng_inc_long(&probe.video_backwards);
	probe.last_video_ts = (int64_t)timestamp_ns;
	return true;
}

static void on_audio(void *opaque, const float *const planes[2], int frames, uint64_t timestamp_ns)
{
	(void)opaque;
	(void)planes;
	(void)timestamp_ns;
	eng_add_long(&probe.audio_samples, frames);
}

static void on_state(void *opaque, enum ctd_feed_state state)
{
	(void)opaque;
	const double t = seconds_since_start();
	if (state == CTD_FEED_LIVE) {
		if (probe.first_live_s < 0)
			probe.first_live_s = t;
		if (probe.left_live_s >= 0) {
			const double gap = t - probe.left_live_s;
			if (gap > probe.worst_gap_s)
				probe.worst_gap_s = gap;
			probe.gaps++;
			probe.left_live_s = -1;
		}
	} else if (probe.first_live_s >= 0 && probe.left_live_s < 0) {
		probe.left_live_s = t;
	}
	if (probe.json) {
		printf("{\"t\":%.3f,\"type\":\"state\",\"state\":", t);
		print_json_string(ctd_feed_state_name(state));
		printf("}\n");
	} else {
		printf("%8.3f  state %s\n", t, ctd_feed_state_name(state));
	}
	fflush(stdout);
}

static void on_input(void *opaque, const struct ctd_input *in)
{
	(void)opaque;
	const double t = seconds_since_start();
	if (probe.json) {
		printf("{\"t\":%.3f,\"type\":\"input\",\"video\":", t);
		print_json_string(in->video_codec);
		printf(",\"width\":%d,\"height\":%d,\"audio\":", in->width, in->height);
		print_json_string(in->audio_codec);
		printf(",\"sampleRate\":%d,\"channels\":%d}\n", in->sample_rate, in->channels);
	} else {
		printf("%8.3f  input %s %dx%d · %s %d Hz %d ch\n", t, in->video_codec, in->width, in->height,
		       in->audio_codec, in->sample_rate, in->channels);
	}
	fflush(stdout);
}

static void on_log(void *opaque, enum ctd_log_level level, const char *message)
{
	(void)opaque;
	static const char *const names[] = {"error", "warning", "info", "debug"};
	const double t = seconds_since_start();
	const char *name = level >= CTD_LOG_ERROR && level <= CTD_LOG_DEBUG ? names[level] : "info";
	if (probe.json) {
		printf("{\"t\":%.3f,\"type\":\"log\",\"level\":\"%s\",\"message\":", t, name);
		print_json_string(message);
		printf("}\n");
	} else {
		printf("%8.3f  %s: %s\n", t, name, message);
	}
	fflush(stdout);
}

/* ---------------------------------------------------------------- thread -- */

#ifdef _WIN32
static unsigned __stdcall engine_thread(void *engine)
{
	ctd_engine_run(engine);
	return 0;
}
#else
static void *engine_thread(void *engine)
{
	ctd_engine_run(engine);
	return NULL;
}
#endif

static void on_signal(int sig)
{
	(void)sig;
	eng_store_bool(&probe.stop, true);
}

/* ------------------------------------------------------------------ main -- */

static void usage(void)
{
	fprintf(stderr,
		"usage: ctd-live-probe --host HOST --port PORT --path PATH [--seconds N] [--json]\n"
		"                      [--user USER --pass PASS] [--passphrase PHRASE]\n"
		"                      [--latency MS] [--drift-cap MS] [--reconnect S] [--stall S] [--hold MS]\n"
		"                      [--decoder auto|cpu|gpu] [--audio-only] [--video-only] [--no-soft-correction]\n"
		"                      [--channels stereo|left|right|mono|swap|invert-right] [--no-fill-gaps]\n"
		"                      [--lock-audio] [--rate-override HZ] [--min-gaps-filled N]\n"
		"                      [--video-delay MS] [--audio-delay MS] [--delay-cap MB]\n"
		"                      [--change-delay-at S --video-delay-to MS --audio-delay-to MS] [--reach-delays]\n"
		"                      [--no-gaps-filled]\n"
		"                      [--timecode 0-9] [--expect-tc-fps F [--expect-drop]] [--max-tc-errors PCT]\n"
		"                      [--expect-detected N] [--sync-check-at S --expect-av-offset MS [--av-tolerance MS]]\n"
		"                      [--live-within S] [--max-gap S] [--max-lateness MS] [--no-resyncs] [--monotonic]\n");
}

int run_group(int argc, char **argv); /* group.c */

int main(int argc, char **argv)
{
	/* --group: several feeds in one sync group (T18, T19) */
	for (int i = 1; i < argc; i++)
		if (strcmp(argv[i], "--group") == 0)
			return run_group(argc, argv);

	struct ctd_engine_config config = {
		.host = "127.0.0.1",
		.port = 8890,
		.path = "",
		.latency_ms = 120,
		.drift_cap_ms = 700,
		.reconnect_s = 1,
		.stall_s = 3,
		.decoder = CTD_DECODER_SOFTWARE, /* the harness has no graphics card */
	};
	double seconds = 0;
	int hold_ms = 1500;
	bool soft = true;
	/* section 7 */
	enum ctd_channels channels = CTD_CHANNELS_STEREO;
	bool fill_gaps = true, lock_audio = false;
	int rate_override = 0;
	/* section 8.1 */
	int video_delay = 0, audio_delay = 0, delay_cap = 512;
	double change_delay_at = 0;
	int video_delay_to = -1, audio_delay_to = -1;
	int timecode = 0; /* enum ctd_timecode_source: 1 to 8 LTC on that channel, 9 SEI */
	probe.max_tc_errors = -1;
	probe.av_tolerance_ms = 40;

	for (int i = 1; i < argc; i++) {
		const char *arg = argv[i];
		const char *value = i + 1 < argc ? argv[i + 1] : NULL;
#define TAKES_VALUE                        \
	if (!value) {                      \
		usage();                   \
		return 1;                  \
	}                                  \
	i++
		if (strcmp(arg, "--host") == 0) {
			TAKES_VALUE;
			config.host = value;
		} else if (strcmp(arg, "--port") == 0) {
			TAKES_VALUE;
			config.port = atoi(value);
		} else if (strcmp(arg, "--path") == 0) {
			TAKES_VALUE;
			config.path = value;
		} else if (strcmp(arg, "--user") == 0) {
			TAKES_VALUE;
			config.read_user = value;
		} else if (strcmp(arg, "--pass") == 0) {
			TAKES_VALUE;
			config.read_pass = value;
		} else if (strcmp(arg, "--passphrase") == 0) {
			TAKES_VALUE;
			config.passphrase = value;
		} else if (strcmp(arg, "--seconds") == 0) {
			TAKES_VALUE;
			seconds = atof(value);
		} else if (strcmp(arg, "--latency") == 0) {
			TAKES_VALUE;
			config.latency_ms = atoi(value);
		} else if (strcmp(arg, "--drift-cap") == 0) {
			TAKES_VALUE;
			config.drift_cap_ms = atoi(value);
		} else if (strcmp(arg, "--reconnect") == 0) {
			TAKES_VALUE;
			config.reconnect_s = atoi(value);
		} else if (strcmp(arg, "--stall") == 0) {
			TAKES_VALUE;
			config.stall_s = atoi(value);
		} else if (strcmp(arg, "--hold") == 0) {
			TAKES_VALUE;
			hold_ms = atoi(value);
		} else if (strcmp(arg, "--decoder") == 0) {
			TAKES_VALUE;
			config.decoder = strcmp(value, "gpu") == 0    ? CTD_DECODER_HARDWARE
					 : strcmp(value, "auto") == 0 ? CTD_DECODER_AUTO
								      : CTD_DECODER_SOFTWARE;
		} else if (strcmp(arg, "--live-within") == 0) {
			TAKES_VALUE;
			probe.live_within_s = atof(value);
		} else if (strcmp(arg, "--max-gap") == 0) {
			TAKES_VALUE;
			probe.max_gap_s = atof(value);
		} else if (strcmp(arg, "--max-lateness") == 0) {
			TAKES_VALUE;
			probe.max_lateness_ms = atol(value);
		} else if (strcmp(arg, "--audio-only") == 0) {
			config.audio_only = true;
		} else if (strcmp(arg, "--video-only") == 0) {
			config.no_audio = true;
		} else if (strcmp(arg, "--channels") == 0) {
			TAKES_VALUE;
			static const char *const names[] = {"stereo", "left", "right", "mono", "swap", "invert-right"};
			int found = -1;
			for (int n = 0; n < 6; n++)
				if (strcmp(value, names[n]) == 0)
					found = n;
			if (found < 0) {
				usage();
				return 1;
			}
			channels = (enum ctd_channels)found;
		} else if (strcmp(arg, "--no-fill-gaps") == 0) {
			fill_gaps = false;
		} else if (strcmp(arg, "--lock-audio") == 0) {
			lock_audio = true;
		} else if (strcmp(arg, "--rate-override") == 0) {
			TAKES_VALUE;
			rate_override = atoi(value);
		} else if (strcmp(arg, "--min-gaps-filled") == 0) {
			TAKES_VALUE;
			probe.min_gaps_filled = atol(value);
		} else if (strcmp(arg, "--video-delay") == 0) {
			TAKES_VALUE;
			video_delay = atoi(value);
		} else if (strcmp(arg, "--audio-delay") == 0) {
			TAKES_VALUE;
			audio_delay = atoi(value);
		} else if (strcmp(arg, "--delay-cap") == 0) {
			TAKES_VALUE;
			delay_cap = atoi(value);
		} else if (strcmp(arg, "--change-delay-at") == 0) {
			TAKES_VALUE;
			change_delay_at = atof(value);
		} else if (strcmp(arg, "--video-delay-to") == 0) {
			TAKES_VALUE;
			video_delay_to = atoi(value);
		} else if (strcmp(arg, "--audio-delay-to") == 0) {
			TAKES_VALUE;
			audio_delay_to = atoi(value);
		} else if (strcmp(arg, "--timecode") == 0) {
			TAKES_VALUE;
			timecode = atoi(value);
		} else if (strcmp(arg, "--expect-tc-fps") == 0) {
			TAKES_VALUE;
			probe.expect_tc_fps = atof(value);
		} else if (strcmp(arg, "--expect-drop") == 0) {
			probe.expect_drop = true;
		} else if (strcmp(arg, "--max-tc-errors") == 0) {
			TAKES_VALUE;
			probe.max_tc_errors = atof(value);
		} else if (strcmp(arg, "--sync-check-at") == 0) {
			TAKES_VALUE;
			probe.sync_check_at = atof(value);
		} else if (strcmp(arg, "--expect-av-offset") == 0) {
			TAKES_VALUE;
			probe.expect_av_offset_ms = atof(value);
		} else if (strcmp(arg, "--av-tolerance") == 0) {
			TAKES_VALUE;
			probe.av_tolerance_ms = atof(value);
		} else if (strcmp(arg, "--expect-detected") == 0) {
			TAKES_VALUE;
			probe.expect_detected = atoi(value);
		} else if (strcmp(arg, "--no-gaps-filled") == 0) {
			probe.no_gaps_filled = true;
		} else if (strcmp(arg, "--reach-delays") == 0) {
			probe.reach_delays = true;
		} else if (strcmp(arg, "--no-soft-correction") == 0) {
			soft = false;
		} else if (strcmp(arg, "--no-resyncs") == 0) {
			probe.no_resyncs = true;
		} else if (strcmp(arg, "--monotonic") == 0) {
			probe.monotonic = true;
		} else if (strcmp(arg, "--json") == 0) {
			probe.json = true;
		} else {
			usage();
			return 1;
		}
#undef TAKES_VALUE
	}
	if (!*config.path) {
		usage();
		return 1;
	}

	const struct ctd_engine_callbacks callbacks = {
		.now_ns = probe_now,
		.video = on_video,
		.audio = on_audio,
		.state = on_state,
		.input = on_input,
		.log = on_log,
	};
	struct ctd_engine *engine = ctd_engine_create(&config, &callbacks);
	if (!engine) {
		fprintf(stderr, "ctd-live-probe: couldn't create the engine\n");
		return 1;
	}
	ctd_engine_set_loss(engine, CTD_LOSS_CARD, hold_ms);
	ctd_engine_set_soft_correction(engine, soft);
	ctd_engine_set_audio_repair(engine, channels, fill_gaps, lock_audio, rate_override);
	ctd_engine_set_delay(engine, video_delay, audio_delay, delay_cap);
	ctd_engine_set_timecode(engine, timecode);
	if (video_delay_to < 0)
		video_delay_to = video_delay;
	if (audio_delay_to < 0)
		audio_delay_to = audio_delay;
	bool delay_changed = false;
	bool check_started = false;

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	probe.start_ns = probe_now();
	/* t = 0 on the wall clock, so a harness can line its own actions (publisher stopped at …) up with "t" */
	if (probe.json)
		printf("{\"t\":0,\"type\":\"start\",\"epochMs\":%lld}\n", (long long)(av_gettime() / 1000));
	fflush(stdout);

#ifdef _WIN32
	HANDLE thread = (HANDLE)_beginthreadex(NULL, 0, engine_thread, engine, 0, NULL);
	if (!thread) {
#else
	pthread_t thread;
	if (pthread_create(&thread, NULL, engine_thread, engine) != 0) {
#endif
		fprintf(stderr, "ctd-live-probe: couldn't start the engine thread\n");
		ctd_engine_destroy(engine);
		return 1;
	}

	/* one stats line a second until --seconds or Ctrl+C */
	long last_samples = 0;
	uint64_t last_ns = probe_now();
	while (!eng_load_bool(&probe.stop) && (seconds <= 0 || seconds_since_start() < seconds)) {
		av_usleep(1000000);
		struct ctd_engine_stats st;
		ctd_engine_get_stats(engine, &st);
		const uint64_t now = probe_now();
		const long samples = eng_load_long(&probe.audio_samples);
		const double audio_rate = (double)(samples - last_samples) * 1e9 / (double)(now - last_ns);
		last_samples = samples;
		last_ns = now;
		if (st.state == CTD_FEED_LIVE && labs(st.lateness_ms) > labs(probe.worst_lateness_ms))
			probe.worst_lateness_ms = st.lateness_ms;
		probe.last_timecode = st.timecode;
		if (st.timecode.detected_channel)
			probe.detected_seen = st.timecode.detected_channel;
		if (st.delay_memory_mb > probe.worst_delay_mb)
			probe.worst_delay_mb = st.delay_memory_mb;
		/* T21: the sync check */
		if (probe.sync_check_at > 0 && !check_started && seconds_since_start() >= probe.sync_check_at) {
			ctd_engine_start_sync_check(engine);
			check_started = true;
		}
		/* T20: the delays changed while live */
		if (change_delay_at > 0 && !delay_changed && seconds_since_start() >= change_delay_at) {
			ctd_engine_set_delay(engine, video_delay_to, audio_delay_to, delay_cap);
			delay_changed = true;
		}

		const double t = seconds_since_start();
		if (probe.json) {
			printf("{\"t\":%.3f,\"type\":\"stats\",\"state\":\"%s\",\"latenessMs\":%ld,\"targetMs\":", t,
			       ctd_feed_state_name(st.state), st.lateness_ms);
			if (st.target_set)
				printf("%ld", st.target_ms);
			else
				printf("null");
			printf(",\"correctionPpm\":%ld,\"fps\":%.2f,\"audioRate\":%.0f,\"videoFrames\":%ld,"
			       "\"audioFrames\":%ld,\"resyncs\":%ld,\"reconnects\":%ld,\"discontinuities\":%ld,"
			       "\"videoBackwards\":%ld,\"gapsFilled\":%ld,\"gapMsFilled\":%ld,\"audioLockPpm\":%ld,"
			       "\"peakDb\":[%.1f,%.1f],\"videoDelayMs\":%ld,\"audioDelayMs\":%ld,\"delayMb\":%.1f,",
			       st.correction_ppm, st.fps_x100 / 100.0, audio_rate, st.video_frames, st.audio_frames,
			       st.resyncs, st.reconnects, st.discontinuities, eng_load_long(&probe.video_backwards),
			       st.gaps_filled, st.gap_ms_filled, st.audio_lock_ppm, st.peak_db[0], st.peak_db[1],
			       st.video_delay_ms, st.audio_delay_ms, st.delay_memory_mb);
			const struct ctd_timecode *tc = &st.timecode;
			if (tc->locked)
				printf("\"timecode\":\"%02d:%02d:%02d%c%02d\"", tc->hours, tc->minutes, tc->seconds,
				       tc->drop_frame ? ';' : ':', tc->frames);
			else
				printf("\"timecode\":null");
			printf(",\"tcFps\":%.3f,\"tcErrors\":%.3f,\"tcLevel\":%.1f,\"tcDetected\":%d}\n", tc->fps,
			       tc->error_percent, tc->level_db, tc->detected_channel);
		} else {
			printf("%8.3f  %-12s lateness %4ld ms", t, ctd_feed_state_name(st.state), st.lateness_ms);
			if (st.target_set)
				printf(" (target %ld, %+ld ppm)", st.target_ms, st.correction_ppm);
			printf(" · %.2f fps · audio %.0f Hz · resyncs %ld · reconnects %ld · discontinuities %ld\n",
			       st.fps_x100 / 100.0, audio_rate, st.resyncs, st.reconnects, st.discontinuities);
			if (st.video_delay_ms || st.audio_delay_ms || st.delay_memory_mb > 0)
				printf("          delay video %ld ms · audio %ld ms · holding %.0f MB\n",
				       st.video_delay_ms, st.audio_delay_ms, st.delay_memory_mb);
			if (st.timecode.locked)
				printf("          timecode %02d:%02d:%02d%c%02d at %.3f fps · level %.1f dB · errors %.2f%%\n",
				       st.timecode.hours, st.timecode.minutes, st.timecode.seconds,
				       st.timecode.drop_frame ? ';' : ':', st.timecode.frames, st.timecode.fps,
				       st.timecode.level_db, st.timecode.error_percent);
			else if (st.timecode.detected_channel)
				printf("          LTC found on channel %d\n", st.timecode.detected_channel);
			if (st.gaps_filled || st.audio_lock_ppm)
				printf("          audio gaps filled %ld (%ld ms) · audio lock %+ld ppm\n",
				       st.gaps_filled, st.gap_ms_filled, st.audio_lock_ppm);
		}
		fflush(stdout);
	}

	ctd_engine_stop(engine);
#ifdef _WIN32
	WaitForSingleObject(thread, INFINITE);
	CloseHandle(thread);
#else
	pthread_join(thread, NULL);
#endif
	struct ctd_engine_stats st;
	ctd_engine_get_stats(engine, &st);
	ctd_engine_destroy(engine);
	/* a gap still open at the end counts up to now */
	if (probe.left_live_s >= 0 && seconds_since_start() - probe.left_live_s > probe.worst_gap_s)
		probe.worst_gap_s = seconds_since_start() - probe.left_live_s;

	/* the bounds */
	int broken = 0;
	char why[512] = "";
	size_t used = 0;
#define BROKEN(...)                                                                          \
	do {                                                                                 \
		broken = 1;                                                                  \
		used += (size_t)snprintf(why + used, used < sizeof(why) ? sizeof(why) - used : 0, \
					 __VA_ARGS__);                                       \
	} while (0)
	if (probe.live_within_s > 0 && (probe.first_live_s < 0 || probe.first_live_s > probe.live_within_s))
		BROKEN("not LIVE within %.1f s; ", probe.live_within_s);
	if (probe.max_gap_s > 0 && probe.worst_gap_s > probe.max_gap_s)
		BROKEN("out of LIVE for %.1f s; ", probe.worst_gap_s);
	if (probe.max_lateness_ms > 0 && labs(probe.worst_lateness_ms) > probe.max_lateness_ms)
		BROKEN("lateness reached %ld ms; ", probe.worst_lateness_ms);
	if (probe.no_resyncs && st.resyncs > 0)
		BROKEN("%ld resyncs; ", st.resyncs);
	if (probe.min_gaps_filled > 0 && st.gaps_filled < probe.min_gaps_filled)
		BROKEN("only %ld audio gaps filled; ", st.gaps_filled);
	const struct ctd_timecode *tc = &probe.last_timecode;
	if (probe.expect_tc_fps > 0) {
		if (!tc->locked)
			BROKEN("timecode not locked at the end; ");
		else if (fabs(tc->fps - probe.expect_tc_fps) > 0.01 || tc->drop_frame != probe.expect_drop)
			BROKEN("timecode at %.3f fps%s, not %.3f%s; ", tc->fps, tc->drop_frame ? " drop" : "",
			       probe.expect_tc_fps, probe.expect_drop ? " drop" : "");
	}
	if (probe.max_tc_errors >= 0 && tc->error_percent > probe.max_tc_errors)
		BROKEN("%.2f%% of LTC words lost; ", tc->error_percent);
	if (probe.expect_detected && probe.detected_seen != probe.expect_detected)
		BROKEN("LTC not found on channel %d (found: %d); ", probe.expect_detected, probe.detected_seen);
	if (probe.sync_check_at > 0) {
		if (st.sync_check.state != CTD_SYNC_CHECK_DONE)
			BROKEN("the sync check didn't finish (state %d, %d pairs); ", (int)st.sync_check.state,
			       st.sync_check.pairs);
		else if (fabs(st.sync_check.offset_ms - probe.expect_av_offset_ms) > probe.av_tolerance_ms)
			BROKEN("the sync check measured %.0f ms, not %.0f; ", st.sync_check.offset_ms,
			       probe.expect_av_offset_ms);
	}
	if (probe.no_gaps_filled && st.gaps_filled > 0)
		BROKEN("%ld audio gaps filled (%ld ms); ", st.gaps_filled, st.gap_ms_filled);
	if (probe.worst_delay_mb > delay_cap)
		BROKEN("the delay queue held %.0f MB, over its %d MB cap; ", probe.worst_delay_mb, delay_cap);
	if (probe.reach_delays) {
		/* the video delay moves in whole frames (or stops at what the cap holds), the audio exactly */
		const long video_to = st.video_delay_limit_ms && video_delay_to > st.video_delay_limit_ms
					      ? st.video_delay_limit_ms
					      : video_delay_to;
		if (labs(st.video_delay_ms - video_to) > 25 || labs(st.audio_delay_ms - audio_delay_to) > 1)
			BROKEN("delays at video %ld ms, audio %ld ms, not %ld and %d; ", st.video_delay_ms,
			       st.audio_delay_ms, video_to, audio_delay_to);
	}
	if (probe.monotonic && eng_load_long(&probe.video_backwards) > 0)
		BROKEN("%ld video timestamps went backwards; ", eng_load_long(&probe.video_backwards));
#undef BROKEN

	if (probe.json) {
		printf("{\"t\":%.3f,\"type\":\"result\",\"pass\":%s,\"firstLiveS\":", seconds_since_start(),
		       broken ? "false" : "true");
		if (probe.first_live_s >= 0)
			printf("%.3f", probe.first_live_s);
		else
			printf("null");
		printf(",\"worstLatenessMs\":%ld,\"gaps\":%ld,\"worstGapS\":%.3f,\"resyncs\":%ld,\"reconnects\":%ld,"
		       "\"discontinuities\":%ld,\"videoBackwards\":%ld,\"gapsFilled\":%ld,\"gapMsFilled\":%ld,"
		       "\"videoDelayMs\":%ld,\"audioDelayMs\":%ld,\"worstDelayMb\":%.1f,\"tcLocked\":%s,\"tcFps\":%.3f,"
		       "\"tcDrop\":%s,\"tcErrors\":%.3f,\"tcDetected\":%d,\"syncCheckState\":%d,\"syncCheckOffsetMs\":%.1f,"
		       "\"syncCheckPairs\":%d,\"why\":",
		       probe.worst_lateness_ms, probe.gaps, probe.worst_gap_s, st.resyncs, st.reconnects,
		       st.discontinuities, eng_load_long(&probe.video_backwards), st.gaps_filled, st.gap_ms_filled,
		       st.video_delay_ms, st.audio_delay_ms, probe.worst_delay_mb, tc->locked ? "true" : "false",
		       tc->fps, tc->drop_frame ? "true" : "false", tc->error_percent, probe.detected_seen,
		       (int)st.sync_check.state, st.sync_check.offset_ms, st.sync_check.pairs);
		print_json_string(why);
		printf("}\n");
	} else {
		printf("first LIVE %.2f s · worst lateness %ld ms · %ld gaps, worst %.2f s\n", probe.first_live_s,
		       probe.worst_lateness_ms, probe.gaps, probe.worst_gap_s);
		printf("%s%s\n", broken ? "FAIL: " : "PASS", why);
	}
	fflush(stdout);
	fflush(stderr);
	/* Everything is done and written. End here rather than in the C runtime's exit: on Windows, unloading the FFmpeg
	 * and libsrt DLLs sometimes waits forever on a thread of libsrt's own, which would hang the harness. */
#ifdef _WIN32
	TerminateProcess(GetCurrentProcess(), broken ? 2 : 0);
#endif
	return broken ? 2 : 0;
}
