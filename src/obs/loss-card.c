/*
 * The picture shown while a feed is lost. See loss-card.h.
 */

#include "loss-card.h"

#include <obs-module.h>
#include <util/bmem.h>

#include <ctype.h>
#include <string.h>

#define GLYPH_W 5
#define GLYPH_H 7
#define ADVANCE 6 /* a glyph plus one column of spacing */

struct glyph {
	char c;
	uint8_t rows[GLYPH_H]; /* bit 4 is the leftmost pixel */
};

/* A 5x7 pixel font: capitals, digits and the punctuation a source name is likely to use. '*' is a middle dot. */
static const struct glyph font[] = {
	{' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}}, {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
	{'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}}, {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
	{'D', {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}}, {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
	{'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}}, {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}},
	{'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}}, {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},
	{'J', {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}}, {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
	{'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}}, {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},
	{'N', {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}}, {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
	{'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}}, {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}},
	{'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}}, {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
	{'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}}, {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
	{'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}}, {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}},
	{'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}}, {'Y', {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}},
	{'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}}, {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
	{'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}}, {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
	{'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}}, {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
	{'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}}, {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
	{'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}}, {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
	{'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}}, {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}},
	{',', {0x00, 0x00, 0x00, 0x00, 0x0C, 0x04, 0x08}}, {'*', {0x00, 0x00, 0x00, 0x0C, 0x0C, 0x00, 0x00}},
	{'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}}, {'_', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F}},
	{':', {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}}, {'/', {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10}},
	{'!', {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04}}, {'?', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04}},
	{'(', {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}}, {')', {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}},
	{'#', {0x0A, 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x0A}}, {'+', {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00}},
	{'&', {0x0C, 0x12, 0x14, 0x08, 0x15, 0x12, 0x0D}}, {'\'', {0x04, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00}},
	{'=', {0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00}},
};

/* BGRA, the CTD brand's Night theme */
static const uint8_t background[4] = {0x0C, 0x07, 0x04, 0xFF};    /* void #04070C */
static const uint8_t grid_color[4] = {0x21, 0x18, 0x0B, 0xFF};    /* grid-line (surface-raised) #0B1821 */
static const uint8_t title_color[4] = {0xFF, 0xFB, 0xF2, 0xFF};   /* ink #F2FBFF */
static const uint8_t name_color[4] = {0xAF, 0xA3, 0x8B, 0xFF};    /* ink-muted #8BA3AF */
static const uint8_t accent_color[4] = {0xFF, 0xE7, 0x39, 0xFF};  /* cyan #39E7FF */
static const uint8_t magenta_color[4] = {0xB8, 0x00, 0xFF, 0xFF}; /* magenta #FF00B8 */
static const uint8_t bar_color[4] = {0x00, 0x00, 0x00, 0xFF};     /* around a picture of another shape */

static const uint8_t *find_glyph(char c)
{
	const char upper = (char)toupper((unsigned char)c);
	for (size_t i = 0; i < sizeof(font) / sizeof(*font); i++) {
		if (font[i].c == upper)
			return font[i].rows;
	}
	return NULL;
}

/* The text as the font can draw it: capitals, one '?' per character it has no glyph for (UTF-8 aware). */
static char *drawable(const char *text, size_t max_chars)
{
	const size_t len = text ? strlen(text) : 0;
	char *out = bzalloc(len + 1);
	size_t n = 0;
	for (size_t i = 0; i < len && n < max_chars; i++) {
		const unsigned char c = (unsigned char)text[i];
		if ((c & 0xC0) == 0x80)
			continue; /* UTF-8 continuation byte: its lead byte already became '?' */
		out[n++] = c < 0x80 && find_glyph((char)c) ? (char)c : '?';
	}
	return out;
}

static void fill(struct loss_card *card, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t color[4])
{
	if (x >= card->width || y >= card->height)
		return;
	if (w > card->width - x)
		w = card->width - x;
	if (h > card->height - y)
		h = card->height - y;
	for (uint32_t row = y; row < y + h; row++) {
		uint8_t *p = card->pixels + ((size_t)row * card->width + x) * 4;
		for (uint32_t col = 0; col < w; col++, p += 4)
			memcpy(p, color, 4);
	}
}

static uint32_t text_units(const char *text)
{
	const size_t len = strlen(text);
	return len ? (uint32_t)(len * ADVANCE - 1) : 0;
}

/* Draws text centred horizontally with its top at y, each font pixel scale x scale screen pixels. */
static void draw_text(struct loss_card *card, const char *text, uint32_t y, uint32_t scale, const uint8_t color[4])
{
	const uint32_t width = text_units(text) * scale;
	uint32_t x = width < card->width ? (card->width - width) / 2 : 0;
	for (const char *c = text; *c; c++, x += ADVANCE * scale) {
		const uint8_t *rows = find_glyph(*c);
		if (!rows)
			continue;
		for (uint32_t r = 0; r < GLYPH_H; r++) {
			for (uint32_t col = 0; col < GLYPH_W; col++) {
				if (rows[r] & (0x10 >> col))
					fill(card, x + col * scale, y + r * scale, scale, scale, color);
			}
		}
	}
}

/* the brand's stage grid: faint lines like a monitor's safe-area overlay */
static void draw_grid(struct loss_card *card)
{
	const uint32_t cell = card->height / 12 ? card->height / 12 : 1;
	const uint32_t thickness = card->height / 1080 ? card->height / 1080 : 1;
	for (uint32_t x = cell; x < card->width; x += cell)
		fill(card, x, 0, thickness, card->height, grid_color);
	for (uint32_t y = cell; y < card->height; y += cell)
		fill(card, 0, y, card->width, thickness, grid_color);
}

/* the stripe motif (a wide cyan bar, a thin ink bar, a wide magenta bar, skewed 20 degrees), bleeding off the
 * bottom-right corner: once per view */
static void draw_stripe(struct loss_card *card)
{
	const uint32_t height = card->height / 4;
	const uint32_t wide = card->height / 18 ? card->height / 18 : 2;
	const uint32_t thin = wide / 6 ? wide / 6 : 1;
	const uint32_t gap = thin;
	const uint32_t start = card->width > card->width / 7 + 3 * wide ? card->width - card->width / 7 - 2 * wide : 0;
	const struct {
		uint32_t offset, width;
		const uint8_t *color;
	} bars[] = {{0, wide, accent_color},
		    {wide + gap, thin, title_color},
		    {wide + gap + thin + gap, wide, magenta_color}};
	for (uint32_t row = 0; row < height; row++) {
		const uint32_t y = card->height - 1 - row;
		const uint32_t shift = row * 364 / 1000; /* tan(20 degrees) */
		for (size_t b = 0; b < sizeof(bars) / sizeof(*bars); b++)
			fill(card, start + bars[b].offset + shift, y, bars[b].width, 1, bars[b].color);
	}
}

static void draw_builtin(struct loss_card *card, const char *title_text, const char *name_text)
{
	fill(card, 0, 0, card->width, card->height, background);
	draw_grid(card);
	draw_stripe(card);

	char *title = drawable(title_text, 64);
	const uint32_t units = text_units(title);
	/* the title takes about 85% of the width, or a ninth of the height on narrow (vertical) pictures */
	uint32_t scale = units ? (card->width * 85 / 100) / units : 1;
	if (scale > card->height / 100)
		scale = card->height / 100;
	if (!scale)
		scale = 1;

	uint32_t name_scale = scale * 3 / 5;
	if (!name_scale)
		name_scale = 1;
	char *name = drawable(name_text, (card->width * 85 / 100) / (ADVANCE * name_scale));

	/* the eyebrow above the title, cyan, as on every CTD section */
	uint32_t eyebrow_scale = scale * 2 / 5;
	if (!eyebrow_scale)
		eyebrow_scale = 1;
	const char *eyebrow = "CTD // LIVE SOURCE";

	const uint32_t gap = scale * 3;
	const uint32_t bar_h = scale / 2 ? scale / 2 : 1;
	const uint32_t name_h = *name ? gap + GLYPH_H * name_scale : 0;
	const uint32_t eyebrow_h = GLYPH_H * eyebrow_scale + gap;
	const uint32_t total = eyebrow_h + GLYPH_H * scale + gap + bar_h + name_h;
	const uint32_t top = total < card->height ? (card->height - total) / 2 + eyebrow_h : eyebrow_h;

	draw_text(card, eyebrow, top - eyebrow_h, eyebrow_scale, accent_color);
	draw_text(card, title, top, scale, title_color);
	const uint32_t bar_w = units * scale / 6;
	fill(card, (card->width - bar_w) / 2, top + GLYPH_H * scale + gap, bar_w, bar_h, accent_color);
	if (*name)
		draw_text(card, name, top + GLYPH_H * scale + gap + bar_h + gap, name_scale, name_color);

	bfree(title);
	bfree(name);
}

/* Scales a decoded image onto the card (bilinear), keeping its shape. Returns false if it can't be read. */
static bool draw_image(struct loss_card *card, const char *path)
{
	enum gs_color_format format = GS_UNKNOWN;
	uint32_t iw = 0, ih = 0;
	uint8_t *image = gs_create_texture_file_data(path, &format, &iw, &ih);
	const bool rgba = format == GS_RGBA;
	if (!image || !iw || !ih || (format != GS_RGBA && format != GS_BGRA && format != GS_BGRX)) {
		bfree(image);
		return false;
	}

	const struct loss_rect fit = loss_card_fit(card->width, card->height, iw, ih);
	fill(card, 0, 0, card->width, card->height, bar_color);
	for (uint32_t y = 0; y < fit.height; y++) {
		const float sy = ((float)y + 0.5f) * (float)ih / (float)fit.height - 0.5f;
		const uint32_t y0 = sy <= 0.0f ? 0 : (uint32_t)sy;
		const uint32_t y1 = y0 + 1 < ih ? y0 + 1 : ih - 1;
		const float fy = sy <= 0.0f ? 0.0f : sy - (float)y0;
		for (uint32_t x = 0; x < fit.width; x++) {
			const float sx = ((float)x + 0.5f) * (float)iw / (float)fit.width - 0.5f;
			const uint32_t x0 = sx <= 0.0f ? 0 : (uint32_t)sx;
			const uint32_t x1 = x0 + 1 < iw ? x0 + 1 : iw - 1;
			const float fx = sx <= 0.0f ? 0.0f : sx - (float)x0;
			const uint8_t *p00 = image + ((size_t)y0 * iw + x0) * 4,
				      *p01 = image + ((size_t)y0 * iw + x1) * 4;
			const uint8_t *p10 = image + ((size_t)y1 * iw + x0) * 4,
				      *p11 = image + ((size_t)y1 * iw + x1) * 4;
			uint8_t *out = card->pixels + ((size_t)(fit.y + y) * card->width + fit.x + x) * 4;
			for (int c = 0; c < 4; c++) {
				const float top = (float)p00[c] + ((float)p01[c] - (float)p00[c]) * fx;
				const float bottom = (float)p10[c] + ((float)p11[c] - (float)p10[c]) * fx;
				/* RGBA images swap red and blue into BGRA */
				const int dst = rgba && c != 3 ? 2 - c : c;
				out[dst] = (uint8_t)(top + (bottom - top) * fy + 0.5f);
			}
			if (format == GS_BGRX)
				out[3] = 0xFF;
		}
	}
	bfree(image);
	return true;
}

void loss_card_build(struct loss_card *card, uint32_t width, uint32_t height, const char *title, const char *name,
		     const char *image_path)
{
	loss_card_free(card);
	if (!width || !height)
		return;
	card->width = width;
	card->height = height;
	card->pixels = bmalloc((size_t)width * height * 4);
	if (!image_path || !*image_path || !draw_image(card, image_path))
		draw_builtin(card, title, name);
}

void loss_card_free(struct loss_card *card)
{
	bfree(card->pixels);
	card->pixels = NULL;
	card->width = 0;
	card->height = 0;
}

struct loss_rect loss_card_fit(uint32_t card_width, uint32_t card_height, uint32_t width, uint32_t height)
{
	struct loss_rect fit = {0, 0, card_width, card_height};
	if (!width || !height)
		return fit;
	/* compares the shapes (width / height against the card's) in whole numbers */
	if ((uint64_t)width * card_height > (uint64_t)card_width * height) {
		/* wider than the card: bars above and below */
		fit.height = (uint32_t)(((uint64_t)card_width * height + width / 2) / width);
	} else {
		/* narrower: bars at the sides */
		fit.width = (uint32_t)(((uint64_t)card_height * width + height / 2) / height);
	}
	if (!fit.width)
		fit.width = 1;
	if (!fit.height)
		fit.height = 1;
	fit.x = (card_width - fit.width) / 2;
	fit.y = (card_height - fit.height) / 2;
	return fit;
}
