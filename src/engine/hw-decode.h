/*
 * Hardware video decoding (SPEC R19, R20): d3d12va then dxva2 on Windows, videotoolbox on macOS, vaapi then cuda on
 * Linux.
 *
 * Decoded pictures are copied back to system memory (NV12, or P010 for 10-bit video) and handed to OBS exactly like
 * software-decoded ones, so the timing (R1 to R3) doesn't change. Whatever goes wrong, the caller decodes in software
 * instead: a device that won't open, a codec the GPU can't do, or (via get_format) a stream it turns down.
 */

#pragma once

#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>

#include <stdbool.h>

enum hw_setup {
	HW_READY,      /* the decoder will decode on the GPU */
	HW_NO_DECODER, /* FFmpeg has no GPU decoder for this codec here */
	HW_NO_DEVICE,  /* there is one, but the GPU device wouldn't open */
};

struct hw_decode {
	AVBufferRef *device; /* opened once and kept across reconnects */
	enum AVHWDeviceType type;
	enum AVPixelFormat format; /* the open decoder's pictures in video memory */
	bool refused;              /* the open decoder turned the GPU down for this stream and decodes in software */
	char why[128];             /* what went wrong, for the log */
};

/*
 * Sets dec up to decode on the GPU; call before avcodec_open2. adapter picks the GPU on Windows (OBS's own, as a
 * number; NULL for the default). Anything but HW_READY leaves dec untouched, with the reason in hw->why.
 */
enum hw_setup hw_decode_setup(struct hw_decode *hw, AVCodecContext *dec, const AVCodec *codec, const char *adapter);

/* Whether frame is in video memory and needs hw_decode_download. */
bool hw_decode_on_gpu(const struct hw_decode *hw, const AVFrame *frame);

/* Copies a picture out of video memory into dst, with its timestamps and colour information. */
bool hw_decode_download(AVFrame *dst, const AVFrame *src);

/* "d3d12va", "dxva2", "videotoolbox", "vaapi" or "cuda". */
const char *hw_decode_name(const struct hw_decode *hw);

void hw_decode_free(struct hw_decode *hw);
