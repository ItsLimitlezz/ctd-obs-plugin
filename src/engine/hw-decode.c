/*
 * Hardware video decoding. See hw-decode.h.
 */

#include "hw-decode.h"

#include <libavutil/pixdesc.h>

#include <stdio.h>

/*
 * What each platform tries, in order. Not d3d11va on Windows: copying its pictures back to system memory costs more CPU
 * than decoding in software (4 x 1080p30 on an RTX 3060: 18% of the machine, against 10.5% in software, 5.5% with
 * d3d12va and 5.3% with dxva2).
 */
static const enum AVHWDeviceType device_types[] = {
#if defined(_WIN32)
	AV_HWDEVICE_TYPE_D3D12VA,
	AV_HWDEVICE_TYPE_DXVA2,
#elif defined(__APPLE__)
	AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
#else
	AV_HWDEVICE_TYPE_VAAPI,
	AV_HWDEVICE_TYPE_CUDA,
#endif
};

/* The format the decoder puts pictures in on that kind of device, or AV_PIX_FMT_NONE if it can't use one. */
static enum AVPixelFormat gpu_format(const AVCodec *codec, enum AVHWDeviceType type)
{
	for (int i = 0;; i++) {
		const AVCodecHWConfig *config = avcodec_get_hw_config(codec, i);
		if (!config)
			return AV_PIX_FMT_NONE;
		if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) && config->device_type == type)
			return config->pix_fmt;
	}
}

/* Picks the GPU format when the decoder offers it; otherwise (a profile or size the GPU can't do) software. */
static enum AVPixelFormat get_format(AVCodecContext *dec, const enum AVPixelFormat *formats)
{
	struct hw_decode *hw = dec->opaque;
	for (const enum AVPixelFormat *f = formats; *f != AV_PIX_FMT_NONE; f++) {
		if (*f == hw->format)
			return *f;
	}
	hw->refused = true;
	for (const enum AVPixelFormat *f = formats; *f != AV_PIX_FMT_NONE; f++) {
		const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(*f);
		if (desc && !(desc->flags & AV_PIX_FMT_FLAG_HWACCEL))
			return *f;
	}
	return AV_PIX_FMT_NONE;
}

enum hw_setup hw_decode_setup(struct hw_decode *hw, AVCodecContext *dec, const AVCodec *codec, const char *adapter)
{
	hw->refused = false;
	hw->format = hw->device ? gpu_format(codec, hw->type) : AV_PIX_FMT_NONE;

	const char *failed_type = NULL;
	for (size_t i = 0; i < sizeof(device_types) / sizeof(*device_types) && hw->format == AV_PIX_FMT_NONE; i++) {
		const enum AVHWDeviceType type = device_types[i];
		const enum AVPixelFormat format = gpu_format(codec, type);
		if (format == AV_PIX_FMT_NONE)
			continue;
		AVBufferRef *device = NULL;
		/* d3d12va numbers graphics cards the way OBS does (DXGI); dxva2 has its own numbering, so its default */
#ifdef _WIN32
		const char *name = type == AV_HWDEVICE_TYPE_D3D12VA ? adapter : NULL;
#else
		const char *name = NULL; /* d3d12va (and OBS's adapter number) is Windows only */
		(void)adapter;
#endif
		if (av_hwdevice_ctx_create(&device, type, name, NULL, 0) < 0) {
			failed_type = av_hwdevice_get_type_name(type);
			continue;
		}
		av_buffer_unref(&hw->device);
		hw->device = device;
		hw->type = type;
		hw->format = format;
	}

	if (hw->format == AV_PIX_FMT_NONE) {
		if (failed_type) {
			snprintf(hw->why, sizeof(hw->why), "couldn't open the graphics card for %s", failed_type);
			return HW_NO_DEVICE;
		}
		snprintf(hw->why, sizeof(hw->why), "FFmpeg has no graphics-card decoder for %s here", codec->name);
		return HW_NO_DECODER;
	}

	dec->hw_device_ctx = av_buffer_ref(hw->device);
	if (!dec->hw_device_ctx) {
		snprintf(hw->why, sizeof(hw->why), "out of memory");
		return HW_NO_DEVICE;
	}
	dec->opaque = hw;
	dec->get_format = get_format;
	return HW_READY;
}

bool hw_decode_on_gpu(const struct hw_decode *hw, const AVFrame *frame)
{
	return hw->format != AV_PIX_FMT_NONE && frame->format == hw->format;
}

bool hw_decode_download(AVFrame *dst, const AVFrame *src)
{
	av_frame_unref(dst);
	return av_hwframe_transfer_data(dst, src, 0) == 0 && av_frame_copy_props(dst, src) == 0;
}

const char *hw_decode_name(const struct hw_decode *hw)
{
	const char *name = hw->device ? av_hwdevice_get_type_name(hw->type) : NULL;
	return name ? name : "graphics card";
}

void hw_decode_free(struct hw_decode *hw)
{
	av_buffer_unref(&hw->device);
	hw->format = AV_PIX_FMT_NONE;
}
