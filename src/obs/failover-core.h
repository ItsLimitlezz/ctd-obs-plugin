/*
 * Auto-failover on dead feeds (CTD OBS F2): the rules behind src/obs/failover.c. Pure C, no libobs, so it is
 * unit-tested on its own (test/failover).
 *
 * A watched source (a CTD Live Source with video) is "down" when it has no signal, shows colour bars, shows a saved
 * picture (the camera's "no input" slate), or has been frozen too long. When a source that is visible on the program
 * scene has been down for the delay, the plugin switches program to another scene: the source's own target scene, or
 * the first scene of the fallback list that shows no down source. Optionally it goes back once the source is healthy.
 *
 * Three parts:
 *   fo_grid_*    picture matching on a 16 x 16 luma grid (the engine's frozen-picture sample points).
 *   fo_observe   per-source timers: since when each source is down, or healthy.
 *   fo_decide    what to do about the program scene right now. The caller makes the switch and reports it with
 *                fo_committed; a program scene that changes any other way is the operator's doing.
 *
 * The operator always wins: cutting back to the scene the plugin just left (its source still down) stands the
 * plugin down for that scene until the source recovers, and moving anywhere else cancels a pending return.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FO_NAME_MAX 160 /* a scene or source name, with its terminating NUL (longer names are cut) */
#define FO_MAX_SCENES 32 /* fallback list entries */
#define FO_MAX_RULES 48  /* sources with their own settings */
#define FO_MAX_SOURCES 64 /* watched sources tracked at once */
#define FO_GRID 16
#define FO_GRID_BYTES (FO_GRID * FO_GRID)
#define FO_GRID_HEX (FO_GRID_BYTES * 2 + 1)

#define FO_DELAY_DEFAULT_MS 2000
#define FO_DELAY_MIN_MS 0
#define FO_DELAY_MAX_MS 60000
#define FO_FROZEN_DEFAULT_MS 10000
#define FO_TOLERANCE_DEFAULT 6 /* mean absolute luma difference (0 to 255) that still counts as the same picture */
#define FO_RETURN_DEFAULT_MS 5000
#define FO_COOLDOWN_MS 3000 /* after a switch, how long before the next one (a transition is still running) */

/* ------------------------------------------------------ picture matching -- */

/* Mean absolute difference of two grids: 0 (the same picture) to 255. */
int fo_grid_distance(const uint8_t a[FO_GRID_BYTES], const uint8_t b[FO_GRID_BYTES]);
bool fo_grid_matches(const uint8_t a[FO_GRID_BYTES], const uint8_t b[FO_GRID_BYTES], int tolerance);
/* Colour bars (SMPTE, EBU, HD bars with grey pillars), from luma alone: vertical stripes that step down in
 * brightness from left to right over most of the width, identical row after row in the upper part of the picture. */
bool fo_grid_is_bars(const uint8_t grid[FO_GRID_BYTES]);
/* For the settings file: 512 lowercase hex digits and a NUL. */
void fo_grid_to_hex(const uint8_t grid[FO_GRID_BYTES], char out[FO_GRID_HEX]);
bool fo_grid_from_hex(const char *hex, uint8_t out[FO_GRID_BYTES]);

/* --------------------------------------------------------------- config -- */

struct fo_rule {
	char source[FO_NAME_MAX];
	bool watch;          /* false: never switch because of this source */
	bool on_signal_lost; /* no signal (NO_SIGNAL, reconnecting, never connected) */
	bool on_bars;        /* colour bars */
	bool on_frozen;      /* the picture hasn't changed for fo_config.frozen_ms */
	bool has_reference;  /* a saved picture to recognise */
	uint8_t reference[FO_GRID_BYTES];
	char target[FO_NAME_MAX]; /* the scene to go to; "" = the fallback list */
};

struct fo_config {
	bool enabled;
	int delay_ms;  /* how long a source must be down before program moves */
	int frozen_ms; /* for rules with on_frozen */
	int tolerance; /* for saved pictures */
	bool return_when_back;
	int return_after_ms; /* healthy this long before going back */
	bool watch_all;      /* sources without a rule are watched with the defaults (signal lost, colour bars) */
	size_t scene_count;
	char scenes[FO_MAX_SCENES][FO_NAME_MAX]; /* the fallback list, in order */
	size_t rule_count;
	struct fo_rule rules[FO_MAX_RULES];
};

/* Off, 2 s delay, every source watched for signal loss and colour bars, no fallback scenes yet. */
void fo_config_defaults(struct fo_config *c);
/* Limits put right (delays in range, names terminated, counts capped, duplicate and empty list entries dropped). */
void fo_config_sanitize(struct fo_config *c);
/* A rule with the defaults for a source that has none of its own. */
void fo_rule_defaults(struct fo_rule *r, const char *source);
/* The rule that applies to a source: its own, or the defaults when watch_all; false when it isn't watched. */
bool fo_rule_for(const struct fo_config *c, const char *source, struct fo_rule *out);

/* ---------------------------------------------------------------- health -- */

enum fo_reason {
	FO_OK,
	FO_SIGNAL_LOST,
	FO_BARS,
	FO_IMAGE_MATCH,
	FO_FROZEN,
};

/* "signal-lost", "color-bars", "image-match", "frozen-picture" ("" for FO_OK): the reason sent in obs.failover */
const char *fo_reason_name(enum fo_reason reason);

/* What the plugin saw of one watched source this tick. */
struct fo_sample {
	const char *source;
	bool has_signal;     /* frames are arriving (LIVE, or only just stalled) */
	long frozen_ms;      /* how long the picture has been identical, 0 while it changes */
	const uint8_t *grid; /* its latest picture, or NULL when there is none */
};

/* Why the sample counts as down under the rule that applies to it (FO_OK: it doesn't, or it isn't watched). */
enum fo_reason fo_classify(const struct fo_config *c, const struct fo_sample *s);

struct fo_source_state {
	char source[FO_NAME_MAX];
	enum fo_reason reason;
	uint64_t down_since_ms; /* 0 while healthy */
	uint64_t ok_since_ms;   /* 0 while down */
	bool seen;
};

struct fo_state {
	size_t count;
	struct fo_source_state sources[FO_MAX_SOURCES];
	char program[FO_NAME_MAX]; /* the program scene as last seen */
	/* the switch the plugin made and hasn't undone: for going back, and to recognise the operator's override */
	bool active;
	char from[FO_NAME_MAX];
	char to[FO_NAME_MAX];
	char source[FO_NAME_MAX];
	uint64_t switched_at_ms;
	char left[FO_NAME_MAX]; /* the scene the last switch left: seeing it again during the cooldown is that switch
				   still on its way, not the operator */
	/* the scene the operator cut back to while its source was still down: left alone until it is healthy again */
	char overridden[FO_NAME_MAX];
	uint64_t quiet_until_ms; /* nothing is decided before this (start-up, a new scene collection) */
};

void fo_state_init(struct fo_state *st);
/* Decide nothing until now_ms + grace_ms: sources need a moment to connect after OBS starts. */
void fo_state_quiet(struct fo_state *st, uint64_t now_ms, uint64_t grace_ms);
/* This tick's samples: starts and stops each source's timers. Sources no longer sampled are forgotten. */
void fo_observe(struct fo_state *st, const struct fo_config *c, const struct fo_sample *samples, size_t count,
		uint64_t now_ms);
/* Down right now (no delay), and why. */
enum fo_reason fo_source_reason(const struct fo_state *st, const char *source);
/* Down for at least the delay. */
bool fo_source_is_down(const struct fo_state *st, const struct fo_config *c, const char *source, uint64_t now_ms);

/* -------------------------------------------------------------- deciding -- */

/* A scene and the watched sources visible in it (through nested scenes and groups). */
struct fo_scene {
	const char *name;
	const char *const *sources;
	size_t source_count;
};

struct fo_world {
	const char *program; /* the program scene's name, "" when unknown */
	const struct fo_scene *scenes;
	size_t scene_count;
};

enum fo_action {
	FO_NONE,
	FO_SWITCH, /* away from a dead feed */
	FO_RETURN, /* back to where it was, now that the feed is healthy */
};

struct fo_decision {
	enum fo_action action;
	char from[FO_NAME_MAX];
	char to[FO_NAME_MAX];
	char source[FO_NAME_MAX]; /* the source that went down (or came back) */
	enum fo_reason reason;
	bool by_target; /* the source's own target scene, not the fallback list */
};

/* "target", "list" or "return": the mode sent in obs.failover */
const char *fo_decision_mode(const struct fo_decision *d);

/* What to do now, after fo_observe. FO_NONE most of the time. */
void fo_decide(struct fo_state *st, const struct fo_config *c, const struct fo_world *w, uint64_t now_ms,
	       struct fo_decision *out);
/* The caller made the switch fo_decide asked for. */
void fo_committed(struct fo_state *st, const struct fo_decision *d, uint64_t now_ms);

#ifdef __cplusplus
}
#endif
