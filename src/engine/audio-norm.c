/*
 * Audio normalization. See audio-norm.h.
 */

#include "audio-norm.h"

#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

#include <stdio.h>
#include <string.h>

static void describe(char *buffer, size_t size, int rate, const AVChannelLayout *layout, int format)
{
	char channels[48];
	if (av_channel_layout_describe(layout, channels, sizeof(channels)) < 0)
		snprintf(channels, sizeof(channels), "%d channels", layout->nb_channels);
	const char *sample_format = av_get_sample_fmt_name((enum AVSampleFormat)format);
	snprintf(buffer, size, "%d Hz %s %s", rate, channels, sample_format ? sample_format : "?");
}

/* Sets the converter up for this input. False if it can't be converted. */
static bool configure(struct audio_norm *norm, const AVFrame *frame, const AVChannelLayout *layout)
{
	swr_free(&norm->swr);
	av_channel_layout_uninit(&norm->in_layout);
	if (av_channel_layout_copy(&norm->in_layout, layout) < 0)
		return false;
	norm->in_rate = frame->sample_rate;
	norm->in_format = frame->format;
	norm->configured = true;

	/* always through the resampler, even when the input already matches: soft clock correction (R5) stretches the
	 * audio there, and switching to and from a pass-through would click */
	const AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
	norm->compensation_applied = 0;
	norm->samples_since_armed = 0;
	if (swr_alloc_set_opts2(&norm->swr, &stereo, AV_SAMPLE_FMT_FLTP, AUDIO_NORM_RATE, layout,
				(enum AVSampleFormat)frame->format, frame->sample_rate, 0, NULL) < 0)
		return false;
	/* R9: mono is the centre channel; at full level on both sides it is dual mono, not left only and not quieter */
	if (layout->nb_channels == 1)
		av_opt_set_double(norm->swr, "center_mix_level", 1.0, 0);
	if (swr_init(norm->swr) < 0) {
		swr_free(&norm->swr);
		return false;
	}
	return true;
}

int audio_norm_convert(struct audio_norm *norm, const AVFrame *frame, const uint8_t *out[2], bool *changed)
{
	*changed = false;
	if (frame->sample_rate <= 0 || frame->ch_layout.nb_channels <= 0 || frame->nb_samples <= 0)
		return AVERROR(EINVAL);

	/* a layout without channel names (some decoders) gets the usual one for its channel count */
	AVChannelLayout layout = {0};
	if (frame->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
		av_channel_layout_default(&layout, frame->ch_layout.nb_channels);
	else if (av_channel_layout_copy(&layout, &frame->ch_layout) < 0)
		return AVERROR(ENOMEM);

	const bool same = norm->configured && norm->in_rate == frame->sample_rate && norm->in_format == frame->format &&
			  av_channel_layout_compare(&norm->in_layout, &layout) == 0;
	if (!same) {
		/* R8: rebuilt on the fly; the caller logs "audio format changed <before> → <after>" */
		*changed = norm->configured;
		memcpy(norm->previous_description, norm->description, sizeof(norm->description));
		describe(norm->description, sizeof(norm->description), frame->sample_rate, &layout, frame->format);
		if (!configure(norm, frame, &layout)) {
			norm->configured = false;
			av_channel_layout_uninit(&layout);
			return AVERROR(EINVAL);
		}
	}
	av_channel_layout_uninit(&layout);

	/* R5: stretch (positive) or squeeze the audio by the clock's correction; armed for a second at a time, so it is
	 * re-armed each second while it lasts */
	if (norm->compensation != norm->compensation_applied ||
	    (norm->compensation && norm->samples_since_armed >= AUDIO_NORM_RATE)) {
		if (swr_set_compensation(norm->swr, norm->compensation, AUDIO_NORM_RATE) >= 0)
			norm->compensation_applied = norm->compensation;
		norm->samples_since_armed = 0;
	}

	const int needed = swr_get_out_samples(norm->swr, frame->nb_samples);
	if (needed < 0)
		return needed;
	if (needed > norm->capacity) {
		norm->capacity = needed;
		for (int c = 0; c < 2; c++) {
			av_free(norm->planes[c]);
			norm->planes[c] = av_malloc((size_t)needed * sizeof(float));
			if (!norm->planes[c])
				norm->capacity = 0;
		}
		if (!norm->capacity)
			return AVERROR(ENOMEM);
	}
	const int converted = swr_convert(norm->swr, (uint8_t **)norm->planes, norm->capacity,
					  (const uint8_t **)frame->extended_data, frame->nb_samples);
	out[0] = (const uint8_t *)norm->planes[0];
	out[1] = (const uint8_t *)norm->planes[1];
	if (converted > 0)
		norm->samples_since_armed += converted;
	return converted;
}

void audio_norm_set_compensation(struct audio_norm *norm, double ns_per_s)
{
	/* the clock moving ns_per_s nanoseconds each second is that many seconds of extra audio each second */
	double samples = ns_per_s * AUDIO_NORM_RATE / 1e9;
	norm->compensation = (int)(samples >= 0 ? samples + 0.5 : samples - 0.5);
}

void audio_norm_free(struct audio_norm *norm)
{
	swr_free(&norm->swr);
	av_channel_layout_uninit(&norm->in_layout);
	for (int c = 0; c < 2; c++) {
		av_free(norm->planes[c]);
		norm->planes[c] = NULL;
	}
	norm->capacity = 0;
	norm->configured = false;
}
