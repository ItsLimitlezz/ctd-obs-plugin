/* src/relay-encoder.c: what a stream to CTD Relay has to be, the pre-flight check, and which encoder "Send stream"
 * switches to on Windows, macOS and Linux machines. No libobs. */
#include "relay-encoder.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                            \
	do {                                                                   \
		if (!(cond)) {                                                 \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                            \
		}                                                              \
	} while (0)

#define CHECK_STR(actual, expected)                                                                    \
	do {                                                                                           \
		const char *a_ = (actual), *e_ = (expected);                                           \
		if (!a_ || strcmp(a_, e_) != 0) {                                                      \
			printf("FAIL %s:%d: got \"%s\", want \"%s\"\n", __FILE__, __LINE__,            \
			       a_ ? a_ : "(null)", e_);                                                \
			failures++;                                                                    \
		}                                                                                      \
	} while (0)

/* A machine is the list of encoder ids it has. */
typedef struct machine {
	const char *encoders[12];
} machine_t;

static bool has(void *ctx, const char *id)
{
	const machine_t *m = ctx;
	for (int i = 0; m->encoders[i]; i++) {
		if (strcmp(m->encoders[i], id) == 0)
			return true;
	}
	return false;
}

#define VT_H264 "com.apple.videotoolbox.videoencoder.ave.avc"
#define VT_HEVC "com.apple.videotoolbox.videoencoder.ave.hevc"
#define VT_H264_INTEL "com.apple.videotoolbox.videoencoder.h264.gva"

/* the MacBook of the 2026-11-19 incident: an imported profile's Apple VT HEVC went to CTD Relay */
static machine_t mac_silicon = {{"obs_x264", VT_H264, VT_HEVC, "CoreAudio_AAC", "ffmpeg_aac", "ffmpeg_opus"}};
static machine_t mac_intel = {{"obs_x264", VT_H264_INTEL, "CoreAudio_AAC", "ffmpeg_aac"}};
static machine_t mac_no_vt = {{"obs_x264", "ffmpeg_aac"}};
static machine_t win_nvidia = {{"obs_x264", "obs_nvenc_h264_tex", "obs_nvenc_hevc_tex", "obs_nvenc_av1_tex",
				"ffmpeg_svt_av1", "ffmpeg_aac", "ffmpeg_opus"}};
static machine_t win_old_nvidia = {{"obs_x264", "ffmpeg_nvenc", "ffmpeg_aac"}};
static machine_t win_amd = {{"obs_x264", "h264_texture_amf", "h265_texture_amf", "av1_texture_amf", "ffmpeg_aac"}};
static machine_t win_intel = {
	{"obs_x264", "obs_qsv11_v2", "obs_qsv11", "obs_qsv11_hevc", "obs_qsv11_av1", "libfdk_aac", "ffmpeg_aac"}};
static machine_t win_old_intel = {{"obs_x264", "obs_qsv11", "ffmpeg_aac"}};
static machine_t win_nvidia_amd_intel = {
	{"obs_x264", "obs_qsv11_v2", "h264_texture_amf", "obs_nvenc_h264_tex", "ffmpeg_aac"}};
static machine_t win_amd_intel = {{"obs_x264", "obs_qsv11_v2", "h264_texture_amf", "ffmpeg_aac"}};
static machine_t win_plain = {{"obs_x264", "ffmpeg_svt_av1", "ffmpeg_aac"}};
static machine_t linux_vaapi = {{"obs_x264", "ffmpeg_vaapi_tex", "hevc_ffmpeg_vaapi_tex", "ffmpeg_aac"}};
static machine_t linux_nvidia = {{"obs_x264", "obs_nvenc_h264_tex", "ffmpeg_vaapi_tex", "ffmpeg_aac"}};
static machine_t nothing = {{NULL}};

static void test_codecs(void)
{
	CHECK(relay_video_codec("h264") == RELAY_CODEC_H264);
	CHECK(relay_video_codec("H264") == RELAY_CODEC_H264);
	CHECK(relay_video_codec("avc") == RELAY_CODEC_H264);
	CHECK(relay_video_codec("hevc") == RELAY_CODEC_HEVC);
	CHECK(relay_video_codec("h265") == RELAY_CODEC_HEVC);
	CHECK(relay_video_codec("av1") == RELAY_CODEC_AV1);
	CHECK(relay_video_codec("prores") == RELAY_CODEC_OTHER);
	CHECK(relay_video_codec("h2645") == RELAY_CODEC_OTHER);
	CHECK(relay_video_codec("") == RELAY_CODEC_UNKNOWN);
	CHECK(relay_video_codec(NULL) == RELAY_CODEC_UNKNOWN);

	CHECK_STR(relay_video_codec_label("h264"), "H.264");
	CHECK_STR(relay_video_codec_label("hevc"), "HEVC");
	CHECK_STR(relay_video_codec_label("av1"), "AV1");
	CHECK_STR(relay_video_codec_label("prores"), "prores");
	CHECK_STR(relay_video_codec_label(NULL), "");

	CHECK(relay_audio_is_aac("aac"));
	CHECK(relay_audio_is_aac("AAC"));
	CHECK(!relay_audio_is_aac("opus"));
	CHECK(!relay_audio_is_aac(""));
	CHECK(!relay_audio_is_aac(NULL));
}

static void test_preflight(void)
{
	CHECK(relay_stream_issues("h264", "aac") == 0);
	/* the incident: HEVC video with AAC audio */
	CHECK(relay_stream_issues("hevc", "aac") == RELAY_ISSUE_VIDEO);
	CHECK(relay_stream_issues("av1", "aac") == RELAY_ISSUE_VIDEO);
	CHECK(relay_stream_issues("h264", "opus") == RELAY_ISSUE_AUDIO);
	CHECK(relay_stream_issues("hevc", "opus") == (RELAY_ISSUE_VIDEO | RELAY_ISSUE_AUDIO));
	CHECK(relay_stream_issues("prores", "pcm_s24le") == (RELAY_ISSUE_VIDEO | RELAY_ISSUE_AUDIO));
	/* nothing known, nothing claimed */
	CHECK(relay_stream_issues(NULL, NULL) == 0);
	CHECK(relay_stream_issues("", "") == 0);
	CHECK(relay_stream_issues("hevc", NULL) == RELAY_ISSUE_VIDEO);
	CHECK(relay_stream_issues(NULL, "opus") == RELAY_ISSUE_AUDIO);
}

static void test_pick(void)
{
	/* macOS: Apple VT H.264 hardware, else x264 */
	CHECK_STR(relay_pick_h264(RELAY_PLATFORM_MACOS, has, &mac_silicon), VT_H264);
	CHECK_STR(relay_pick_h264(RELAY_PLATFORM_MACOS, has, &mac_intel), VT_H264_INTEL);
	CHECK_STR(relay_pick_h264(RELAY_PLATFORM_MACOS, has, &mac_no_vt), "obs_x264");
	/* Windows: NVENC, then AMF, then Quick Sync, else x264 */
	CHECK_STR(relay_pick_h264(RELAY_PLATFORM_WINDOWS, has, &win_nvidia), "obs_nvenc_h264_tex");
	CHECK_STR(relay_pick_h264(RELAY_PLATFORM_WINDOWS, has, &win_old_nvidia), "ffmpeg_nvenc");
	CHECK_STR(relay_pick_h264(RELAY_PLATFORM_WINDOWS, has, &win_amd), "h264_texture_amf");
	CHECK_STR(relay_pick_h264(RELAY_PLATFORM_WINDOWS, has, &win_intel), "obs_qsv11_v2");
	CHECK_STR(relay_pick_h264(RELAY_PLATFORM_WINDOWS, has, &win_old_intel), "obs_qsv11");
	CHECK_STR(relay_pick_h264(RELAY_PLATFORM_WINDOWS, has, &win_nvidia_amd_intel), "obs_nvenc_h264_tex");
	CHECK_STR(relay_pick_h264(RELAY_PLATFORM_WINDOWS, has, &win_amd_intel), "h264_texture_amf");
	CHECK_STR(relay_pick_h264(RELAY_PLATFORM_WINDOWS, has, &win_plain), "obs_x264");
	/* a Mac's encoders are never offered on Windows, nor the other way round */
	CHECK_STR(relay_pick_h264(RELAY_PLATFORM_WINDOWS, has, &mac_silicon), "obs_x264");
	CHECK_STR(relay_pick_h264(RELAY_PLATFORM_MACOS, has, &win_nvidia), "obs_x264");
	/* Linux */
	CHECK_STR(relay_pick_h264(RELAY_PLATFORM_LINUX, has, &linux_vaapi), "ffmpeg_vaapi_tex");
	CHECK_STR(relay_pick_h264(RELAY_PLATFORM_LINUX, has, &linux_nvidia), "obs_nvenc_h264_tex");
	/* nothing to pick from */
	CHECK(relay_pick_h264(RELAY_PLATFORM_WINDOWS, has, &nothing) == NULL);
	CHECK(relay_pick_h264(RELAY_PLATFORM_MACOS, NULL, NULL) == NULL);

	CHECK_STR(relay_pick_aac(has, &mac_silicon), "CoreAudio_AAC");
	CHECK_STR(relay_pick_aac(has, &win_intel), "libfdk_aac");
	CHECK_STR(relay_pick_aac(has, &win_nvidia), "ffmpeg_aac");
	CHECK(relay_pick_aac(has, &nothing) == NULL);
	CHECK(relay_pick_aac(NULL, NULL) == NULL);
}

static void test_simple(void)
{
	CHECK(relay_simple_codec("x264") == RELAY_CODEC_H264);
	CHECK(relay_simple_codec("x264_lowcpu") == RELAY_CODEC_H264);
	CHECK(relay_simple_codec("nvenc") == RELAY_CODEC_H264);
	CHECK(relay_simple_codec("amd") == RELAY_CODEC_H264);
	CHECK(relay_simple_codec("qsv") == RELAY_CODEC_H264);
	CHECK(relay_simple_codec("apple_h264") == RELAY_CODEC_H264);
	CHECK(relay_simple_codec("apple_hevc") == RELAY_CODEC_HEVC);
	CHECK(relay_simple_codec("nvenc_hevc") == RELAY_CODEC_HEVC);
	CHECK(relay_simple_codec("amd_hevc") == RELAY_CODEC_HEVC);
	CHECK(relay_simple_codec("nvenc_av1") == RELAY_CODEC_AV1);
	CHECK(relay_simple_codec("amd_av1") == RELAY_CODEC_AV1);
	CHECK(relay_simple_codec("qsv_av1") == RELAY_CODEC_AV1);
	CHECK(relay_simple_codec("") == RELAY_CODEC_UNKNOWN);
	CHECK(relay_simple_codec(NULL) == RELAY_CODEC_UNKNOWN);

	CHECK_STR(relay_pick_h264_simple(RELAY_PLATFORM_MACOS, has, &mac_silicon), "apple_h264");
	/* Simple mode has no word for the Intel Mac hardware encoder, the old Quick Sync or VA-API */
	CHECK_STR(relay_pick_h264_simple(RELAY_PLATFORM_MACOS, has, &mac_intel), "x264");
	CHECK_STR(relay_pick_h264_simple(RELAY_PLATFORM_WINDOWS, has, &win_old_intel), "x264");
	CHECK_STR(relay_pick_h264_simple(RELAY_PLATFORM_LINUX, has, &linux_vaapi), "x264");
	CHECK_STR(relay_pick_h264_simple(RELAY_PLATFORM_WINDOWS, has, &win_nvidia), "nvenc");
	CHECK_STR(relay_pick_h264_simple(RELAY_PLATFORM_WINDOWS, has, &win_old_nvidia), "nvenc");
	CHECK_STR(relay_pick_h264_simple(RELAY_PLATFORM_WINDOWS, has, &win_amd), "amd");
	CHECK_STR(relay_pick_h264_simple(RELAY_PLATFORM_WINDOWS, has, &win_intel), "qsv");
	CHECK_STR(relay_pick_h264_simple(RELAY_PLATFORM_WINDOWS, has, &win_plain), "x264");
	CHECK_STR(relay_pick_h264_simple(RELAY_PLATFORM_WINDOWS, has, &nothing), "x264");
}

static void test_fix(void)
{
	relay_video_fix_t fix;

	/* the incident: Apple VT HEVC becomes Apple VT H.264 hardware */
	CHECK(relay_video_fix(VT_HEVC, "hevc", RELAY_PLATFORM_MACOS, has, &mac_silicon, &fix));
	CHECK(fix.needed && fix.possible);
	CHECK(fix.from == RELAY_CODEC_HEVC);
	CHECK_STR(fix.encoder, VT_H264);

	/* H.264 already: left alone, even when a hardware encoder is there (the user chose x264) */
	CHECK(relay_video_fix("obs_x264", "h264", RELAY_PLATFORM_WINDOWS, has, &win_nvidia, &fix));
	CHECK(!fix.needed && fix.possible);
	CHECK(fix.from == RELAY_CODEC_H264);
	CHECK_STR(fix.encoder, "obs_x264");
	CHECK(relay_video_fix(VT_H264, "h264", RELAY_PLATFORM_MACOS, has, &mac_silicon, &fix));
	CHECK(!fix.needed);
	CHECK_STR(fix.encoder, VT_H264);

	/* AV1 and HEVC on Windows go to that machine's hardware H.264 */
	CHECK(relay_video_fix("obs_nvenc_av1_tex", "av1", RELAY_PLATFORM_WINDOWS, has, &win_nvidia, &fix));
	CHECK(fix.needed && fix.from == RELAY_CODEC_AV1);
	CHECK_STR(fix.encoder, "obs_nvenc_h264_tex");
	CHECK(relay_video_fix("h265_texture_amf", "hevc", RELAY_PLATFORM_WINDOWS, has, &win_amd, &fix));
	CHECK_STR(fix.encoder, "h264_texture_amf");
	CHECK(relay_video_fix("obs_qsv11_hevc", "hevc", RELAY_PLATFORM_WINDOWS, has, &win_intel, &fix));
	CHECK_STR(fix.encoder, "obs_qsv11_v2");
	/* software AV1 with no GPU encoder: x264 */
	CHECK(relay_video_fix("ffmpeg_svt_av1", "av1", RELAY_PLATFORM_WINDOWS, has, &win_plain, &fix));
	CHECK_STR(fix.encoder, "obs_x264");

	/* an imported profile naming an encoder this machine doesn't have (no codec): switched as well */
	CHECK(relay_video_fix("obs_nvenc_hevc_tex", NULL, RELAY_PLATFORM_MACOS, has, &mac_silicon, &fix));
	CHECK(fix.needed && fix.from == RELAY_CODEC_UNKNOWN);
	CHECK_STR(fix.encoder, VT_H264);
	CHECK(relay_video_fix("", "", RELAY_PLATFORM_MACOS, has, &mac_no_vt, &fix));
	CHECK(fix.needed);
	CHECK_STR(fix.encoder, "obs_x264");
	CHECK(relay_video_fix(NULL, NULL, RELAY_PLATFORM_LINUX, has, &linux_vaapi, &fix));
	CHECK_STR(fix.encoder, "ffmpeg_vaapi_tex");

	/* no H.264 encoder at all: says so and keeps what was there */
	CHECK(!relay_video_fix(VT_HEVC, "hevc", RELAY_PLATFORM_MACOS, has, &nothing, &fix));
	CHECK(fix.needed && !fix.possible);
	CHECK_STR(fix.encoder, VT_HEVC);
	CHECK(!relay_video_fix(NULL, "hevc", RELAY_PLATFORM_MACOS, NULL, NULL, &fix));
	CHECK_STR(fix.encoder, "");

	/* Simple output mode */
	CHECK(relay_video_fix_simple("apple_hevc", RELAY_PLATFORM_MACOS, has, &mac_silicon, &fix));
	CHECK(fix.needed && fix.from == RELAY_CODEC_HEVC);
	CHECK_STR(fix.encoder, "apple_h264");
	CHECK(relay_video_fix_simple("nvenc_av1", RELAY_PLATFORM_WINDOWS, has, &win_nvidia, &fix));
	CHECK(fix.needed && fix.from == RELAY_CODEC_AV1);
	CHECK_STR(fix.encoder, "nvenc");
	CHECK(relay_video_fix_simple("amd_hevc", RELAY_PLATFORM_WINDOWS, has, &win_plain, &fix));
	CHECK_STR(fix.encoder, "x264");
	CHECK(relay_video_fix_simple("x264", RELAY_PLATFORM_WINDOWS, has, &win_nvidia, &fix));
	CHECK(!fix.needed);
	CHECK_STR(fix.encoder, "x264");
	CHECK(relay_video_fix_simple("", RELAY_PLATFORM_WINDOWS, has, &win_nvidia, &fix));
	CHECK(!fix.needed);
	CHECK(relay_video_fix_simple(NULL, RELAY_PLATFORM_WINDOWS, has, &win_nvidia, &fix));
	CHECK(!fix.needed);
	CHECK_STR(fix.encoder, "");
}

int main(void)
{
	test_codecs();
	test_preflight();
	test_pick();
	test_simple();
	test_fix();
	/* built for one of the three */
	const relay_platform_t platform = relay_platform_current();
	CHECK(platform == RELAY_PLATFORM_WINDOWS || platform == RELAY_PLATFORM_MACOS ||
	      platform == RELAY_PLATFORM_LINUX);
	if (failures) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("ctd-relay-encoder-tests: all passed\n");
	return 0;
}
