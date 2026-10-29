/*
 * LTC, SMPTE linear timecode (SPEC S4 to S8): a small in-house biphase-mark decoder (and the encoder the test harness
 * uses), on plain float samples at the audio's own rate. No FFmpeg, no libltc.
 *
 * An LTC word is 80 bits, one video frame long: BCD hours, minutes, seconds and frames, the drop-frame and colour flags,
 * 32 user bits, and the sync word 0011 1111 1111 1101 at the end. Biphase mark: the level flips at every bit boundary,
 * and once more in the middle of a 1. The decoder tracks the level with hysteresis, times each flip at its zero
 * crossing (to a fraction of a sample), sorts the gaps into half and whole bits with a bit length it keeps adapting
 * (23.976 to 30 fps all fit its window), and looks for the sync word.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* one decoded word */
struct ltc_word {
	int hours, minutes, seconds, frames;
	bool drop_frame;
	uint32_t user_bits; /* UB1 in the low nibble to UB8 in the high one */
	/* where the sync word ended (S6), in samples from the first sample of the buffer just decoded (it can be slightly
	 * negative: the last edge may sit in the previous buffer) */
	double end_offset;
};

struct ltc_decoder {
	double envelope;    /* peak level, slowly decaying */
	int state;          /* the level after hysteresis: +1, -1, or 0 before any */
	float previous;     /* the previous sample */
	double crossing;    /* the latest zero crossing, in samples since the decoder started */
	double last_edge;   /* the previous level flip, same scale; < 0 before one */
	double bit_length;  /* samples per bit, adapting; 0 until the first buffer */
	bool half_pending;  /* the first half of a 1 has been seen */
	uint64_t low, high; /* the last 128 bits, newest in bit 0 of low */
	int bits;           /* bits since the last word (or since an error) */
	int64_t position;   /* samples consumed */
	float level_peak;   /* the peak since ltc_take_level() */
	long errors;        /* edges that fit no bit (noise, dropouts) */
};

/* Decodes count samples at sample_rate. Writes up to max_words words to out and returns how many. */
int ltc_decode(struct ltc_decoder *d, const float *samples, int count, int sample_rate, struct ltc_word *out,
	       int max_words);
/* The LTC signal's peak since the last call, in dBFS (-120 for none), and starts a new measurement. */
double ltc_take_level(struct ltc_decoder *d);
/* frames per second the bit length suggests (0 until it has locked) */
double ltc_bit_rate_fps(const struct ltc_decoder *d, int sample_rate);
void ltc_decoder_reset(struct ltc_decoder *d);

/* A frame count since midnight, for comparing timecodes; drop-frame counting when drop is set (base 30). */
int64_t ltc_frame_index(int hours, int minutes, int seconds, int frames, int base, bool drop);
/* The timecode at a frame index (the inverse) */
void ltc_from_index(int64_t index, int base, bool drop, int *hours, int *minutes, int *seconds, int *frames);

/*
 * The rate tracker: which of 23.976, 24, 25, 29.97 (drop or not) and 30 fps a run of words is. The base (24, 25, 30)
 * comes from the highest frame number seen; the drop flag means 29.97; 23.976 and 29.97 non-drop show as words
 * arriving 0.1% slower than the base over a few seconds of stream time. It also counts missing words, for the error
 * rate: within a run of consecutive timecodes, every index skipped is a word that didn't decode.
 */
struct ltc_rate {
	int base;            /* 24, 25 or 30; 0 until known */
	bool drop;           /* drop frame (29.97) */
	bool fractional;     /* /1.001 */
	int max_frame;       /* highest frame number seen this second-and-a-bit */
	int64_t seen_words;  /* words in the current run */
	int64_t first_index; /* the run's first and last frame index */
	int64_t last_index;
	int64_t first_ns; /* the stream time of the run's first and last word */
	int64_t last_ns;
	int64_t missing;  /* words skipped within runs, since the tracker started */
	int64_t expected; /* words the runs should have held */
};

/* Adds a word pinned at stream time ns; bit_rate_fps is the decoder's estimate, for the base before a full second. */
void ltc_rate_add(struct ltc_rate *r, const struct ltc_word *w, int64_t ns, double bit_rate_fps);
/* the detected rate in frames per second (e.g. 29.97), or 0 */
double ltc_rate_fps(const struct ltc_rate *r);
/* words that didn't decode, as a percentage of those expected */
double ltc_rate_error_percent(const struct ltc_rate *r);

/* The encoder (for the harness): fills count samples of LTC at amplitude, continuing from *phase (bits into the
 * current word, starts at 0) and *index (the frame index, advanced as words complete), with *level carrying the
 * signal level between calls (start it at 1). */
struct ltc_encoder {
	double fps;    /* 23.976, 24, 25, 29.97 or 30 */
	bool drop;     /* drop-frame numbering (29.97 only) */
	int base;      /* 24, 25 or 30 */
	int64_t index; /* the frame being sent */
	double phase;  /* bits into it */
	float level;   /* +1 or -1 */
	bool mid_flipped;
	uint32_t user_bits;
	uint8_t word[80]; /* the bits of word_index */
	int64_t word_index;
};
void ltc_encoder_init(struct ltc_encoder *enc, double fps, bool drop, int64_t start_index);
void ltc_encode(struct ltc_encoder *enc, float *samples, int count, int sample_rate, float amplitude);
