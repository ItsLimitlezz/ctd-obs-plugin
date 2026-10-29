#include "delay-queue.h"

#include <libavutil/mem.h>

#include <string.h>

size_t delay_picture_size(const AVFrame *frame)
{
	size_t bytes = 0;
	for (int i = 0; i < AV_NUM_DATA_POINTERS && frame->buf[i]; i++)
		bytes += frame->buf[i]->size;
	return bytes;
}

bool delay_push_picture(struct delay_queue *q, const AVFrame *frame, uint64_t timestamp_ns)
{
	struct delay_picture *p = av_mallocz(sizeof(*p));
	if (!p)
		return false;
	p->frame = av_frame_clone(frame);
	if (!p->frame) {
		av_free(p);
		return false;
	}
	p->timestamp_ns = timestamp_ns;
	p->bytes = delay_picture_size(p->frame);
	if (q->pictures_tail)
		q->pictures_tail->next = p;
	else
		q->pictures = p;
	q->pictures_tail = p;
	q->picture_count++;
	q->picture_bytes += p->bytes;
	return true;
}

bool delay_push_sound(struct delay_queue *q, const float *const planes[2], int frames, uint64_t timestamp_ns)
{
	if (frames <= 0)
		return true;
	struct delay_sound *s = av_mallocz(sizeof(*s));
	if (!s)
		return false;
	for (int c = 0; c < 2; c++) {
		s->planes[c] = av_malloc((size_t)frames * sizeof(float));
		if (!s->planes[c]) {
			delay_sound_free(s);
			return false;
		}
		memcpy(s->planes[c], planes[c], (size_t)frames * sizeof(float));
	}
	s->frames = frames;
	s->timestamp_ns = timestamp_ns;
	if (q->sounds_tail)
		q->sounds_tail->next = s;
	else
		q->sounds = s;
	q->sounds_tail = s;
	q->sound_bytes += (size_t)frames * 2 * sizeof(float);
	return true;
}

struct delay_picture *delay_pop_picture(struct delay_queue *q, uint64_t due_ns)
{
	struct delay_picture *p = q->pictures;
	if (!p || p->timestamp_ns > due_ns)
		return NULL;
	q->pictures = p->next;
	if (!q->pictures)
		q->pictures_tail = NULL;
	q->picture_count--;
	q->picture_bytes -= p->bytes;
	p->next = NULL;
	return p;
}

struct delay_sound *delay_pop_sound(struct delay_queue *q, uint64_t due_ns)
{
	struct delay_sound *s = q->sounds;
	if (!s || s->timestamp_ns > due_ns)
		return NULL;
	q->sounds = s->next;
	if (!q->sounds)
		q->sounds_tail = NULL;
	q->sound_bytes -= (size_t)s->frames * 2 * sizeof(float);
	s->next = NULL;
	return s;
}

void delay_picture_free(struct delay_picture *p)
{
	if (!p)
		return;
	av_frame_free(&p->frame);
	av_free(p);
}

void delay_sound_free(struct delay_sound *s)
{
	if (!s)
		return;
	av_free(s->planes[0]);
	av_free(s->planes[1]);
	av_free(s);
}

bool delay_queue_empty(const struct delay_queue *q)
{
	return !q->pictures && !q->sounds;
}

void delay_queue_clear(struct delay_queue *q)
{
	while (q->pictures) {
		struct delay_picture *p = q->pictures;
		q->pictures = p->next;
		delay_picture_free(p);
	}
	while (q->sounds) {
		struct delay_sound *s = q->sounds;
		q->sounds = s->next;
		delay_sound_free(s);
	}
	memset(q, 0, sizeof(*q));
}
