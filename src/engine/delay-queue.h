/*
 * The delay queue (SPEC S2): decoded pictures and audio held inside the plugin until just before they are due, so a
 * delay never depends on OBS's own async frame queue (a few frames, reset on large timestamp gaps). Pictures are held
 * as references, audio as copies; both in arrival order, which is timestamp order. Only the worker thread touches it.
 */

#pragma once

#include <libavutil/frame.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct delay_picture {
	AVFrame *frame;
	uint64_t timestamp_ns;
	size_t bytes;
	struct delay_picture *next;
};

struct delay_sound {
	float *planes[2];
	int frames;
	uint64_t timestamp_ns;
	struct delay_sound *next;
};

struct delay_queue {
	struct delay_picture *pictures, *pictures_tail;
	struct delay_sound *sounds, *sounds_tail;
	size_t picture_count;
	size_t picture_bytes; /* what the held pictures take, for the memory cap */
	size_t sound_bytes;
};

/* the memory a picture holds (its buffers) */
size_t delay_picture_size(const AVFrame *frame);

/* Adds a reference to frame. False if out of memory. */
bool delay_push_picture(struct delay_queue *q, const AVFrame *frame, uint64_t timestamp_ns);
/* Copies the samples. False if out of memory. */
bool delay_push_sound(struct delay_queue *q, const float *const planes[2], int frames, uint64_t timestamp_ns);

/* The oldest picture or sound if it is due at or before due_ns, taken off the queue; NULL otherwise. */
struct delay_picture *delay_pop_picture(struct delay_queue *q, uint64_t due_ns);
struct delay_sound *delay_pop_sound(struct delay_queue *q, uint64_t due_ns);
void delay_picture_free(struct delay_picture *p);
void delay_sound_free(struct delay_sound *s);

bool delay_queue_empty(const struct delay_queue *q);
/* Drops everything held. */
void delay_queue_clear(struct delay_queue *q);
