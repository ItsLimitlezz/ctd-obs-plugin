/*
 * Audio normalization (SPEC R7 to R9). Whatever the decoder produces (8 to 96 kHz, mono to 7.1, any sample format)
 * goes to OBS as 48 kHz stereo float planar. Mono becomes dual mono at full level. A change of rate, layout or format
 * mid-stream rebuilds the resampler on the spot, without a reconnect; the caller logs the change.
 */

#pragma once

#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>

#include <stdbool.h>
#include <stdint.h>

#define AUDIO_NORM_RATE 48000

struct audio_norm {
	struct SwrContext *swr;
	bool configured;
	int compensation;         /* samples a second to add (positive) or drop, for soft clock correction (R5) */
	int compensation_applied; /* what the resampler is set to */
	int samples_since_armed;
	int in_rate;
	int in_format;
	AVChannelLayout in_layout;
	float *planes[2];              /* converted samples */
	int capacity;                  /* samples per plane */
	char description[96];          /* the input, e.g. "16000 Hz mono s16" */
	char previous_description[96]; /* the input before the last change */
};

/*
 * Converts one decoded frame. Returns the number of samples now in out[0] (left) and out[1] (right), valid until
 * the next call or until the frame is released; 0 when the resampler is still filling; negative on an error.
 * *changed says the input format differs from the previous frame's (see the descriptions).
 */
int audio_norm_convert(struct audio_norm *norm, const AVFrame *frame, const uint8_t *out[2], bool *changed);

/* Soft clock correction (SPEC R5): the clock's offset is moving ns_per_s nanoseconds a second (positive: playback is
 * being slowed down), so the audio is stretched or squeezed to match. Applied from the next frame. */
void audio_norm_set_compensation(struct audio_norm *norm, double ns_per_s);

void audio_norm_free(struct audio_norm *norm);
