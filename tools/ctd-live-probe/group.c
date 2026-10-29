/*
 * ctd-live-probe's group mode (SPEC 8.3, T18 and T19): several feeds in one sync group, with the same engine and the
 * same group controller as the OBS plugin.
 *
 *   ctd-live-probe --group cam1,cam2,cam3 [--host H] [--port P] [--timecode N] [--seconds N] [--json] [bounds]
 *
 * Every round (10 a second) it reads each member's timecode, runs the controller and applies the group delays; every
 * second it prints each member's state, timecode, measured offset and delays, and the spread: how far apart the
 * members' timecodes are on screen at the same instant.
 *
 * Bounds (any broken one makes the exit code 2):
 *   --lock-within S          every member locked within S seconds                         (T18)
 *   --max-spread-frames F    once all are locked and settled, on screen within F frames    (T18, T19)
 *   --no-jumps-after-lock    a member's delay only ever jumps at its first lock            (T18, T19)
 *   --expect-freewheel       some member freewheeled (lost its timecode) at some point     (T19)
 *   --all-locked-at-end      every member locked at the end                                 (T19)
 * Group settings: --max-delay MS (1000), --fixed-latency MS (fixed latency mode instead of align to slowest).
 */

#include "engine.h"
#include "sync-group.h"

#include <libavutil/time.h>

#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#else
#include <pthread.h>
#endif

static volatile int group_stop;

static uint64_t group_now(void)
{
	return (uint64_t)av_gettime_relative() * 1000ull;
}

/* the local time of day, ns since midnight (fixed latency mode) */
static int64_t time_of_day_ns(void)
{
	struct timespec ts;
	timespec_get(&ts, TIME_UTC);
	struct tm local;
#ifdef _WIN32
	localtime_s(&local, &ts.tv_sec);
#else
	localtime_r(&ts.tv_sec, &local);
#endif
	return ((int64_t)local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec) * 1000000000ll + ts.tv_nsec;
}

static bool member_video(void *opaque, const AVFrame *picture, uint64_t timestamp_ns)
{
	(void)opaque;
	(void)picture;
	(void)timestamp_ns;
	return true;
}

static void member_audio(void *opaque, const float *const planes[2], int frames, uint64_t timestamp_ns)
{
	(void)opaque;
	(void)planes;
	(void)frames;
	(void)timestamp_ns;
}

static void member_log(void *opaque, enum ctd_log_level level, const char *message)
{
	if (level <= CTD_LOG_INFO)
		fprintf(stderr, "[%s] %s\n", (const char *)opaque, message);
}

#ifdef _WIN32
static unsigned __stdcall member_thread(void *engine)
{
	ctd_engine_run(engine);
	return 0;
}
#else
static void *member_thread(void *engine)
{
	ctd_engine_run(engine);
	return NULL;
}
#endif

static void on_group_signal(int sig)
{
	(void)sig;
	group_stop = 1;
}

/* the timecode at its pin, in seconds since midnight */
static double timecode_seconds(const struct ctd_timecode *tc)
{
	const int base = tc->fps > 29 ? 30 : tc->fps > 24.5 ? 25 : 24;
	return tc->hours * 3600.0 + tc->minutes * 60.0 + tc->seconds + (double)tc->frames / base;
}

int run_group(int argc, char **argv)
{
	char *paths[SYNC_MAX_MEMBERS];
	int count = 0;
	const char *host = "127.0.0.1";
	int port = 8890, latency = 120, timecode = CTD_TIMECODE_LTC_RIGHT;
	double seconds = 0, lock_within = 0, max_spread_frames = 0;
	bool json = false, no_jumps = false, expect_freewheel = false, all_locked_at_end = false;
	struct sync_group group;
	sync_group_init(&group);
	for (int i = 1; i < argc; i++) {
		const char *arg = argv[i];
		const char *value = i + 1 < argc ? argv[i + 1] : NULL;
		if (strcmp(arg, "--group") == 0 && value) {
			/* comma separated */
			for (const char *p = value; *p && count < SYNC_MAX_MEMBERS;) {
				const char *end = strchr(p, ',');
				const size_t length = end ? (size_t)(end - p) : strlen(p);
				if (length) {
					paths[count] = malloc(length + 1);
					memcpy(paths[count], p, length);
					paths[count++][length] = '\0';
				}
				p += length + (end ? 1 : 0);
			}
			i++;
		} else if (strcmp(arg, "--path") == 0 && value) {
			i++; /* the harness passes one; the group names its own */
		} else if (strcmp(arg, "--host") == 0 && value) {
			host = value;
			i++;
		} else if (strcmp(arg, "--port") == 0 && value) {
			port = atoi(value);
			i++;
		} else if (strcmp(arg, "--latency") == 0 && value) {
			latency = atoi(value);
			i++;
		} else if (strcmp(arg, "--timecode") == 0 && value) {
			timecode = atoi(value);
			i++;
		} else if (strcmp(arg, "--seconds") == 0 && value) {
			seconds = atof(value);
			i++;
		} else if (strcmp(arg, "--max-delay") == 0 && value) {
			group.max_delay_ms = atoi(value);
			i++;
		} else if (strcmp(arg, "--fixed-latency") == 0 && value) {
			group.mode = SYNC_FIXED_LATENCY;
			group.fixed_latency_ms = atoi(value);
			i++;
		} else if (strcmp(arg, "--lock-within") == 0 && value) {
			lock_within = atof(value);
			i++;
		} else if (strcmp(arg, "--max-spread-frames") == 0 && value) {
			max_spread_frames = atof(value);
			i++;
		} else if (strcmp(arg, "--no-jumps-after-lock") == 0) {
			no_jumps = true;
		} else if (strcmp(arg, "--expect-freewheel") == 0) {
			expect_freewheel = true;
		} else if (strcmp(arg, "--all-locked-at-end") == 0) {
			all_locked_at_end = true;
		} else if (strcmp(arg, "--json") == 0) {
			json = true;
		} else {
			fprintf(stderr, "ctd-live-probe --group: unknown option %s\n", arg);
			return 1;
		}
	}
	if (count < 2) {
		fprintf(stderr, "ctd-live-probe --group needs at least two paths, comma separated\n");
		return 1;
	}

	struct ctd_engine *engines[SYNC_MAX_MEMBERS];
#ifdef _WIN32
	HANDLE threads[SYNC_MAX_MEMBERS];
#else
	pthread_t threads[SYNC_MAX_MEMBERS];
#endif
	for (int i = 0; i < count; i++) {
		const struct ctd_engine_config config = {
			.host = host,
			.port = port,
			.path = paths[i],
			.latency_ms = latency,
			.drift_cap_ms = 700,
			.reconnect_s = 1,
			.stall_s = 3,
			.decoder = CTD_DECODER_SOFTWARE,
		};
		const struct ctd_engine_callbacks callbacks = {
			.opaque = paths[i],
			.now_ns = group_now,
			.video = member_video,
			.audio = member_audio,
			.log = member_log,
		};
		engines[i] = ctd_engine_create(&config, &callbacks);
		if (!engines[i])
			return 1;
		ctd_engine_set_timecode(engines[i], timecode);
		sync_member_join(&group, i, 0);
#ifdef _WIN32
		threads[i] = (HANDLE)_beginthreadex(NULL, 0, member_thread, engines[i], 0, NULL);
#else
		pthread_create(&threads[i], NULL, member_thread, engines[i]);
#endif
	}
	signal(SIGINT, on_group_signal);
	signal(SIGTERM, on_group_signal);

	const uint64_t start = group_now();
	double all_locked_at = -1, settled_at = -1, worst_spread_ms = 0, worst_spread_frames = 0;
	bool freewheeled = false;
	long jumps_after_lock = 0;
	long seen_jumps[SYNC_MAX_MEMBERS] = {0};
	struct sync_input in[SYNC_MAX_MEMBERS];
	struct sync_output out[SYNC_MAX_MEMBERS];
	struct ctd_engine_stats st[SYNC_MAX_MEMBERS];
	double next_print = 1.0;
	if (json)
		printf("{\"t\":0,\"type\":\"start\",\"epochMs\":%lld,\"members\":%d}\n",
		       (long long)(av_gettime() / 1000), count);
	while (!group_stop) {
		av_usleep(100000);
		const uint64_t now = group_now();
		const double t = (double)(now - start) / 1e9;
		if (seconds > 0 && t >= seconds)
			break;
		memset(in, 0, sizeof(in));
		for (int i = 0; i < count; i++) {
			ctd_engine_get_stats(engines[i], &st[i]);
			const struct ctd_timecode *tc = &st[i].timecode;
			in[i].timecode = tc->locked && tc->clock_ns;
			in[i].pinned = tc->clock_ns != 0;
			in[i].tc_seconds = timecode_seconds(tc);
			in[i].pin_ns = tc->clock_ns;
			in[i].fps = tc->fps;
			in[i].settled = st[i].delay_settled;
		}
		sync_group_update(&group, in, now, time_of_day_ns(), out);
		bool all_locked = true, all_settled = true;
		double fps = 0;
		for (int i = 0; i < count; i++) {
			const struct sync_member *m = &group.members[i];
			if (out[i].changed)
				ctd_engine_set_group_delay(engines[i], (int)lround(out[i].delay_ms), out[i].jump);
			if (m->jumps > seen_jumps[i]) {
				/* only a member's first lock may jump */
				if (m->jumps > 1)
					jumps_after_lock += m->jumps - seen_jumps[i];
				seen_jumps[i] = m->jumps;
			}
			freewheeled |= m->state == SYNC_FREEWHEEL;
			all_locked &= m->state == SYNC_LOCKED || m->state == SYNC_ADJUSTING;
			all_settled &= m->state == SYNC_LOCKED && st[i].delay_settled;
			if (in[i].fps > fps)
				fps = in[i].fps;
		}
		if (all_locked && all_locked_at < 0)
			all_locked_at = t;
		/* the spread on screen: each member's timecode arriving now, less its video delay (ms) */
		double spread_ms = -1;
		if (all_settled) {
			if (settled_at < 0)
				settled_at = t;
			double lowest = 0, highest = 0;
			for (int i = 0; i < count; i++) {
				const double shown =
					(in[i].tc_seconds + (double)((int64_t)now - (int64_t)in[i].pin_ns) / 1e9) *
						1000.0 -
					(double)st[i].video_delay_ms;
				if (i == 0 || shown < lowest)
					lowest = shown;
				if (i == 0 || shown > highest)
					highest = shown;
			}
			spread_ms = highest - lowest;
			if (spread_ms > worst_spread_ms) {
				worst_spread_ms = spread_ms;
				worst_spread_frames = fps > 0 ? spread_ms * fps / 1000.0 : 0;
				/* the round it happened in, for working out why */
				if (json) {
					printf("{\"t\":%.3f,\"type\":\"worst-spread\",\"spreadMs\":%.1f,\"members\":[",
					       t, spread_ms);
					for (int i = 0; i < count; i++)
						printf("%s{\"tc\":%.3f,\"pinAgeMs\":%.1f,\"fps\":%.3f,\"videoDelayMs\":%ld,"
						       "\"groupDelayMs\":%ld,\"settled\":%s}",
						       i ? "," : "", in[i].tc_seconds,
						       (double)((int64_t)now - (int64_t)in[i].pin_ns) / 1e6, in[i].fps,
						       st[i].video_delay_ms, st[i].group_delay_ms,
						       st[i].delay_settled ? "true" : "false");
					printf("]}\n");
				}
			}
		}

		if (t >= next_print) {
			next_print += 1.0;
			if (json) {
				printf("{\"t\":%.3f,\"type\":\"group\",\"spreadMs\":", t);
				if (spread_ms >= 0)
					printf("%.1f", spread_ms);
				else
					printf("null");
				printf(",\"members\":[");
			} else {
				printf("%8.3f  spread %s", t, spread_ms >= 0 ? "" : "-");
				if (spread_ms >= 0)
					printf("%.1f ms", spread_ms);
				printf("\n");
			}
			for (int i = 0; i < count; i++) {
				const struct sync_member *m = &group.members[i];
				const struct ctd_timecode *tc = &st[i].timecode;
				char code[16] = "";
				if (tc->locked)
					snprintf(code, sizeof(code), "%02d:%02d:%02d:%02d", tc->hours, tc->minutes,
						 tc->seconds, tc->frames);
				if (json)
					printf("%s{\"path\":\"%s\",\"state\":\"%s\",\"feed\":\"%s\",\"timecode\":\"%s\","
					       "\"offsetMs\":%.1f,\"groupDelayMs\":%.0f,\"videoDelayMs\":%ld,"
					       "\"audioDelayMs\":%ld,\"reference\":%s,\"jumps\":%ld}",
					       i ? "," : "", paths[i], sync_state_name(m->state),
					       ctd_feed_state_name(st[i].state), code, m->offset_ms, m->delay_ms,
					       st[i].video_delay_ms, st[i].audio_delay_ms,
					       m->reference ? "true" : "false", m->jumps);
				else
					printf("          %-10s %-11s %-12s %s offset %7.1f ms · group delay %4.0f ms · video "
					       "%ld · audio %ld%s\n",
					       paths[i], sync_state_name(m->state), ctd_feed_state_name(st[i].state),
					       code, m->offset_ms, m->delay_ms, st[i].video_delay_ms,
					       st[i].audio_delay_ms, m->reference ? " (reference)" : "");
			}
			if (json)
				printf("]}\n");
			fflush(stdout);
		}
	}

	for (int i = 0; i < count; i++)
		ctd_engine_stop(engines[i]);
	for (int i = 0; i < count; i++) {
#ifdef _WIN32
		WaitForSingleObject(threads[i], INFINITE);
		CloseHandle(threads[i]);
#else
		pthread_join(threads[i], NULL);
#endif
	}
	bool locked_at_end = true;
	for (int i = 0; i < count; i++)
		locked_at_end &= group.members[i].state == SYNC_LOCKED || group.members[i].state == SYNC_ADJUSTING;

	char why[512] = "";
	size_t used = 0;
	int broken = 0;
#define GROUP_BROKEN(...)                                                                                     \
	do {                                                                                                  \
		broken = 1;                                                                                   \
		used += (size_t)snprintf(why + used, used < sizeof(why) ? sizeof(why) - used : 0, __VA_ARGS__); \
	} while (0)
	if (lock_within > 0 && (all_locked_at < 0 || all_locked_at > lock_within))
		GROUP_BROKEN("not all locked within %.0f s; ", lock_within);
	if (max_spread_frames > 0 && (settled_at < 0 || worst_spread_frames > max_spread_frames))
		GROUP_BROKEN("spread reached %.1f ms (%.2f frames); ", worst_spread_ms, worst_spread_frames);
	if (no_jumps && jumps_after_lock)
		GROUP_BROKEN("%ld jumps after lock; ", jumps_after_lock);
	if (expect_freewheel && !freewheeled)
		GROUP_BROKEN("no member freewheeled; ");
	if (all_locked_at_end && !locked_at_end)
		GROUP_BROKEN("not all locked at the end; ");
#undef GROUP_BROKEN
	const double t_end = (double)(group_now() - start) / 1e9;
	if (json) {
		printf("{\"t\":%.3f,\"type\":\"result\",\"pass\":%s,\"allLockedS\":", t_end, broken ? "false" : "true");
		if (all_locked_at >= 0)
			printf("%.3f", all_locked_at);
		else
			printf("null");
		printf(",\"worstSpreadMs\":%.1f,\"worstSpreadFrames\":%.2f,\"jumpsAfterLock\":%ld,\"freewheeled\":%s,"
		       "\"why\":\"%s\"}\n",
		       worst_spread_ms, worst_spread_frames, jumps_after_lock, freewheeled ? "true" : "false", why);
	} else {
		printf("all locked at %.1f s · worst spread %.1f ms (%.2f frames) · %ld jumps after lock\n",
		       all_locked_at, worst_spread_ms, worst_spread_frames, jumps_after_lock);
		printf("%s%s\n", broken ? "FAIL: " : "PASS", why);
	}
	fflush(stdout);
	for (int i = 0; i < count; i++)
		ctd_engine_destroy(engines[i]);
#ifdef _WIN32
	TerminateProcess(GetCurrentProcess(), broken ? 2 : 0);
#endif
	return broken ? 2 : 0;
}
