/*
 * The reconnecting card as a looping video. See loss-video.h.
 */

#include "loss-video.h"
#include "loss-card.h"

#include <util/bmem.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>

#include <string.h>

#define NS_PER_S 1000000000ll
/* A frame later than this (a busy machine, or the source was off screen) is shown at once, and the video carries on from
 * there instead of racing to catch up. */
#define MAX_LATE_NS (NS_PER_S / 2)
/* reads without a picture before the file counts as unplayable */
#define MAX_READS_PER_FRAME 2000

/* One open file. Made by loss_video_play(), then owned by the player thread. */
struct playback {
	char *path;
	char *source_name; /* for the log */
	AVFormatContext *format;
	AVCodecContext *decoder;
	AVPacket *packet;
	AVFrame *frame;
	int stream;
	AVRational time_base;
	int64_t frame_ns;          /* one frame's duration, from the frame rate */
	unsigned frames_this_pass; /* a whole pass through the file without a picture means it can't be played */

	/* the card, BGRA: the video is scaled into fit, black around it */
	uint32_t width;
	uint32_t height;
	uint8_t *pixels;
	struct SwsContext *scaler;
	int scaler_width; /* the decoded frames the scaler is set up for */
	int scaler_height;
	int scaler_format;
	struct loss_rect fit;

	/* pacing: a frame is due at base_ns + its timestamp */
	int64_t base_ns;
	int64_t due_ns;     /* the current frame's */
	int64_t pts_ns;     /* the current frame's timestamp */
	int64_t restart_ns; /* when not 0, the next frame is due then, whatever its timestamp: the file started over */
	bool ready;         /* a frame is decoded and scaled, waiting for its time */
};

struct loss_video {
	obs_source_t *source;
	pthread_t thread;
	bool thread_running;
	os_event_t *wake;  /* auto-reset: play, stop or exit */
	char *failed_path; /* the last file that couldn't be played, so it is logged once; used by the playing thread */

	pthread_mutex_t mutex;    /* guards the fields below, and is held for every frame handed to OBS */
	struct playback *pending; /* opened by loss_video_play(), not yet picked up by the player thread */
	bool playing;
	bool exiting;
};

/* ------------------------------------------------------------------ file -- */

static void playback_close(struct playback *pb)
{
	if (!pb)
		return;
	sws_freeContext(pb->scaler);
	avcodec_free_context(&pb->decoder);
	avformat_close_input(&pb->format);
	av_packet_free(&pb->packet);
	av_frame_free(&pb->frame);
	bfree(pb->pixels);
	bfree(pb->path);
	bfree(pb->source_name);
	bfree(pb);
}

static struct playback *playback_open(const char *path, uint32_t width, uint32_t height, const char **why)
{
	struct playback *pb = bzalloc(sizeof(*pb));
	const AVCodec *codec = NULL;
	AVStream *stream = NULL;
	AVRational rate;
	pb->path = bstrdup(path);
	pb->stream = -1;
	pb->scaler_format = AV_PIX_FMT_NONE;
	pb->width = width;
	pb->height = height;

	*why = "can't open it";
	if (avformat_open_input(&pb->format, path, NULL, NULL) < 0)
		goto fail;
	*why = "can't read it";
	if (avformat_find_stream_info(pb->format, NULL) < 0)
		goto fail;
	*why = "it has no video FFmpeg can decode";
	pb->stream = av_find_best_stream(pb->format, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
	if (pb->stream < 0 || !codec)
		goto fail;
	/* only the picture is read: the sound stays off (R12) */
	for (unsigned i = 0; i < pb->format->nb_streams; i++) {
		if ((int)i != pb->stream)
			pb->format->streams[i]->discard = AVDISCARD_ALL;
	}

	stream = pb->format->streams[pb->stream];
	pb->decoder = avcodec_alloc_context3(codec);
	if (!pb->decoder || avcodec_parameters_to_context(pb->decoder, stream->codecpar) < 0 ||
	    avcodec_open2(pb->decoder, codec, NULL) < 0)
		goto fail;
	pb->time_base = stream->time_base;
	rate = stream->avg_frame_rate;
	if (rate.num <= 0 || rate.den <= 0)
		rate = stream->r_frame_rate;
	pb->frame_ns = rate.num > 0 && rate.den > 0 ? av_rescale(NS_PER_S, rate.den, rate.num) : NS_PER_S / 30;
	if (pb->frame_ns < NS_PER_S / 240 || pb->frame_ns > NS_PER_S)
		pb->frame_ns = NS_PER_S / 30;

	*why = "out of memory";
	pb->packet = av_packet_alloc();
	pb->frame = av_frame_alloc();
	if (!pb->packet || !pb->frame)
		goto fail;
	pb->pixels = bmalloc((size_t)width * height * 4);
	return pb;

fail:
	playback_close(pb);
	return NULL;
}

/* Decodes the next frame into pb->frame, starting over at the end of the file. False once the file stops giving
 * pictures. */
static bool decode_next(struct playback *pb)
{
	for (int reads = 0; reads < MAX_READS_PER_FRAME; reads++) {
		const int err = avcodec_receive_frame(pb->decoder, pb->frame);
		if (err == 0) {
			pb->frames_this_pass++;
			return true;
		}
		if (err == AVERROR_EOF) {
			/* the whole file has played: back to the start */
			if (!pb->frames_this_pass)
				return false;
			pb->frames_this_pass = 0;
			avcodec_flush_buffers(pb->decoder);
			const AVStream *stream = pb->format->streams[pb->stream];
			const int64_t start = stream->start_time != AV_NOPTS_VALUE ? stream->start_time : 0;
			if (av_seek_frame(pb->format, pb->stream, start, AVSEEK_FLAG_BACKWARD) < 0)
				return false;
			pb->restart_ns = pb->due_ns + pb->frame_ns;
			continue;
		}
		if (err != AVERROR(EAGAIN))
			return false;

		/* the decoder wants more of the file */
		if (av_read_frame(pb->format, pb->packet) < 0) {
			/* the end of the file (or a damaged tail): the decoder hands over what it still holds, then EOF */
			avcodec_send_packet(pb->decoder, NULL);
			continue;
		}
		if (pb->packet->stream_index == pb->stream)
			avcodec_send_packet(pb->decoder, pb->packet); /* a damaged packet is just skipped */
		av_packet_unref(pb->packet);
	}
	return false;
}

/* Works out when the decoded frame is due on the clock. */
static void schedule(struct playback *pb, int64_t now)
{
	const int64_t ts = pb->frame->best_effort_timestamp;
	const int64_t pts_ns = ts != AV_NOPTS_VALUE ? av_rescale_q(ts, pb->time_base, (AVRational){1, 1000000000})
						    : pb->pts_ns + pb->frame_ns;
	int64_t due;
	if (pb->restart_ns) {
		due = pb->restart_ns;
		pb->restart_ns = 0;
	} else {
		due = pb->base_ns + pts_ns;
		/* the file's timestamps jumped: carry on at the frame rate */
		if (due < pb->due_ns || due > pb->due_ns + NS_PER_S)
			due = pb->due_ns + pb->frame_ns;
	}
	if (due < now - MAX_LATE_NS)
		due = now;
	pb->base_ns = due - pts_ns;
	pb->due_ns = due;
	pb->pts_ns = pts_ns;
}

static bool full_range(const AVFrame *frame)
{
	return frame->color_range == AVCOL_RANGE_JPEG || frame->format == AV_PIX_FMT_YUVJ420P ||
	       frame->format == AV_PIX_FMT_YUVJ422P || frame->format == AV_PIX_FMT_YUVJ444P;
}

/* Scales the decoded frame onto the card. */
static bool render(struct playback *pb)
{
	const AVFrame *frame = pb->frame;
	if (frame->width <= 0 || frame->height <= 0)
		return false;

	if (frame->width != pb->scaler_width || frame->height != pb->scaler_height ||
	    frame->format != pb->scaler_format) {
		/* the first frame, or the video changed size: fit it again and redraw the bars */
		int64_t shown_width = frame->width;
		if (frame->sample_aspect_ratio.num > 0 && frame->sample_aspect_ratio.den > 0)
			shown_width = av_rescale(frame->width, frame->sample_aspect_ratio.num,
						 frame->sample_aspect_ratio.den);
		pb->fit = loss_card_fit(pb->width, pb->height, (uint32_t)shown_width, (uint32_t)frame->height);
		pb->fit.x &= ~3u; /* a 16-byte aligned start keeps the scaler on its fast path */
		pb->scaler = sws_getCachedContext(pb->scaler, frame->width, frame->height,
						  (enum AVPixelFormat)frame->format, (int)pb->fit.width,
						  (int)pb->fit.height, AV_PIX_FMT_BGRA, SWS_BILINEAR, NULL, NULL, NULL);
		if (!pb->scaler)
			return false;
		/* untagged video: HD sizes are BT.709, smaller ones BT.601 */
		int colorspace = frame->colorspace;
		if (colorspace == AVCOL_SPC_UNSPECIFIED)
			colorspace = frame->height >= 720 ? SWS_CS_ITU709 : SWS_CS_ITU601;
		sws_setColorspaceDetails(pb->scaler, sws_getCoefficients(colorspace), full_range(frame),
					 sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);

		for (size_t i = 0; i < (size_t)pb->width * pb->height; i++) {
			uint8_t *p = pb->pixels + i * 4;
			p[0] = p[1] = p[2] = 0;
			p[3] = 0xFF;
		}
		pb->scaler_width = frame->width;
		pb->scaler_height = frame->height;
		pb->scaler_format = frame->format;
	}

	uint8_t *dst[4] = {pb->pixels + ((size_t)pb->fit.y * pb->width + pb->fit.x) * 4, NULL, NULL, NULL};
	const int dst_linesize[4] = {(int)pb->width * 4, 0, 0, 0};
	return sws_scale(pb->scaler, (const uint8_t *const *)frame->data, frame->linesize, 0, frame->height, dst,
			 dst_linesize) > 0;
}

/* -------------------------------------------------------------- player --- */

/* Hands the card to OBS. The caller holds the mutex and has checked that playback is still wanted. */
static void output(struct loss_video *video, const struct playback *pb)
{
	struct obs_source_frame frame = {0};
	frame.format = VIDEO_FORMAT_BGRA;
	frame.width = pb->width;
	frame.height = pb->height;
	frame.data[0] = pb->pixels;
	frame.linesize[0] = pb->width * 4;
	frame.full_range = true;
	frame.timestamp = os_gettime_ns();
	obs_source_output_video(video->source, &frame);
}

static void *player_thread(void *opaque)
{
	struct loss_video *video = opaque;
	os_set_thread_name("ctd-live-card-video");
	struct playback *pb = NULL;

	for (;;) {
		pthread_mutex_lock(&video->mutex);
		struct playback *next = video->pending;
		video->pending = NULL;
		const bool playing = video->playing;
		const bool exiting = video->exiting;
		pthread_mutex_unlock(&video->mutex);

		if (next || !playing || exiting) {
			playback_close(pb);
			pb = next;
		}
		if (exiting) {
			playback_close(pb);
			break;
		}
		if (!pb) {
			os_event_wait(video->wake);
			continue;
		}
		if (!obs_source_showing(video->source)) {
			/* off screen: nothing to decode for. A frame held back meanwhile is shown late, which re-paces. */
			os_event_timedwait(video->wake, 100);
			continue;
		}

		if (!pb->ready) {
			if (!decode_next(pb) || !render(pb)) {
				blog(LOG_WARNING,
				     "[ctd-live:%s] the card video %s stopped: no more pictures could be decoded",
				     pb->source_name, pb->path);
				playback_close(pb);
				pb = NULL;
				continue; /* its last frame stays up */
			}
			schedule(pb, (int64_t)os_gettime_ns());
			pb->ready = true;
		}

		const int64_t wait_ns = pb->due_ns - (int64_t)os_gettime_ns();
		if (wait_ns > 0 && os_event_timedwait(video->wake, (unsigned long)((wait_ns + 999999) / 1000000)) == 0)
			continue; /* woken: play, stop or exit */

		pthread_mutex_lock(&video->mutex);
		if (video->playing && !video->pending && !video->exiting)
			output(video, pb);
		pthread_mutex_unlock(&video->mutex);
		pb->ready = false;
	}
	return NULL;
}

/* ---------------------------------------------------------------- API --- */

struct loss_video *loss_video_create(obs_source_t *source)
{
	struct loss_video *video = bzalloc(sizeof(*video));
	video->source = source;
	pthread_mutex_init(&video->mutex, NULL);
	os_event_init(&video->wake, OS_EVENT_TYPE_AUTO);
	return video;
}

void loss_video_destroy(struct loss_video *video)
{
	if (!video)
		return;
	pthread_mutex_lock(&video->mutex);
	video->exiting = true;
	pthread_mutex_unlock(&video->mutex);
	os_event_signal(video->wake);
	if (video->thread_running)
		pthread_join(video->thread, NULL);
	playback_close(video->pending);
	os_event_destroy(video->wake);
	pthread_mutex_destroy(&video->mutex);
	bfree(video->failed_path);
	bfree(video);
}

bool loss_video_is_video_file(const char *path)
{
	static const char *const extensions[] = {".mp4", ".m4v", ".mov", ".mkv", ".webm"};
	const char *dot = path ? strrchr(path, '.') : NULL;
	if (!dot)
		return false;
	for (size_t i = 0; i < sizeof(extensions) / sizeof(*extensions); i++) {
		if (astrcmpi(dot, extensions[i]) == 0)
			return true;
	}
	return false;
}

bool loss_video_play(struct loss_video *video, const char *path, uint32_t width, uint32_t height,
		     const char *source_name)
{
	const char *why = "the card has no size";
	struct playback *pb = width && height ? playback_open(path, width, height, &why) : NULL;
	if (pb) {
		why = "no picture could be decoded from it";
		if (!decode_next(pb) || !render(pb)) {
			playback_close(pb);
			pb = NULL;
		}
	}
	if (!pb) {
		loss_video_stop(video);
		if (!video->failed_path || strcmp(video->failed_path, path) != 0) {
			blog(LOG_WARNING, "[ctd-live:%s] can't play the card video %s (%s): showing the built-in card",
			     source_name, path, why);
			bfree(video->failed_path);
			video->failed_path = bstrdup(path);
		}
		return false;
	}
	bfree(video->failed_path);
	video->failed_path = NULL;
	pb->source_name = bstrdup(source_name);
	pb->restart_ns = (int64_t)os_gettime_ns();
	schedule(pb, pb->restart_ns);

	if (!video->thread_running && pthread_create(&video->thread, NULL, player_thread, video) == 0)
		video->thread_running = true;

	/* the first frame goes up now; the player thread carries on from the second */
	pthread_mutex_lock(&video->mutex);
	struct playback *older = video->pending; /* one the thread never picked up */
	video->pending = pb;
	video->playing = true;
	output(video, pb);
	pthread_mutex_unlock(&video->mutex);
	playback_close(older);
	os_event_signal(video->wake);
	return true;
}

void loss_video_stop(struct loss_video *video)
{
	if (!video)
		return;
	pthread_mutex_lock(&video->mutex);
	const bool active = video->playing || video->pending;
	struct playback *pending = video->pending;
	video->pending = NULL;
	video->playing = false;
	pthread_mutex_unlock(&video->mutex);
	playback_close(pending);
	if (active)
		os_event_signal(video->wake);
}
