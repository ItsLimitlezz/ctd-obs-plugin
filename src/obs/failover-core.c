/* Auto-failover on dead feeds: the rules (see failover-core.h). Pure C, no libobs. */
#include "failover-core.h"

#include <stdio.h>
#include <string.h>

static void copy_name(char *dst, const char *src)
{
	snprintf(dst, FO_NAME_MAX, "%s", src ? src : "");
}

static bool same(const char *a, const char *b)
{
	return strcmp(a ? a : "", b ? b : "") == 0;
}

static int clamp(int value, int low, int high)
{
	return value < low ? low : value > high ? high : value;
}

/* ------------------------------------------------------ picture matching -- */

int fo_grid_distance(const uint8_t a[FO_GRID_BYTES], const uint8_t b[FO_GRID_BYTES])
{
	long sum = 0;
	for (int i = 0; i < FO_GRID_BYTES; i++)
		sum += a[i] > b[i] ? a[i] - b[i] : b[i] - a[i];
	return (int)((sum + FO_GRID_BYTES / 2) / FO_GRID_BYTES);
}

bool fo_grid_matches(const uint8_t a[FO_GRID_BYTES], const uint8_t b[FO_GRID_BYTES], int tolerance)
{
	return fo_grid_distance(a, b) <= (tolerance < 0 ? 0 : tolerance);
}

/* The rows that are bars in every common pattern (SMPTE's lower third and the HD pattern's lower 5/12 are not). */
#define BARS_ROW_FIRST 1
#define BARS_ROW_LAST 7
/* The columns between the HD pattern's grey pillars. */
#define BARS_COL_FIRST 2
#define BARS_COL_LAST 13

bool fo_grid_is_bars(const uint8_t grid[FO_GRID_BYTES])
{
	int column[FO_GRID];
	int ragged = 0;
	for (int x = 0; x < FO_GRID; x++) {
		int sum = 0, low = 255, high = 0;
		for (int y = BARS_ROW_FIRST; y <= BARS_ROW_LAST; y++) {
			const int v = grid[y * FO_GRID + x];
			sum += v;
			low = v < low ? v : low;
			high = v > high ? v : high;
		}
		column[x] = sum / (BARS_ROW_LAST - BARS_ROW_FIRST + 1);
		/* stripes are vertical: a column is one brightness all the way down (but for coding noise) */
		if (high - low > 12)
			ragged++;
	}
	if (ragged > 1)
		return false;
	int steps = 0, flats = 0;
	for (int x = BARS_COL_FIRST; x < BARS_COL_LAST; x++) {
		const int drop = column[x] - column[x + 1];
		if (drop < -3)
			return false; /* brighter to the right: not bars */
		if (drop >= 8)
			steps++;
		else if (drop <= 3)
			flats++;
	}
	/* a staircase, not a ramp: several clear steps, flat in between, and a long way down overall */
	return steps >= 4 && flats >= 3 && column[BARS_COL_FIRST] - column[BARS_COL_LAST] >= 60;
}

void fo_grid_to_hex(const uint8_t grid[FO_GRID_BYTES], char out[FO_GRID_HEX])
{
	static const char digits[] = "0123456789abcdef";
	for (int i = 0; i < FO_GRID_BYTES; i++) {
		out[i * 2] = digits[grid[i] >> 4];
		out[i * 2 + 1] = digits[grid[i] & 15];
	}
	out[FO_GRID_BYTES * 2] = '\0';
}

static int hex_value(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

bool fo_grid_from_hex(const char *hex, uint8_t out[FO_GRID_BYTES])
{
	if (!hex || strlen(hex) != (size_t)FO_GRID_BYTES * 2)
		return false;
	for (int i = 0; i < FO_GRID_BYTES; i++) {
		const int high = hex_value(hex[i * 2]);
		const int low = hex_value(hex[i * 2 + 1]);
		if (high < 0 || low < 0)
			return false;
		out[i] = (uint8_t)(high * 16 + low);
	}
	return true;
}

/* --------------------------------------------------------------- config -- */

void fo_rule_defaults(struct fo_rule *r, const char *source)
{
	memset(r, 0, sizeof(*r));
	copy_name(r->source, source);
	r->watch = true;
	r->on_signal_lost = true;
	r->on_bars = true;
}

void fo_config_defaults(struct fo_config *c)
{
	memset(c, 0, sizeof(*c));
	c->delay_ms = FO_DELAY_DEFAULT_MS;
	c->frozen_ms = FO_FROZEN_DEFAULT_MS;
	c->tolerance = FO_TOLERANCE_DEFAULT;
	c->return_after_ms = FO_RETURN_DEFAULT_MS;
	c->watch_all = true;
}

void fo_config_sanitize(struct fo_config *c)
{
	c->delay_ms = clamp(c->delay_ms, FO_DELAY_MIN_MS, FO_DELAY_MAX_MS);
	c->frozen_ms = clamp(c->frozen_ms, 1000, 600000);
	c->tolerance = clamp(c->tolerance, 0, 64);
	c->return_after_ms = clamp(c->return_after_ms, 0, 600000);
	if (c->scene_count > FO_MAX_SCENES)
		c->scene_count = FO_MAX_SCENES;
	if (c->rule_count > FO_MAX_RULES)
		c->rule_count = FO_MAX_RULES;
	/* the fallback list: no empty names, each scene once */
	size_t kept = 0;
	for (size_t i = 0; i < c->scene_count; i++) {
		c->scenes[i][FO_NAME_MAX - 1] = '\0';
		bool duplicate = !*c->scenes[i];
		for (size_t k = 0; k < kept && !duplicate; k++)
			duplicate = same(c->scenes[k], c->scenes[i]);
		if (duplicate)
			continue;
		if (kept != i)
			memcpy(c->scenes[kept], c->scenes[i], FO_NAME_MAX);
		kept++;
	}
	c->scene_count = kept;
	/* rules: one per source, none without a name */
	kept = 0;
	for (size_t i = 0; i < c->rule_count; i++) {
		struct fo_rule *r = &c->rules[i];
		r->source[FO_NAME_MAX - 1] = '\0';
		r->target[FO_NAME_MAX - 1] = '\0';
		bool duplicate = !*r->source;
		for (size_t k = 0; k < kept && !duplicate; k++)
			duplicate = same(c->rules[k].source, r->source);
		if (duplicate)
			continue;
		if (kept != i)
			c->rules[kept] = *r;
		kept++;
	}
	c->rule_count = kept;
}

bool fo_rule_for(const struct fo_config *c, const char *source, struct fo_rule *out)
{
	for (size_t i = 0; i < c->rule_count; i++) {
		if (same(c->rules[i].source, source)) {
			*out = c->rules[i];
			return out->watch;
		}
	}
	if (!c->watch_all)
		return false;
	fo_rule_defaults(out, source);
	return true;
}

/* ---------------------------------------------------------------- health -- */

const char *fo_reason_name(enum fo_reason reason)
{
	switch (reason) {
	case FO_SIGNAL_LOST:
		return "signal-lost";
	case FO_BARS:
		return "color-bars";
	case FO_IMAGE_MATCH:
		return "image-match";
	case FO_FROZEN:
		return "frozen-picture";
	case FO_OK:
		break;
	}
	return "";
}

enum fo_reason fo_classify(const struct fo_config *c, const struct fo_sample *s)
{
	struct fo_rule rule;
	if (!s || !s->source || !fo_rule_for(c, s->source, &rule))
		return FO_OK;
	if (!s->has_signal)
		return rule.on_signal_lost ? FO_SIGNAL_LOST : FO_OK; /* its last picture says nothing now */
	if (s->grid) {
		if (rule.has_reference && fo_grid_matches(s->grid, rule.reference, c->tolerance))
			return FO_IMAGE_MATCH;
		if (rule.on_bars && fo_grid_is_bars(s->grid))
			return FO_BARS;
	}
	if (rule.on_frozen && s->frozen_ms >= c->frozen_ms)
		return FO_FROZEN;
	return FO_OK;
}

void fo_state_init(struct fo_state *st)
{
	memset(st, 0, sizeof(*st));
}

void fo_state_quiet(struct fo_state *st, uint64_t now_ms, uint64_t grace_ms)
{
	st->quiet_until_ms = now_ms + grace_ms;
	st->active = false;
	st->overridden[0] = '\0';
}

static struct fo_source_state *find_source(struct fo_state *st, const char *source)
{
	for (size_t i = 0; i < st->count; i++) {
		if (same(st->sources[i].source, source))
			return &st->sources[i];
	}
	return NULL;
}

static const struct fo_source_state *find_source_const(const struct fo_state *st, const char *source)
{
	for (size_t i = 0; i < st->count; i++) {
		if (same(st->sources[i].source, source))
			return &st->sources[i];
	}
	return NULL;
}

void fo_observe(struct fo_state *st, const struct fo_config *c, const struct fo_sample *samples, size_t count,
		uint64_t now_ms)
{
	/* 0 means "not set" in the timers below */
	const uint64_t now = now_ms ? now_ms : 1;
	for (size_t i = 0; i < st->count; i++)
		st->sources[i].seen = false;
	for (size_t i = 0; i < count; i++) {
		const struct fo_sample *s = &samples[i];
		if (!s->source || !*s->source)
			continue;
		struct fo_source_state *src = find_source(st, s->source);
		if (!src) {
			if (st->count >= FO_MAX_SOURCES)
				continue;
			src = &st->sources[st->count++];
			memset(src, 0, sizeof(*src));
			copy_name(src->source, s->source);
		}
		src->seen = true;
		src->reason = fo_classify(c, s);
		if (src->reason == FO_OK) {
			src->down_since_ms = 0;
			if (!src->ok_since_ms)
				src->ok_since_ms = now;
		} else {
			src->ok_since_ms = 0;
			if (!src->down_since_ms)
				src->down_since_ms = now;
		}
	}
	/* a source that is gone (removed, renamed, disconnected on purpose) is nobody's problem */
	size_t kept = 0;
	for (size_t i = 0; i < st->count; i++) {
		if (!st->sources[i].seen)
			continue;
		if (kept != i)
			st->sources[kept] = st->sources[i];
		kept++;
	}
	st->count = kept;
}

enum fo_reason fo_source_reason(const struct fo_state *st, const char *source)
{
	const struct fo_source_state *src = find_source_const(st, source);
	return src ? src->reason : FO_OK;
}

bool fo_source_is_down(const struct fo_state *st, const struct fo_config *c, const char *source, uint64_t now_ms)
{
	const struct fo_source_state *src = find_source_const(st, source);
	if (!src || src->reason == FO_OK || !src->down_since_ms)
		return false;
	return now_ms >= src->down_since_ms && now_ms - src->down_since_ms >= (uint64_t)c->delay_ms;
}

/* -------------------------------------------------------------- deciding -- */

const char *fo_decision_mode(const struct fo_decision *d)
{
	if (d->action == FO_RETURN)
		return "return";
	return d->by_target ? "target" : "list";
}

static const struct fo_scene *find_scene(const struct fo_world *w, const char *name)
{
	if (!name || !*name)
		return NULL;
	for (size_t i = 0; i < w->scene_count; i++) {
		if (same(w->scenes[i].name, name))
			return &w->scenes[i];
	}
	return NULL;
}

/* no watched source visible in it is down right now (no delay: going there has to be safe this instant) */
static bool scene_healthy(const struct fo_state *st, const struct fo_scene *scene)
{
	for (size_t i = 0; i < scene->source_count; i++) {
		if (fo_source_reason(st, scene->sources[i]) != FO_OK)
			return false;
	}
	return true;
}

/* the watched source on this scene that has been down longest (past the delay), or NULL */
static const struct fo_source_state *down_on(const struct fo_state *st, const struct fo_config *c,
					     const struct fo_scene *scene, uint64_t now_ms)
{
	const struct fo_source_state *worst = NULL;
	for (size_t i = 0; i < scene->source_count; i++) {
		if (!fo_source_is_down(st, c, scene->sources[i], now_ms))
			continue;
		const struct fo_source_state *src = find_source_const(st, scene->sources[i]);
		if (!worst || src->down_since_ms < worst->down_since_ms)
			worst = src;
	}
	return worst;
}

/* The program scene changed without the plugin: the operator did it. */
static void operator_moved(struct fo_state *st, const char *program)
{
	if (st->active) {
		/* back to the scene the plugin left, its source still down: they want it; stand down for it */
		if (same(program, st->from) && fo_source_reason(st, st->source) != FO_OK)
			copy_name(st->overridden, program);
		/* anywhere else than where the plugin went: the show moved on, there is nothing to go back to */
		if (!same(program, st->to))
			st->active = false;
	}
	if (*st->overridden && !same(program, st->overridden))
		st->overridden[0] = '\0';
	copy_name(st->program, program);
}

void fo_decide(struct fo_state *st, const struct fo_config *c, const struct fo_world *w, uint64_t now_ms,
	       struct fo_decision *out)
{
	memset(out, 0, sizeof(*out));
	out->action = FO_NONE;
	const char *program = w->program ? w->program : "";
	if (!*program)
		return;
	const bool cooling = st->switched_at_ms && now_ms - st->switched_at_ms < FO_COOLDOWN_MS;
	if (cooling && same(program, st->left))
		return; /* the plugin's own switch hasn't reached program yet */
	if (!same(program, st->program))
		operator_moved(st, program);
	if (!c->enabled) {
		st->active = false;
		st->overridden[0] = '\0';
		return;
	}
	if (now_ms < st->quiet_until_ms)
		return;
	if (cooling)
		return;
	const struct fo_scene *on_air = find_scene(w, program);
	if (!on_air)
		return;

	const struct fo_source_state *dead = down_on(st, c, on_air, now_ms);
	if (!dead) {
		if (*st->overridden && scene_healthy(st, on_air))
			st->overridden[0] = '\0'; /* it recovered: the plugin watches this scene again */
		if (!st->active || !c->return_when_back || !same(program, st->to))
			return;
		/* going back: the source that went down has been healthy long enough, and so is the whole scene */
		const struct fo_source_state *trigger = find_source_const(st, st->source);
		const struct fo_scene *origin = find_scene(w, st->from);
		if (!origin) {
			st->active = false; /* the scene is gone */
			return;
		}
		if (!trigger || trigger->reason != FO_OK || !trigger->ok_since_ms ||
		    now_ms - trigger->ok_since_ms < (uint64_t)c->return_after_ms || !scene_healthy(st, origin))
			return;
		out->action = FO_RETURN;
		copy_name(out->from, program);
		copy_name(out->to, st->from);
		copy_name(out->source, st->source);
		out->reason = FO_OK;
		return;
	}
	if (same(program, st->overridden))
		return; /* the operator put this scene back on air knowing its feed is down */

	/* where to: the source's own scene if it is fit to show, else the first fallback scene that is */
	struct fo_rule rule;
	const struct fo_scene *to = NULL;
	bool by_target = false;
	if (fo_rule_for(c, dead->source, &rule) && *rule.target && !same(rule.target, program)) {
		const struct fo_scene *target = find_scene(w, rule.target);
		if (target && scene_healthy(st, target)) {
			to = target;
			by_target = true;
		}
	}
	for (size_t i = 0; i < c->scene_count && !to; i++) {
		if (same(c->scenes[i], program))
			continue;
		const struct fo_scene *candidate = find_scene(w, c->scenes[i]);
		if (candidate && scene_healthy(st, candidate))
			to = candidate;
	}
	if (!to)
		return; /* nowhere better to go: stay, and look again next tick */
	out->action = FO_SWITCH;
	copy_name(out->from, program);
	copy_name(out->to, to->name);
	copy_name(out->source, dead->source);
	out->reason = dead->reason;
	out->by_target = by_target;
}

void fo_committed(struct fo_state *st, const struct fo_decision *d, uint64_t now_ms)
{
	if (d->action == FO_NONE)
		return;
	st->switched_at_ms = now_ms ? now_ms : 1;
	copy_name(st->left, d->from);
	if (d->action == FO_RETURN) {
		st->active = false;
	} else {
		/* a second hop (the fallback died too) still goes back to where the show was */
		if (!st->active)
			copy_name(st->from, d->from);
		st->active = true;
		copy_name(st->to, d->to);
		copy_name(st->source, d->source);
	}
	st->overridden[0] = '\0';
	copy_name(st->program, d->to); /* this change is the plugin's own, not the operator's */
}
