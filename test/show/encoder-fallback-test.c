/* ctd-encoder-fallback-tests: the Show Profiles encoder fallback (ticket T15) with Windows and macOS encoder sets.
 * No libobs. Run with ctest (see CMakeLists.txt). */
#include "../../src/show/encoder-fallback.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                \
	do {                                                                       \
		if (!(cond)) {                                                     \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
			failures++;                                                \
		}                                                                  \
	} while (0)

#define CHECK_STR(actual, expected)                                                                          \
	do {                                                                                                 \
		if (strcmp((actual), (expected)) != 0) {                                                     \
			printf("FAIL %s:%d: got \"%s\", want \"%s\"\n", __FILE__, __LINE__, (actual), (expected)); \
			failures++;                                                                          \
		}                                                                                            \
	} while (0)

/* A machine is the list of encoder ids it has. */
typedef struct machine {
	const char *name;
	const char *encoders[8];
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

static const machine_t win_nvidia = {"Windows + NVIDIA",
				     {"obs_x264", "obs_nvenc_h264_tex", "obs_nvenc_hevc_tex", "obs_nvenc_av1_tex",
				      "ffmpeg_svt_av1"}};
static const machine_t win_intel = {"Windows + Intel iGPU",
				    {"obs_x264", "obs_qsv11", "obs_qsv11_hevc", "obs_qsv11_av1"}};
static const machine_t win_amd = {"Windows + AMD",
				  {"obs_x264", "h264_texture_amf", "h265_texture_amf", "av1_texture_amf"}};
static const machine_t win_plain = {"Windows, no GPU encoder", {"obs_x264", "ffmpeg_svt_av1"}};
static const machine_t win_nvidia_and_intel = {"Windows + NVIDIA + Intel",
					       {"obs_x264", "obs_nvenc_h264_tex", "obs_qsv11"}};
static const machine_t mac_vt = {"macOS",
				 {"obs_x264", "com.apple.videotoolbox.videoencoder.ave.avc",
				  "com.apple.videotoolbox.videoencoder.ave.hevc"}};
static const machine_t mac_x264_only = {"macOS, VideoToolbox missing", {"obs_x264"}};
static const machine_t nothing = {"no encoders", {NULL}};

static encoder_settings_t bundle(const char *encoder, int bitrate, int keyint, const char *rc)
{
	encoder_settings_t s;
	memset(&s, 0, sizeof(s));
	snprintf(s.encoder, sizeof(s.encoder), "%s", encoder);
	s.bitrate = bitrate;
	s.keyint_sec = keyint;
	snprintf(s.rate_control, sizeof(s.rate_control), "%s", rc);
	return s;
}

static encoder_fallback_result_t run(const machine_t *m, const encoder_settings_t *in)
{
	encoder_fallback_result_t r;
	const bool ok = encoder_fallback(in, has, (void *)m, &r);
	CHECK(ok == r.available);
	return r;
}

static void test_ids(void)
{
	CHECK(encoder_fallback_class("obs_nvenc_h264_tex") == ENCODER_CLASS_NVENC);
	CHECK(encoder_fallback_class("jim_nvenc") == ENCODER_CLASS_NVENC);
	CHECK(encoder_fallback_class("obs_qsv11") == ENCODER_CLASS_QSV);
	CHECK(encoder_fallback_class("h264_texture_amf") == ENCODER_CLASS_AMF);
	CHECK(encoder_fallback_class("com.apple.videotoolbox.videoencoder.ave.avc") == ENCODER_CLASS_VIDEOTOOLBOX);
	CHECK(encoder_fallback_class("com.apple.videotoolbox.videoencoder.h264.gva") == ENCODER_CLASS_VIDEOTOOLBOX);
	CHECK(encoder_fallback_class("obs_x264") == ENCODER_CLASS_X264);
	CHECK(encoder_fallback_class("something_else") == ENCODER_CLASS_UNKNOWN);
	CHECK(encoder_fallback_class("") == ENCODER_CLASS_UNKNOWN);
	CHECK(encoder_fallback_class(NULL) == ENCODER_CLASS_UNKNOWN);

	CHECK(encoder_fallback_codec("obs_nvenc_hevc_tex") == ENCODER_CODEC_HEVC);
	CHECK(encoder_fallback_codec("obs_nvenc_av1_tex") == ENCODER_CODEC_AV1);
	CHECK(encoder_fallback_codec("obs_qsv11_hevc") == ENCODER_CODEC_HEVC);
	CHECK(encoder_fallback_codec("h265_texture_amf") == ENCODER_CODEC_HEVC);
	CHECK(encoder_fallback_codec("com.apple.videotoolbox.videoencoder.ave.hevc") == ENCODER_CODEC_HEVC);
	CHECK(encoder_fallback_codec("obs_x264") == ENCODER_CODEC_H264);
	CHECK(encoder_fallback_codec("something_else") == ENCODER_CODEC_UNKNOWN);

	/* every id the chain can hand out is classified back to its own class and codec */
	for (encoder_class_t c = ENCODER_CLASS_NVENC; c <= ENCODER_CLASS_SOFTWARE_AV1; c++) {
		for (encoder_codec_t k = ENCODER_CODEC_H264; k <= ENCODER_CODEC_AV1; k++) {
			const char *id = encoder_fallback_id(c, k);
			if (!id)
				continue;
			CHECK(encoder_fallback_class(id) == c);
			CHECK(encoder_fallback_codec(id) == k);
		}
	}
	CHECK(encoder_fallback_id(ENCODER_CLASS_VIDEOTOOLBOX, ENCODER_CODEC_AV1) == NULL);
	CHECK(encoder_fallback_id(ENCODER_CLASS_X264, ENCODER_CODEC_HEVC) == NULL);
}

static void test_rate_control(void)
{
	char rc[16];
	CHECK_STR(encoder_fallback_rate_control(ENCODER_CLASS_X264, "CBR", rc, sizeof(rc)), "CBR");
	CHECK_STR(encoder_fallback_rate_control(ENCODER_CLASS_NVENC, "cbr", rc, sizeof(rc)), "CBR");
	CHECK_STR(encoder_fallback_rate_control(ENCODER_CLASS_X264, "VBR", rc, sizeof(rc)), "ABR");
	CHECK_STR(encoder_fallback_rate_control(ENCODER_CLASS_VIDEOTOOLBOX, "VBR", rc, sizeof(rc)), "ABR");
	CHECK_STR(encoder_fallback_rate_control(ENCODER_CLASS_AMF, "ABR", rc, sizeof(rc)), "VBR");
	CHECK_STR(encoder_fallback_rate_control(ENCODER_CLASS_X264, "CQP", rc, sizeof(rc)), "CRF");
	CHECK_STR(encoder_fallback_rate_control(ENCODER_CLASS_QSV, "CRF", rc, sizeof(rc)), "CQP");
	CHECK_STR(encoder_fallback_rate_control(ENCODER_CLASS_QSV, "", rc, sizeof(rc)), "");
	CHECK_STR(encoder_fallback_rate_control(ENCODER_CLASS_QSV, NULL, rc, sizeof(rc)), "");
	CHECK_STR(encoder_fallback_rate_control(ENCODER_CLASS_QSV, "Lossless", rc, sizeof(rc)), "Lossless");
}

static void test_present(void)
{
	/* the encoder is here: nothing changes, not even a 0 s keyframe */
	encoder_settings_t in = bundle("obs_nvenc_h264_tex", 6000, 0, "VBR");
	encoder_fallback_result_t r = run(&win_nvidia, &in);
	CHECK(r.available && !r.fell_back && !r.codec_changed);
	CHECK_STR(r.settings.encoder, "obs_nvenc_h264_tex");
	CHECK(r.settings.keyint_sec == 0);
	CHECK_STR(r.settings.rate_control, "VBR");
	CHECK_STR(r.warning, "");

	in = bundle("com.apple.videotoolbox.videoencoder.ave.avc", 4000, 2, "CBR");
	r = run(&mac_vt, &in);
	CHECK(r.available && !r.fell_back);
}

static void test_windows(void)
{
	/* NVENC bundle, no NVIDIA: Intel, then AMD, then x264 */
	encoder_settings_t in = bundle("obs_nvenc_h264_tex", 12000, 2, "CBR");
	encoder_fallback_result_t r = run(&win_intel, &in);
	CHECK(r.available && r.fell_back && !r.codec_changed);
	CHECK_STR(r.settings.encoder, "obs_qsv11");
	CHECK(r.settings.bitrate == 12000 && r.settings.keyint_sec == 2);
	CHECK_STR(r.settings.rate_control, "CBR");
	CHECK_STR(r.warning, "NVIDIA NVENC H.264 isn't available on this PC; using Intel Quick Sync H.264 instead");

	r = run(&win_amd, &in);
	CHECK_STR(r.settings.encoder, "h264_texture_amf");
	CHECK(r.settings.bitrate == 12000 && r.settings.keyint_sec == 2);
	CHECK_STR(r.settings.rate_control, "CBR");

	r = run(&win_plain, &in);
	CHECK(r.available && r.fell_back);
	CHECK_STR(r.settings.encoder, "obs_x264");
	CHECK(r.settings.bitrate == 12000 && r.settings.keyint_sec == 2);
	CHECK_STR(r.settings.rate_control, "CBR");

	/* hardware first: with both Intel and NVIDIA the chain order wins (NVENC over QSV) */
	in = bundle("h264_texture_amf", 6000, 2, "CBR");
	r = run(&win_nvidia_and_intel, &in);
	CHECK_STR(r.settings.encoder, "obs_nvenc_h264_tex");

	/* AMD bundle on Intel */
	r = run(&win_intel, &in);
	CHECK_STR(r.settings.encoder, "obs_qsv11");

	/* legacy NVENC id (older OBS bundles) is still recognized */
	in = bundle("jim_nvenc", 8000, 2, "CBR");
	r = run(&win_amd, &in);
	CHECK_STR(r.settings.encoder, "h264_texture_amf");
}

static void test_macos(void)
{
	/* a Windows NVENC bundle on a Mac: VideoToolbox first */
	encoder_settings_t in = bundle("obs_nvenc_h264_tex", 4000, 2, "CBR");
	encoder_fallback_result_t r = run(&mac_vt, &in);
	CHECK(r.available && r.fell_back && !r.codec_changed);
	CHECK_STR(r.settings.encoder, "com.apple.videotoolbox.videoencoder.ave.avc");
	CHECK(r.settings.bitrate == 4000 && r.settings.keyint_sec == 2);
	CHECK_STR(r.settings.rate_control, "CBR");
	CHECK_STR(r.warning, "NVIDIA NVENC H.264 isn't available on this PC; using Apple VideoToolbox H.264 instead");

	/* a Mac bundle on Windows with NVIDIA: VideoToolbox is missing, NVENC takes over; VBR stays VBR there */
	in = bundle("com.apple.videotoolbox.videoencoder.ave.avc", 6000, 2, "ABR");
	r = run(&win_nvidia, &in);
	CHECK_STR(r.settings.encoder, "obs_nvenc_h264_tex");
	CHECK_STR(r.settings.rate_control, "VBR");
	CHECK_STR(r.warning, "Apple VideoToolbox H.264 isn't available on this PC; using NVIDIA NVENC H.264 instead");

	/* ... on Windows with nothing but x264, ABR stays ABR */
	r = run(&win_plain, &in);
	CHECK_STR(r.settings.encoder, "obs_x264");
	CHECK_STR(r.settings.rate_control, "ABR");

	/* the older VideoToolbox id names the same class */
	in = bundle("com.apple.videotoolbox.videoencoder.h264.gva", 6000, 2, "CBR");
	r = run(&win_amd, &in);
	CHECK_STR(r.settings.encoder, "h264_texture_amf");

	/* a Mac whose VideoToolbox encoder is missing falls to x264 */
	in = bundle("com.apple.videotoolbox.videoencoder.ave.avc", 6000, 2, "CBR");
	r = run(&mac_x264_only, &in);
	CHECK_STR(r.settings.encoder, "obs_x264");
}

static void test_codec(void)
{
	/* HEVC stays HEVC while some encoder of that codec exists */
	encoder_settings_t in = bundle("obs_nvenc_hevc_tex", 6000, 2, "CBR");
	encoder_fallback_result_t r = run(&win_amd, &in);
	CHECK_STR(r.settings.encoder, "h265_texture_amf");
	CHECK(!r.codec_changed);

	r = run(&mac_vt, &in);
	CHECK_STR(r.settings.encoder, "com.apple.videotoolbox.videoencoder.ave.hevc");

	/* no HEVC encoder here: H.264, and the dock is told the codec changed */
	r = run(&win_plain, &in);
	CHECK(r.available && r.fell_back && r.codec_changed);
	CHECK_STR(r.settings.encoder, "obs_x264");
	CHECK(r.settings.bitrate == 6000 && r.settings.keyint_sec == 2);

	/* AV1: hardware, then SVT-AV1, then H.264 */
	in = bundle("obs_nvenc_av1_tex", 6000, 2, "CBR");
	r = run(&win_intel, &in);
	CHECK_STR(r.settings.encoder, "obs_qsv11_av1");
	r = run(&win_plain, &in);
	CHECK_STR(r.settings.encoder, "ffmpeg_svt_av1");
	CHECK(!r.codec_changed);
	r = run(&mac_vt, &in);
	CHECK_STR(r.settings.encoder, "com.apple.videotoolbox.videoencoder.ave.avc");
	CHECK(r.codec_changed);
}

static void test_settings(void)
{
	/* keyframe automatic (0) becomes 2 s; other values are kept */
	encoder_settings_t in = bundle("obs_nvenc_h264_tex", 9000, 0, "CBR");
	encoder_fallback_result_t r = run(&win_plain, &in);
	CHECK(r.settings.keyint_sec == 2);
	in.keyint_sec = 4;
	r = run(&win_plain, &in);
	CHECK(r.settings.keyint_sec == 4);

	/* quality modes map to the new class's own */
	in = bundle("obs_nvenc_h264_tex", 9000, 2, "CQP");
	r = run(&win_plain, &in);
	CHECK_STR(r.settings.rate_control, "CRF");
	in = bundle("obs_x264", 9000, 2, "CRF");
	r = run(&win_amd, &in);
	CHECK_STR(r.settings.encoder, "obs_x264"); /* x264 is here: no change */
	CHECK(!r.fell_back);
	r = run(&nothing, &in);
	CHECK(!r.available && !r.fell_back);
	CHECK_STR(r.settings.encoder, "obs_x264");
	CHECK(r.warning[0] != 0);

	/* an encoder id we don't know, missing here: H.264 chain */
	in = bundle("vendor_special_enc", 5000, 2, "CBR");
	r = run(&win_amd, &in);
	CHECK_STR(r.settings.encoder, "h264_texture_amf");
	CHECK_STR(r.warning,
		  "The encoder \"vendor_special_enc\" isn't available on this PC; using AMD AMF H.264 instead");

	/* an empty encoder id (a bundle without one) also gets the best H.264 */
	in = bundle("", 5000, 2, "CBR");
	r = run(&mac_vt, &in);
	CHECK_STR(r.settings.encoder, "com.apple.videotoolbox.videoencoder.ave.avc");

	/* NULL availability callback: no decision, input untouched */
	encoder_fallback_result_t r2;
	CHECK(!encoder_fallback(&in, NULL, NULL, &r2));
	CHECK_STR(r2.settings.encoder, "");
}

int main(void)
{
	test_ids();
	test_rate_control();
	test_present();
	test_windows();
	test_macos();
	test_codec();
	test_settings();
	if (failures) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("encoder fallback: all checks passed\n");
	return 0;
}
