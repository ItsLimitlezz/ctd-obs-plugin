/* What a stream to CTD Relay has to be, and how "Send stream" gets it there.
 *
 * CTD Relay copies OBS's feed to the platforms over classic RTMP/FLV without re-encoding, so the feed has to be what
 * they all take: H.264 video and AAC audio. HEVC or AV1 reaches CTD Relay fine and then every destination fails to
 * start.
 *
 * Pure logic: C only, no libobs. Codecs are libobs's codec strings (obs_get_encoder_codec: "h264", "hevc", "av1",
 * "aac", "opus"). Which encoders this machine has is asked through a callback, so the same code runs in the dock
 * (obs_get_encoder_codec() != NULL) and in the tests (fixed sets for Windows, macOS and Linux). */
#ifndef CTD_RELAY_ENCODER_H
#define CTD_RELAY_ENCODER_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum relay_codec {
	RELAY_CODEC_UNKNOWN = 0, /* no codec string: the encoder isn't on this machine, or nothing is set */
	RELAY_CODEC_H264,
	RELAY_CODEC_HEVC,
	RELAY_CODEC_AV1,
	RELAY_CODEC_OTHER,
} relay_codec_t;

typedef enum relay_platform {
	RELAY_PLATFORM_WINDOWS,
	RELAY_PLATFORM_MACOS,
	RELAY_PLATFORM_LINUX,
} relay_platform_t;

/* The platform this was built for. */
relay_platform_t relay_platform_current(void);

/* True when an encoder with this OBS encoder id exists on this machine. */
typedef bool (*relay_available_fn)(void *ctx, const char *encoder_id);

/* "h264" / "avc", "hevc" / "h265", "av1"; anything else is OTHER, NULL or "" is UNKNOWN. Case doesn't matter. */
relay_codec_t relay_video_codec(const char *codec);
/* "H.264", "HEVC", "AV1"; the codec string itself for anything else ("" when there is none). For the dock. */
const char *relay_video_codec_label(const char *codec);
bool relay_audio_is_aac(const char *codec);

/* The pre-flight check. A codec that isn't known (NULL or "") is not reported: there is nothing to say about it. */
#define RELAY_ISSUE_VIDEO 1u /* the video isn't H.264 */
#define RELAY_ISSUE_AUDIO 2u /* the audio isn't AAC */
unsigned relay_stream_issues(const char *video_codec, const char *audio_codec);

/* The H.264 encoder to switch to: the platform's hardware encoder when this machine has one (Apple VideoToolbox on
 * macOS; NVENC, then AMF, then Quick Sync on Windows; those and VA-API on Linux), else x264. NULL when the machine
 * has no H.264 encoder at all. */
const char *relay_pick_h264(relay_platform_t platform, relay_available_fn available, void *ctx);
/* The AAC encoder to switch to (CoreAudio, then FDK, then FFmpeg's), or NULL. */
const char *relay_pick_aac(relay_available_fn available, void *ctx);

/* OBS's Simple output mode names its stream encoder with a short word ("x264", "nvenc", "apple_hevc", ...). */
relay_codec_t relay_simple_codec(const char *simple_name);
/* The short word for the H.264 encoder to switch to in Simple mode, hardware first; always one ("x264" at worst). */
const char *relay_pick_h264_simple(relay_platform_t platform, relay_available_fn available, void *ctx);

/* What "Send stream" does to an output's video encoder. */
typedef struct relay_video_fix_result {
	bool needed;        /* the current encoder isn't H.264 (or isn't on this machine) */
	bool possible;      /* false: needed, and there is no H.264 encoder to switch to */
	relay_codec_t from; /* the codec being replaced */
	char encoder[64];   /* the encoder to use: the current one when it stays */
} relay_video_fix_t;

/* Advanced output mode: `encoder_id` is the stream encoder and `codec` its libobs codec (NULL when the encoder isn't
 * on this machine). An H.264 encoder is never swapped for another one: that choice is the user's. Returns true when
 * the output ends up H.264 (it stays, or there is an encoder to switch to). */
bool relay_video_fix(const char *encoder_id, const char *codec, relay_platform_t platform, relay_available_fn available,
		     void *ctx, relay_video_fix_t *out);
/* Simple output mode: `simple_name` in, a short word out. */
bool relay_video_fix_simple(const char *simple_name, relay_platform_t platform, relay_available_fn available, void *ctx,
			    relay_video_fix_t *out);

#ifdef __cplusplus
}
#endif

#endif
