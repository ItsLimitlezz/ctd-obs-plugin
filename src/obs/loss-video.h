/*
 * A video file looped as the reconnecting card (SPEC R11).
 *
 * The worker can't play it itself: while the feed is gone it sits inside FFmpeg's blocking reads and only gets control
 * back every 100 ms or so. So the card video has its own thread. loss_video_play() opens the file and puts its first
 * frame up straight away on the caller's thread, so a file that can't be played is known at once and the caller can fall
 * back to the built-in card. The thread then decodes the rest at the file's own pace, starts again from the top at the
 * end, and fits each frame onto the card keeping the video's shape (black bars where the shapes differ). It decodes only
 * while the source is on screen somewhere. The video's sound is never decoded: audio stays silent while the feed is gone
 * (R12).
 *
 * Every frame goes to OBS under the player's mutex, so once loss_video_stop() returns no card frame can land on top of
 * the live picture.
 */

#pragma once

#include <obs-module.h>

struct loss_video;

struct loss_video *loss_video_create(obs_source_t *source);
void loss_video_destroy(struct loss_video *video);

/* Whether the card file is a video (by its extension) rather than a picture. */
bool loss_video_is_video_file(const char *path);

/*
 * Plays the file from its start, fitted onto a width x height card, until stopped; replaces anything already playing.
 * Returns false if the file can't be played, logging why once per file. source_name prefixes its log lines. Call from
 * one thread at a time.
 */
bool loss_video_play(struct loss_video *video, const char *path, uint32_t width, uint32_t height,
		     const char *source_name);

/* Stops playback (video may be NULL). No frame of the video reaches OBS after this returns. Cheap when idle. */
void loss_video_stop(struct loss_video *video);
