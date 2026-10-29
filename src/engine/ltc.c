/*
 * LTC decoder and encoder. See ltc.h.
 */

#include "ltc.h"

#include <math.h>
#include <string.h>

#define SYNC_WORD 0x3FFDu /* bits 64 to 79, in the order they arrive */
#define MIN_LEVEL 0.002f  /* below about -54 dBFS there is no LTC to read */

/* ---------------------------------------------------------------- decoder -- */

/* bit i (0 to 79) of the word that just ended */
static int word_bit(const struct ltc_decoder *d, int i)
{
	const int position = 79 - i;
	return position < 64 ? (int)((d->low >> position) & 1) : (int)((d->high >> (position - 64)) & 1);
}

static int field(const struct ltc_decoder *d, int first, int count)
{
	int value = 0;
	for (int k = 0; k < count; k++)
		value |= word_bit(d, first + k) << k;
	return value;
}

/* the word that just ended, if its digits make sense */
static bool parse_word(const struct ltc_decoder *d, struct ltc_word *w)
{
	const int frame_units = field(d, 0, 4), second_units = field(d, 16, 4);
	const int minute_units = field(d, 32, 4), hour_units = field(d, 48, 4);
	if (frame_units > 9 || second_units > 9 || minute_units > 9 || hour_units > 9)
		return false;
	w->frames = frame_units + 10 * field(d, 8, 2);
	w->seconds = second_units + 10 * field(d, 24, 3);
	w->minutes = minute_units + 10 * field(d, 40, 3);
	w->hours = hour_units + 10 * field(d, 56, 2);
	if (w->frames >= 30 || w->seconds >= 60 || w->minutes >= 60 || w->hours >= 24)
		return false;
	w->drop_frame = word_bit(d, 10) != 0;
	static const int user_starts[8] = {4, 12, 20, 28, 36, 44, 52, 60};
	w->user_bits = 0;
	for (int g = 0; g < 8; g++)
		w->user_bits |= (uint32_t)field(d, user_starts[g], 4) << (4 * g);
	return true;
}

static void lose_sync(struct ltc_decoder *d)
{
	d->bits = 0;
	d->half_pending = false;
}

static void adapt(struct ltc_decoder *d, double bit, int sample_rate)
{
	d->bit_length += 0.02 * (bit - d->bit_length);
	const double shortest = sample_rate / (80.0 * 31.0), longest = sample_rate / (80.0 * 23.0);
	if (d->bit_length < shortest)
		d->bit_length = shortest;
	if (d->bit_length > longest)
		d->bit_length = longest;
}

/* one bit in; true when it completed a word */
static bool push_bit(struct ltc_decoder *d, int bit, struct ltc_word *w)
{
	d->high = (d->high << 1) | (d->low >> 63);
	d->low = (d->low << 1) | (uint64_t)bit;
	d->bits++;
	if (d->bits < 80 || (d->low & 0xFFFFu) != SYNC_WORD)
		return false;
	d->bits = 0;
	if (!parse_word(d, w)) {
		d->errors++;
		return false;
	}
	return true;
}

/* a level flip at time t (samples since the decoder started); true when it completed a word */
static bool edge(struct ltc_decoder *d, double t, int sample_rate, struct ltc_word *w)
{
	if (d->last_edge < 0) {
		d->last_edge = t;
		return false;
	}
	const double gap = t - d->last_edge;
	d->last_edge = t;
	const double bit = d->bit_length;
	if (gap < 0.3 * bit) {
		/* too short for half a bit: noise */
		d->errors++;
		lose_sync(d);
		return false;
	}
	if (gap < 0.75 * bit) {
		adapt(d, 2.0 * gap, sample_rate);
		if (!d->half_pending) {
			d->half_pending = true;
			return false;
		}
		d->half_pending = false;
		return push_bit(d, 1, w);
	}
	if (gap < 1.4 * bit) {
		adapt(d, gap, sample_rate);
		if (d->half_pending) {
			/* half a 1 and then a whole bit: out of step; the next 0 puts it right */
			d->errors++;
			lose_sync(d);
			return false;
		}
		return push_bit(d, 0, w);
	}
	/* too long: the signal stopped for a moment */
	lose_sync(d);
	return false;
}

int ltc_decode(struct ltc_decoder *d, const float *samples, int count, int sample_rate, struct ltc_word *out,
	       int max_words)
{
	if (sample_rate <= 0)
		return 0;
	if (d->bit_length <= 0) {
		d->bit_length = sample_rate / (80.0 * 27.0); /* the middle of 23.976 to 30 fps */
		d->last_edge = -1.0;
	}
	/* the envelope falls by half in about 20 ms */
	const float decay = 1.0f - (float)(0.7 / (0.02 * sample_rate));
	int words = 0;
	for (int i = 0; i < count; i++) {
		const float x = samples[i];
		const float magnitude = fabsf(x);
		d->envelope = magnitude > d->envelope ? magnitude : d->envelope * decay;
		if (magnitude > d->level_peak)
			d->level_peak = magnitude;
		const double position = (double)(d->position + i);
		/* where it crossed zero, to a fraction of a sample */
		if ((d->previous < 0.0f) != (x < 0.0f) && d->previous != x)
			d->crossing = position - 1.0 + (double)(d->previous / (d->previous - x));
		d->previous = x;
		if (d->envelope < MIN_LEVEL) {
			d->state = 0;
			d->last_edge = -1.0;
			lose_sync(d);
			continue;
		}
		const float threshold = 0.25f * (float)d->envelope;
		const int state = x > threshold ? 1 : x < -threshold ? -1 : d->state;
		if (state != d->state && state != 0) {
			struct ltc_word w;
			if (d->state != 0 && edge(d, d->crossing, sample_rate, &w) && words < max_words) {
				w.end_offset = d->crossing - (double)d->position;
				out[words++] = w;
			}
			d->state = state;
		}
	}
	d->position += count;
	return words;
}

double ltc_take_level(struct ltc_decoder *d)
{
	const double db = d->level_peak > 1e-6f ? 20.0 * log10((double)d->level_peak) : -120.0;
	d->level_peak = 0.0f;
	return db;
}

double ltc_bit_rate_fps(const struct ltc_decoder *d, int sample_rate)
{
	return d->bit_length > 0 && sample_rate > 0 ? sample_rate / (80.0 * d->bit_length) : 0.0;
}

void ltc_decoder_reset(struct ltc_decoder *d)
{
	memset(d, 0, sizeof(*d));
}

/* ------------------------------------------------------------ frame index -- */

int64_t ltc_frame_index(int hours, int minutes, int seconds, int frames, int base, bool drop)
{
	const int64_t total_minutes = 60 * (int64_t)hours + minutes;
	int64_t index = (total_minutes * 60 + seconds) * base + frames;
	if (drop) /* frames 0 and 1 are skipped each minute, except every tenth */
		index -= 2 * (total_minutes - total_minutes / 10);
	return index;
}

void ltc_from_index(int64_t index, int base, bool drop, int *hours, int *minutes, int *seconds, int *frames)
{
	if (drop) {
		const int64_t tens = index / 17982, rest = index % 17982;
		index += 18 * tens + 2 * ((rest < 2 ? 0 : rest - 2) / 1798);
	}
	*frames = (int)(index % base);
	index /= base;
	*seconds = (int)(index % 60);
	index /= 60;
	*minutes = (int)(index % 60);
	*hours = (int)(index / 60 % 24);
}

/* ------------------------------------------------------------------- rate -- */

static int nearest_base(double fps)
{
	if (fps <= 0)
		return 0;
	return fps < 24.5 ? 24 : fps < 27.5 ? 25 : 30;
}

void ltc_rate_add(struct ltc_rate *r, const struct ltc_word *w, int64_t ns, double bit_rate_fps)
{
	if (w->frames > r->max_frame)
		r->max_frame = w->frames;
	int base;
	if (w->drop_frame)
		base = 30;
	else if (r->max_frame >= 25)
		base = 30;
	else if (r->max_frame == 24)
		base = 25;
	else if (r->seen_words > 30)
		base = 24; /* over a second of words and never past 23 */
	else
		base = nearest_base(bit_rate_fps);
	if (!base)
		return;
	if (base != r->base || w->drop_frame != r->drop) {
		/* a new rate: a new run */
		r->base = base;
		r->drop = w->drop_frame;
		r->fractional = w->drop_frame;
		r->seen_words = 0;
	}

	const int64_t index = ltc_frame_index(w->hours, w->minutes, w->seconds, w->frames, base, w->drop_frame);
	if (r->seen_words > 0 && index > r->last_index && index - r->last_index <= base) {
		r->missing += index - r->last_index - 1;
		r->expected += index - r->last_index;
		r->last_index = index;
		r->last_ns = ns;
		r->seen_words++;
	} else {
		/* the first word, or the timecode jumped: a new run */
		r->first_index = r->last_index = index;
		r->first_ns = r->last_ns = ns;
		r->seen_words = 1;
		r->expected++;
	}
	/* 23.976 and 29.97 non-drop: words come 0.1% slower than the base; five seconds of stream time tells */
	if (!r->drop && base != 25 && r->last_ns - r->first_ns >= 5000000000ll) {
		const double measured =
			(double)(r->last_index - r->first_index) * 1e9 / (double)(r->last_ns - r->first_ns);
		r->fractional = measured < base * (1.0 - 0.0005);
	}
}

double ltc_rate_fps(const struct ltc_rate *r)
{
	if (!r->base)
		return 0.0;
	return r->fractional ? r->base / 1.001 : (double)r->base;
}

double ltc_rate_error_percent(const struct ltc_rate *r)
{
	return r->expected > 0 ? (double)r->missing * 100.0 / (double)r->expected : 0.0;
}

/* ---------------------------------------------------------------- encoder -- */

static void set_field(uint8_t *word, int first, int count, int value)
{
	for (int k = 0; k < count; k++)
		word[first + k] = (uint8_t)((value >> k) & 1);
}

static void build_word(struct ltc_encoder *enc)
{
	int h, m, s, f;
	ltc_from_index(enc->index, enc->base, enc->drop, &h, &m, &s, &f);
	uint8_t *word = enc->word;
	memset(word, 0, 80);
	set_field(word, 0, 4, f % 10);
	set_field(word, 8, 2, f / 10);
	word[10] = enc->drop;
	set_field(word, 16, 4, s % 10);
	set_field(word, 24, 3, s / 10);
	set_field(word, 32, 4, m % 10);
	set_field(word, 40, 3, m / 10);
	set_field(word, 48, 4, h % 10);
	set_field(word, 56, 2, h / 10);
	static const int user_starts[8] = {4, 12, 20, 28, 36, 44, 52, 60};
	for (int g = 0; g < 8; g++)
		set_field(word, user_starts[g], 4, (int)((enc->user_bits >> (4 * g)) & 0xF));
	/* the sync word, bit 64 first */
	for (int k = 0; k < 16; k++)
		word[64 + k] = (uint8_t)((SYNC_WORD >> (15 - k)) & 1);
	enc->word_index = enc->index;
}

void ltc_encoder_init(struct ltc_encoder *enc, double fps, bool drop, int64_t start_index)
{
	memset(enc, 0, sizeof(*enc));
	enc->fps = fps;
	enc->base = fps > 29.0 ? 30 : fps > 24.5 ? 25 : 24;
	enc->drop = drop && enc->base == 30;
	enc->index = start_index;
	enc->level = 1.0f;
	enc->word_index = -1;
}

void ltc_encode(struct ltc_encoder *enc, float *samples, int count, int sample_rate, float amplitude)
{
	const double step = 80.0 * enc->fps / sample_rate; /* bits a sample: about 0.04 to 0.05 */
	for (int i = 0; i < count; i++) {
		if (enc->word_index != enc->index)
			build_word(enc);
		samples[i] = enc->level * amplitude;
		const int bit = (int)enc->phase;
		double next = enc->phase + step;
		if (!enc->mid_flipped && enc->word[bit] && next >= bit + 0.5) {
			enc->level = -enc->level; /* a 1 flips in the middle */
			enc->mid_flipped = true;
		}
		if (next >= bit + 1.0) {
			enc->level = -enc->level; /* every bit starts with a flip */
			enc->mid_flipped = false;
			if (bit + 1 >= 80) {
				next -= 80.0;
				enc->index++;
			}
		}
		enc->phase = next;
	}
}
