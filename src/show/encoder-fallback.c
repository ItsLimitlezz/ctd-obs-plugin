/* Encoder fallback for Show Profiles (design section 1.4). See encoder-fallback.h. Pure C, no libobs. */
#include "encoder-fallback.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static bool contains(const char *text, const char *part)
{
	return strstr(text, part) != NULL;
}

static bool starts_with(const char *text, const char *prefix)
{
	return strncmp(text, prefix, strlen(prefix)) == 0;
}

static bool equals_nocase(const char *a, const char *b)
{
	for (; *a && *b; a++, b++) {
		if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
			return false;
	}
	return *a == *b;
}

encoder_class_t encoder_fallback_class(const char *id)
{
	if (!id || !*id)
		return ENCODER_CLASS_UNKNOWN;
	if (strcmp(id, "obs_x264") == 0)
		return ENCODER_CLASS_X264;
	if (contains(id, "nvenc"))
		return ENCODER_CLASS_NVENC;
	if (starts_with(id, "obs_qsv11"))
		return ENCODER_CLASS_QSV;
	if (contains(id, "amf") &&
	    (contains(id, "h264") || contains(id, "h265") || contains(id, "hevc") || contains(id, "av1")))
		return ENCODER_CLASS_AMF;
	if (starts_with(id, "com.apple.videotoolbox."))
		return ENCODER_CLASS_VIDEOTOOLBOX;
	if (strcmp(id, "ffmpeg_svt_av1") == 0 || strcmp(id, "ffmpeg_aom_av1") == 0)
		return ENCODER_CLASS_SOFTWARE_AV1;
	return ENCODER_CLASS_UNKNOWN;
}

encoder_codec_t encoder_fallback_codec(const char *id)
{
	const encoder_class_t cls = encoder_fallback_class(id);
	if (cls == ENCODER_CLASS_UNKNOWN)
		return ENCODER_CODEC_UNKNOWN;
	if (cls == ENCODER_CLASS_X264)
		return ENCODER_CODEC_H264;
	if (cls == ENCODER_CLASS_SOFTWARE_AV1 || contains(id, "av1"))
		return ENCODER_CODEC_AV1;
	if (contains(id, "hevc") || contains(id, "h265"))
		return ENCODER_CODEC_HEVC;
	if (cls == ENCODER_CLASS_VIDEOTOOLBOX && contains(id, "prores"))
		return ENCODER_CODEC_UNKNOWN; /* not a streaming codec */
	return ENCODER_CODEC_H264;
}

const char *encoder_fallback_id(encoder_class_t cls, encoder_codec_t codec)
{
	switch (cls) {
	case ENCODER_CLASS_NVENC:
		return codec == ENCODER_CODEC_H264   ? "obs_nvenc_h264_tex"
		       : codec == ENCODER_CODEC_HEVC ? "obs_nvenc_hevc_tex"
		       : codec == ENCODER_CODEC_AV1  ? "obs_nvenc_av1_tex"
						     : NULL;
	case ENCODER_CLASS_QSV:
		return codec == ENCODER_CODEC_H264   ? "obs_qsv11"
		       : codec == ENCODER_CODEC_HEVC ? "obs_qsv11_hevc"
		       : codec == ENCODER_CODEC_AV1  ? "obs_qsv11_av1"
						     : NULL;
	case ENCODER_CLASS_AMF:
		return codec == ENCODER_CODEC_H264   ? "h264_texture_amf"
		       : codec == ENCODER_CODEC_HEVC ? "h265_texture_amf"
		       : codec == ENCODER_CODEC_AV1  ? "av1_texture_amf"
						     : NULL;
	case ENCODER_CLASS_VIDEOTOOLBOX:
		return codec == ENCODER_CODEC_H264   ? "com.apple.videotoolbox.videoencoder.ave.avc"
		       : codec == ENCODER_CODEC_HEVC ? "com.apple.videotoolbox.videoencoder.ave.hevc"
						     : NULL;
	case ENCODER_CLASS_X264:
		return codec == ENCODER_CODEC_H264 ? "obs_x264" : NULL;
	case ENCODER_CLASS_SOFTWARE_AV1:
		return codec == ENCODER_CODEC_AV1 ? "ffmpeg_svt_av1" : NULL;
	default:
		return NULL;
	}
}

const char *encoder_fallback_class_name(encoder_class_t cls)
{
	switch (cls) {
	case ENCODER_CLASS_NVENC:
		return "NVIDIA NVENC";
	case ENCODER_CLASS_QSV:
		return "Intel Quick Sync";
	case ENCODER_CLASS_AMF:
		return "AMD AMF";
	case ENCODER_CLASS_VIDEOTOOLBOX:
		return "Apple VideoToolbox";
	case ENCODER_CLASS_X264:
		return "x264 (software)";
	case ENCODER_CLASS_SOFTWARE_AV1:
		return "SVT-AV1 (software)";
	default:
		return "an unknown encoder";
	}
}

static const char *codec_name(encoder_codec_t codec)
{
	switch (codec) {
	case ENCODER_CODEC_H264:
		return "H.264";
	case ENCODER_CODEC_HEVC:
		return "HEVC";
	case ENCODER_CODEC_AV1:
		return "AV1";
	default:
		return "video";
	}
}

const char *encoder_fallback_rate_control(encoder_class_t cls, const char *rc, char *out, size_t size)
{
	const bool abr_class = cls == ENCODER_CLASS_X264 || cls == ENCODER_CLASS_VIDEOTOOLBOX;
	const char *mapped = rc ? rc : "";
	if (equals_nocase(mapped, "CBR"))
		mapped = "CBR";
	else if (equals_nocase(mapped, "VBR") || equals_nocase(mapped, "ABR"))
		mapped = abr_class ? "ABR" : "VBR";
	else if (equals_nocase(mapped, "CQP") || equals_nocase(mapped, "CRF") || equals_nocase(mapped, "ICQ") ||
		 equals_nocase(mapped, "LA_ICQ") || equals_nocase(mapped, "CQ"))
		mapped = abr_class ? "CRF" : "CQP";
	if (size)
		snprintf(out, size, "%s", mapped);
	return out;
}

static void carry_over(const encoder_settings_t *in, const char *id, encoder_class_t cls, encoder_settings_t *out)
{
	memset(out, 0, sizeof(*out));
	snprintf(out->encoder, sizeof(out->encoder), "%s", id);
	out->bitrate = in->bitrate;
	out->keyint_sec = in->keyint_sec > 0 ? in->keyint_sec : ENCODER_FALLBACK_KEYINT_SEC;
	encoder_fallback_rate_control(cls, in->rate_control, out->rate_control, sizeof(out->rate_control));
}

/* The first available encoder of `codec`, best class first. */
static const char *best_for(encoder_codec_t codec, encoder_available_fn available, void *ctx, encoder_class_t *cls)
{
	static const encoder_class_t order[] = {ENCODER_CLASS_NVENC,        ENCODER_CLASS_QSV,
						ENCODER_CLASS_AMF,          ENCODER_CLASS_VIDEOTOOLBOX,
						ENCODER_CLASS_SOFTWARE_AV1, ENCODER_CLASS_X264};
	for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
		const char *id = encoder_fallback_id(order[i], codec);
		if (id && available(ctx, id)) {
			*cls = order[i];
			return id;
		}
	}
	return NULL;
}

bool encoder_fallback(const encoder_settings_t *in, encoder_available_fn available, void *ctx,
		      encoder_fallback_result_t *result)
{
	memset(result, 0, sizeof(*result));
	result->settings = *in;
	if (!available)
		return false;

	if (in->encoder[0] && available(ctx, in->encoder)) {
		result->available = true; /* nothing to do */
		return true;
	}

	const encoder_class_t wanted_class = encoder_fallback_class(in->encoder);
	encoder_codec_t codec = encoder_fallback_codec(in->encoder);
	if (codec == ENCODER_CODEC_UNKNOWN)
		codec = ENCODER_CODEC_H264;

	encoder_class_t cls = ENCODER_CLASS_UNKNOWN;
	const char *id = best_for(codec, available, ctx, &cls);
	bool codec_changed = false;
	if (!id && codec != ENCODER_CODEC_H264) {
		id = best_for(ENCODER_CODEC_H264, available, ctx, &cls);
		codec_changed = id != NULL;
	}
	if (!id) {
		snprintf(result->warning, sizeof(result->warning), "No video encoder is available on this PC");
		return false;
	}

	carry_over(in, id, cls, &result->settings);
	result->available = true;
	result->fell_back = true;
	result->codec_changed = codec_changed;

	char wanted[96];
	if (wanted_class == ENCODER_CLASS_UNKNOWN)
		snprintf(wanted, sizeof(wanted), "The encoder \"%.40s\"", in->encoder);
	else
		snprintf(wanted, sizeof(wanted), "%s %s", encoder_fallback_class_name(wanted_class), codec_name(codec));
	snprintf(result->warning, sizeof(result->warning), "%s isn't available on this PC; using %s %s instead", wanted,
		 encoder_fallback_class_name(cls), codec_name(codec_changed ? ENCODER_CODEC_H264 : codec));
	return true;
}
