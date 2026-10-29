/*
 * The picture a CTD Live source shows while its feed is lost (SPEC R11): a custom image, or a built-in card in the CTD brand
 * (void ground, stage grid, cyan eyebrow, the stripe motif) with a
 * title such as "SIGNAL LOST · RECONNECTING" over the source's name. Drawn on the CPU with a small built-in pixel font, so
 * it needs no fonts and no graphics context. (A video card is played by loss-video.c.)
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

struct loss_card {
	uint8_t *pixels; /* BGRA, width * 4 bytes per row; NULL until built */
	uint32_t width;
	uint32_t height;
};

struct loss_rect {
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
};

/*
 * (Re)builds the card at width x height. image_path (may be NULL or empty) is fitted onto the card (see loss_card_fit);
 * if it doesn't load, the built-in card is drawn with title (ASCII; '*' draws a middle dot) and name (any UTF-8; shown in
 * capitals).
 */
void loss_card_build(struct loss_card *card, uint32_t width, uint32_t height, const char *title, const char *name,
		     const char *image_path);

void loss_card_free(struct loss_card *card);

/*
 * Where a width x height picture goes on a card_width x card_height card: as large as fits, keeping its shape, centred.
 * The rest of the card is black, so a landscape picture on a vertical feed is letterboxed rather than squashed.
 */
struct loss_rect loss_card_fit(uint32_t card_width, uint32_t card_height, uint32_t width, uint32_t height);
