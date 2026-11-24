/* What a stream to CTD Relay has to be. See relay-encoder.h. Pure C, no libobs. */
#include "relay-encoder.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static bool equals_nocase(const char *a, const char *b)
{
	for (; *a && *b; a++, b++) {
		if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
			return false;
	}
	return *a == *b;
}

static bool ends_with(const char *text, const char *suffix)
{
	const size_t n = strlen(text), m = strlen(suffix);
	return n >= m && strcmp(text + n - m, suffix) == 0;
}

relay_platform_t relay_platform_current(void)
{
#if defined(_WIN32)
	return RELAY_PLATFORM_WINDOWS;
#elif defined(__APPLE__)
	return RELAY_PLATFORM_MACOS;
#else
	return RELAY_PLATFORM_LINUX;
#endif
}

relay_codec_t relay_video_codec(const char *codec)
{
	if (!codec || !*codec)
		return RELAY_CODEC_UNKNOWN;
	if (equals_nocase(codec, "h264") || equals_nocase(codec, "avc"))
		return RELAY_CODEC_H264;
	if (equals_nocase(codec, "hevc") || equals_nocase(codec, "h265"))
		return RELAY_CODEC_HEVC;
	if (equals_nocase(codec, "av1"))
		return RELAY_CODEC_AV1;
	return RELAY_CODEC_OTHER;
}

const char *relay_video_codec_label(const char *codec)
{
	switch (relay_video_codec(codec)) {
	case RELAY_CODEC_H264:
		return "H.264";
	case RELAY_CODEC_HEVC:
		return "HEVC";
	case RELAY_CODEC_AV1:
		return "AV1";
	case RELAY_CODEC_OTHER:
		return codec;
	default:
		return "";
	}
}

bool relay_audio_is_aac(const char *codec)
{
	return codec && equals_nocase(codec, "aac");
}

unsigned relay_stream_issues(const char *video_codec, const char *audio_codec)
{
	unsigned issues = 0;
	const relay_codec_t video = relay_video_codec(video_codec);
	if (video != RELAY_CODEC_UNKNOWN && video != RELAY_CODEC_H264)
		issues |= RELAY_ISSUE_VIDEO;
	if (audio_codec && *audio_codec && !relay_audio_is_aac(audio_codec))
		issues |= RELAY_ISSUE_AUDIO;
	return issues;
}

/* One H.264 encoder: its OBS id, and the word OBS's Simple output mode knows it by (NULL: Simple mode can't pick
 * it). The Simple names and what they stand for are OBS's own (get_simple_output_encoder). */
struct h264_encoder {
	const char *id;
	const char *simple;
};

/* Hardware first, in the order to try them. Apple's VideoToolbox H.264 hardware encoder is "ave.avc" on Apple
 * silicon and "h264.gva" on Intel Macs. */
static const struct h264_encoder mac_h264[] = {
	{"com.apple.videotoolbox.videoencoder.ave.avc", "apple_h264"},
	{"com.apple.videotoolbox.videoencoder.h264.gva", NULL},
	{"obs_x264", "x264"},
};

/* NVENC, AMF, Quick Sync; the second id of each is the one older OBS versions have */
static const struct h264_encoder win_h264[] = {
	{"obs_nvenc_h264_tex", "nvenc"}, {"ffmpeg_nvenc", "nvenc"}, {"h264_texture_amf", "amd"},
	{"obs_qsv11_v2", "qsv"},         {"obs_qsv11", NULL},       {"obs_x264", "x264"},
};

static const struct h264_encoder linux_h264[] = {
	{"obs_nvenc_h264_tex", "nvenc"},
	{"ffmpeg_nvenc", "nvenc"},
	{"h264_texture_amf", "amd"},
	{"obs_qsv11_v2", "qsv"},
	{"ffmpeg_vaapi_tex", NULL},
	{"ffmpeg_vaapi", NULL},
	{"obs_x264", "x264"},
};

static const struct h264_encoder *h264_list(relay_platform_t platform, size_t *count)
{
	switch (platform) {
	case RELAY_PLATFORM_MACOS:
		*count = COUNT(mac_h264);
		return mac_h264;
	case RELAY_PLATFORM_WINDOWS:
		*count = COUNT(win_h264);
		return win_h264;
	default:
		*count = COUNT(linux_h264);
		return linux_h264;
	}
}

const char *relay_pick_h264(relay_platform_t platform, relay_available_fn available, void *ctx)
{
	size_t count = 0;
	const struct h264_encoder *list = h264_list(platform, &count);
	for (size_t i = 0; available && i < count; i++) {
		if (available(ctx, list[i].id))
			return list[i].id;
	}
	return NULL;
}

const char *relay_pick_h264_simple(relay_platform_t platform, relay_available_fn available, void *ctx)
{
	size_t count = 0;
	const struct h264_encoder *list = h264_list(platform, &count);
	for (size_t i = 0; available && i < count; i++) {
		if (list[i].simple && available(ctx, list[i].id))
			return list[i].simple;
	}
	return "x264"; /* OBS itself falls back to x264 for a Simple encoder it doesn't have */
}

const char *relay_pick_aac(relay_available_fn available, void *ctx)
{
	static const char *const order[] = {"CoreAudio_AAC", "libfdk_aac", "ffmpeg_aac"};
	for (size_t i = 0; available && i < COUNT(order); i++) {
		if (available(ctx, order[i]))
			return order[i];
	}
	return NULL;
}

relay_codec_t relay_simple_codec(const char *simple_name)
{
	if (!simple_name || !*simple_name)
		return RELAY_CODEC_UNKNOWN;
	if (ends_with(simple_name, "_hevc"))
		return RELAY_CODEC_HEVC;
	if (ends_with(simple_name, "_av1"))
		return RELAY_CODEC_AV1;
	return RELAY_CODEC_H264; /* x264, x264_lowcpu, qsv, amd, nvenc, apple_h264; an unknown word runs as x264 */
}

static bool fix_result(relay_video_fix_t *out, relay_codec_t from, const char *current, const char *pick)
{
	memset(out, 0, sizeof(*out));
	out->from = from;
	out->needed = from != RELAY_CODEC_H264;
	if (!out->needed) {
		out->possible = true;
		snprintf(out->encoder, sizeof(out->encoder), "%s", current ? current : "");
		return true;
	}
	out->possible = pick != NULL;
	snprintf(out->encoder, sizeof(out->encoder), "%s", pick ? pick : (current ? current : ""));
	return out->possible;
}

bool relay_video_fix(const char *encoder_id, const char *codec, relay_platform_t platform, relay_available_fn available,
		     void *ctx, relay_video_fix_t *out)
{
	const relay_codec_t from = relay_video_codec(codec);
	return fix_result(out, from, encoder_id,
			  from == RELAY_CODEC_H264 ? NULL : relay_pick_h264(platform, available, ctx));
}

bool relay_video_fix_simple(const char *simple_name, relay_platform_t platform, relay_available_fn available, void *ctx,
			    relay_video_fix_t *out)
{
	relay_codec_t from = relay_simple_codec(simple_name);
	if (from == RELAY_CODEC_UNKNOWN)
		from = RELAY_CODEC_H264; /* nothing set: OBS uses its default, which is an H.264 encoder */
	return fix_result(out, from, simple_name,
			  from == RELAY_CODEC_H264 ? NULL : relay_pick_h264_simple(platform, available, ctx));
}
