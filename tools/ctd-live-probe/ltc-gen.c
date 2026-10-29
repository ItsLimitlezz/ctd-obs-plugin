/*
 * ltc-gen (SPEC 10.4, T17): the harness's timecode source. Writes 48 kHz stereo s16le to stdout: LTC on one channel and
 * the harness's 1 kHz beep (50 ms at the top of every second) on the other, for ffmpeg to encode and publish:
 *
 *   ltc-gen --fps 29.97 --drop | ffmpeg -re -f s16le -ar 48000 -ac 2 -i - ...
 *
 *   --fps F          23.976, 24, 25, 29.97 or 30 (default 30)
 *   --drop           drop-frame numbering (29.97 only)
 *   --start H:M:S:F  the first timecode (default 10:00:00:00)
 *   --ltc left|right the channel carrying LTC (default right)
 *   --seconds N      stop after N seconds (default: never)
 *   --time-of-day    start at the local time of day (the sync group tests: every generator agrees)
 *   --offset-ms N    the timecode runs N ms behind (a feed that arrives N ms late, T18)
 *   --mute-from S --mute-to S   no LTC between those seconds (a member losing its timecode, T19)
 *   --self-test      encode and decode every rate in memory and check the decoder; exit code 0 if it passes
 */

#include "ltc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#define RATE 48000
#define CHUNK 960 /* 20 ms */

static int self_test(void)
{
	static const struct {
		double fps;
		bool drop;
	} rates[] = {{23.976, false}, {24, false}, {25, false}, {29.97, true}, {29.97, false}, {30, false}};
	int failed = 0;
	for (size_t r = 0; r < sizeof(rates) / sizeof(rates[0]); r++) {
		struct ltc_encoder enc;
		const int base = rates[r].fps > 29 ? 30 : rates[r].fps > 24.5 ? 25 : 24;
		/* start just before a minute boundary, so drop frame skips numbers on the way */
		const int64_t start = ltc_frame_index(10, 0, 58, 0, base, rates[r].drop);
		ltc_encoder_init(&enc, rates[r].fps, rates[r].drop, start);
		struct ltc_decoder dec = {0};
		struct ltc_rate rate = {0};
		float samples[CHUNK];
		long words = 0, out_of_order = 0;
		int64_t previous = -1;
		const long chunks = 20L * RATE / CHUNK;
		for (long c = 0; c < chunks; c++) {
			ltc_encode(&enc, samples, CHUNK, RATE, 0.5f);
			struct ltc_word out[4];
			const int n = ltc_decode(&dec, samples, CHUNK, RATE, out, 4);
			for (int i = 0; i < n; i++) {
				const int64_t ns = (int64_t)((c * CHUNK + out[i].end_offset) * 1e9 / RATE);
				ltc_rate_add(&rate, &out[i], ns, ltc_bit_rate_fps(&dec, RATE));
				const int64_t index = ltc_frame_index(out[i].hours, out[i].minutes, out[i].seconds,
								      out[i].frames, base, out[i].drop_frame);
				if (previous >= 0 && index != previous + 1)
					out_of_order++;
				previous = index;
				words++;
			}
		}
		const double fps = ltc_rate_fps(&rate);
		const bool ok = out_of_order == 0 && words > 20 * rates[r].fps - 3 && fabs(fps - rates[r].fps) < 0.01 &&
				rate.drop == rates[r].drop;
		printf("%-6g %-8s words %ld, out of order %ld, detected %.3f%s, errors %.2f%%: %s\n", rates[r].fps,
		       rates[r].drop ? "drop" : "non-drop", words, out_of_order, fps, rate.drop ? " drop" : "",
		       ltc_rate_error_percent(&rate), ok ? "ok" : "FAIL");
		failed += !ok;
	}
	return failed ? 2 : 0;
}

static bool parse_timecode(const char *text, int *h, int *m, int *s, int *f)
{
	int *const parts[4] = {h, m, s, f};
	for (int i = 0; i < 4; i++) {
		char *end;
		*parts[i] = (int)strtol(text, &end, 10);
		if (end == text || (i < 3 && *end != ':' && *end != ';') || (i == 3 && *end))
			return false;
		text = end + (i < 3);
	}
	return true;
}

int main(int argc, char **argv)
{
	double fps = 30;
	bool drop = false;
	int h = 10, m = 0, s = 0, f = 0;
	int ltc_channel = 1;
	double seconds = 0;
	bool time_of_day = false;
	double offset_ms = 0, mute_from = -1, mute_to = -1;
	for (int i = 1; i < argc; i++) {
		const char *value = i + 1 < argc ? argv[i + 1] : NULL;
		if (strcmp(argv[i], "--self-test") == 0)
			return self_test();
		if (strcmp(argv[i], "--drop") == 0) {
			drop = true;
		} else if (strcmp(argv[i], "--fps") == 0 && value) {
			fps = atof(value);
			i++;
		} else if (strcmp(argv[i], "--start") == 0 && value && parse_timecode(value, &h, &m, &s, &f)) {
			i++;
		} else if (strcmp(argv[i], "--ltc") == 0 && value) {
			ltc_channel = strcmp(value, "left") == 0 ? 0 : 1;
			i++;
		} else if (strcmp(argv[i], "--seconds") == 0 && value) {
			seconds = atof(value);
			i++;
		} else if (strcmp(argv[i], "--time-of-day") == 0) {
			time_of_day = true;
		} else if (strcmp(argv[i], "--offset-ms") == 0 && value) {
			offset_ms = atof(value);
			i++;
		} else if (strcmp(argv[i], "--mute-from") == 0 && value) {
			mute_from = atof(value);
			i++;
		} else if (strcmp(argv[i], "--mute-to") == 0 && value) {
			mute_to = atof(value);
			i++;
		} else {
			fprintf(stderr, "usage: ltc-gen [--fps F] [--drop] [--start H:M:S:F] [--ltc left|right] "
					"[--seconds N] [--time-of-day] [--offset-ms N] [--mute-from S --mute-to S] | "
					"--self-test\n");
			return 1;
		}
	}
#ifdef _WIN32
	_setmode(_fileno(stdout), _O_BINARY);
#endif
	const int base = fps > 29 ? 30 : fps > 24.5 ? 25 : 24;
	struct ltc_encoder enc;
	ltc_encoder_init(&enc, fps, drop, ltc_frame_index(h, m, s, f, base, drop && base == 30));
	if (time_of_day) {
		/* the frame (and the bit within it) on air at this moment, less the offset */
		struct timespec ts;
		timespec_get(&ts, TIME_UTC);
		struct tm local;
#ifdef _WIN32
		localtime_s(&local, &ts.tv_sec);
#else
		localtime_r(&ts.tv_sec, &local);
#endif
		double tod = local.tm_hour * 3600.0 + local.tm_min * 60.0 + local.tm_sec + ts.tv_nsec / 1e9 -
			     offset_ms / 1000;
		if (tod < 0)
			tod += 86400.0;
		const double frames = tod * fps;
		enc.index = (int64_t)frames;
		enc.phase = (frames - (double)enc.index) * 80.0;
	}
	float ltc[CHUNK];
	short out[CHUNK * 2];
	const double pi = 3.14159265358979323846;
	for (long long sample = 0; seconds <= 0 || sample < (long long)(seconds * RATE); sample += CHUNK) {
		ltc_encode(&enc, ltc, CHUNK, RATE, 0.5f);
		for (int i = 0; i < CHUNK; i++) {
			const long long n = sample + i;
			const double t = (double)n / RATE;
			const double beep = n % RATE < RATE / 20 ? 0.5 * sin(2 * pi * 1000 * t) : 0.0;
			const bool muted = t >= mute_from && t < mute_to;
			out[2 * i + ltc_channel] = muted ? 0 : (short)(ltc[i] * 32767);
			out[2 * i + 1 - ltc_channel] = (short)(beep * 32767);
		}
		if (fwrite(out, sizeof(out), 1, stdout) != 1)
			return 0; /* the reader went away */
	}
	return 0;
}
