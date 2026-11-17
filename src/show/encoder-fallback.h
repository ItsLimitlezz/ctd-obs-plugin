/* Encoder fallback for Show Profiles (design section 1.4, ticket T15).
 *
 * Pure logic: C only, no libobs. A bundle's streamEncoder.json names an encoder; when this machine doesn't have it,
 * encoder_fallback() picks the best equivalent that it does have and carries the bitrate, the 2 s keyframe interval
 * and the rate control over. Which encoders exist is asked through a callback, so the same code runs in the plugin
 * (wrap obs_enum_encoder_types) and in ctd-encoder-fallback-tests (fixed sets for Windows and macOS).
 *
 * Order, best first (design 1.4): NVENC -> QSV -> AMF -> Apple VideoToolbox -> x264. The codec is kept (H.264 stays
 * H.264, HEVC stays HEVC, AV1 stays AV1) for as long as some encoder of that codec is available. If none is, the
 * result is H.264 (hardware first, then x264) and codec_changed is set so the dock can say so. The bundle itself is
 * never changed: this only decides what the staged settings use on this machine. */
#ifndef CTD_SHOW_ENCODER_FALLBACK_H
#define CTD_SHOW_ENCODER_FALLBACK_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ENCODER_FALLBACK_KEYINT_SEC 2 /* CTD Relay wants a keyframe every 2 s */

typedef enum encoder_class {
	ENCODER_CLASS_UNKNOWN = 0,
	ENCODER_CLASS_NVENC,
	ENCODER_CLASS_QSV,
	ENCODER_CLASS_AMF,
	ENCODER_CLASS_VIDEOTOOLBOX,
	ENCODER_CLASS_X264,
	ENCODER_CLASS_SOFTWARE_AV1, /* SVT-AV1 through FFmpeg: the only software AV1 encoder OBS ships */
} encoder_class_t;

typedef enum encoder_codec {
	ENCODER_CODEC_UNKNOWN = 0,
	ENCODER_CODEC_H264,
	ENCODER_CODEC_HEVC,
	ENCODER_CODEC_AV1,
} encoder_codec_t;

/* True when an encoder with this OBS encoder id exists on this machine. */
typedef bool (*encoder_available_fn)(void *ctx, const char *encoder_id);

/* The part of streamEncoder.json that has to survive a change of encoder. */
typedef struct encoder_settings {
	char encoder[64];      /* OBS encoder id, e.g. "obs_nvenc_h264_tex" */
	int bitrate;           /* kbps, copied as is */
	int keyint_sec;        /* seconds; 0 (automatic) becomes 2 */
	char rate_control[16]; /* "CBR", "VBR", "ABR", "CRF", "CQP", ...; "" when the bundle doesn't say */
} encoder_settings_t;

typedef struct encoder_fallback_result {
	encoder_settings_t settings; /* what to stage on this machine */
	bool available;              /* false: not even x264 (or no encoder at all): settings is the input, unchanged */
	bool fell_back;              /* settings.encoder differs from the requested one */
	bool codec_changed; /* the fallback is a different codec (HEVC/AV1 with no encoder of that codec here) */
	char warning[200];  /* one line for the dock, "" when nothing changed */
} encoder_fallback_result_t;

/* Class and codec of an OBS encoder id. Unknown ids give ENCODER_CLASS_UNKNOWN / ENCODER_CODEC_UNKNOWN. */
encoder_class_t encoder_fallback_class(const char *encoder_id);
encoder_codec_t encoder_fallback_codec(const char *encoder_id);

/* The OBS encoder id for a class and codec, or NULL when there is none (e.g. VideoToolbox AV1). */
const char *encoder_fallback_id(encoder_class_t cls, encoder_codec_t codec);

/* "NVIDIA NVENC", "Apple VideoToolbox", ... */
const char *encoder_fallback_class_name(encoder_class_t cls);

/* The rate control to use on `cls` for a bundle's `rate_control` (e.g. "VBR" is "ABR" on x264 and VideoToolbox).
 * Writes into out and returns it. Quality modes (CQP, CRF, ICQ, ...) become the class's own quality mode; the
 * quality value itself is not carried, the encoder's default applies. */
const char *encoder_fallback_rate_control(encoder_class_t cls, const char *rate_control, char *out, size_t size);

/* Decides the encoder. `in->encoder` is what the bundle names. Returns result->available. */
bool encoder_fallback(const encoder_settings_t *in, encoder_available_fn available, void *ctx,
		      encoder_fallback_result_t *result);

#ifdef __cplusplus
}
#endif

#endif
