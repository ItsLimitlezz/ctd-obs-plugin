/* ctd-failover-tests: the auto-failover rules (src/obs/failover-core.c). No libobs. A tiny fake show (scenes, the
 * sources visible in them, each source's health) is stepped through time; every test reads like a night at the
 * club. Run with ctest (test/failover/CMakeLists.txt). */
#include "failover-core.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                            \
	do {                                                                   \
		if (!(cond)) {                                                 \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                            \
		}                                                              \
	} while (0)

#define CHECK_STR(a, b)                                                                          \
	do {                                                                                     \
		if (strcmp((a), (b)) != 0) {                                                     \
			printf("FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); \
			failures++;                                                              \
		}                                                                                \
	} while (0)

/* ------------------------------------------------------------- pictures -- */

/* a grid from one brightness per column for the bar rows and another set for the rest */
static void stripes(uint8_t grid[FO_GRID_BYTES], const int columns[FO_GRID], int noise)
{
	unsigned seed = 12345;
	for (int y = 0; y < FO_GRID; y++) {
		for (int x = 0; x < FO_GRID; x++) {
			seed = seed * 1103515245u + 12345u;
			const int jitter = noise ? (int)((seed >> 16) % (unsigned)(2 * noise + 1)) - noise : 0;
			int v = columns[x] + jitter;
			if (y >= 11)
				v = 16 + (x * 7 + y * 13) % 60; /* the lower third of SMPTE bars is something else */
			grid[y * FO_GRID + x] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
		}
	}
}

/* 75% bars, limited range: white, yellow, cyan, green, magenta, red, blue across seven equal stripes */
static const int SMPTE_75[FO_GRID] = {180, 180, 162, 162, 162, 131, 131, 112, 112, 84, 84, 65, 65, 65, 35, 35};
/* 100% bars */
static const int EBU_100[FO_GRID] = {235, 235, 210, 210, 210, 170, 170, 145, 145, 106, 106, 81, 81, 81, 41, 41};
/* the HD pattern: 40% grey pillars left and right of narrower bars */
static const int HD_RP219[FO_GRID] = {104, 104, 180, 180, 168, 145, 145, 134, 134, 63, 63, 51, 28, 28, 104, 104};

static void picture(uint8_t grid[FO_GRID_BYTES], unsigned seed)
{
	for (int i = 0; i < FO_GRID_BYTES; i++) {
		seed = seed * 1103515245u + 12345u;
		grid[i] = (uint8_t)(40 + (seed >> 16) % 160);
	}
}

static void test_bars(void)
{
	uint8_t grid[FO_GRID_BYTES];
	stripes(grid, SMPTE_75, 0);
	CHECK(fo_grid_is_bars(grid));
	stripes(grid, SMPTE_75, 3); /* through an encoder */
	CHECK(fo_grid_is_bars(grid));
	stripes(grid, EBU_100, 2);
	CHECK(fo_grid_is_bars(grid));
	stripes(grid, HD_RP219, 2);
	CHECK(fo_grid_is_bars(grid));

	/* not bars: a camera picture, black, a flat colour, a smooth ramp, bars mirrored */
	picture(grid, 7);
	CHECK(!fo_grid_is_bars(grid));
	memset(grid, 16, sizeof(grid));
	CHECK(!fo_grid_is_bars(grid));
	memset(grid, 180, sizeof(grid));
	CHECK(!fo_grid_is_bars(grid));
	int ramp[FO_GRID], mirrored[FO_GRID];
	for (int x = 0; x < FO_GRID; x++) {
		ramp[x] = 220 - x * 12;
		mirrored[x] = SMPTE_75[FO_GRID - 1 - x];
	}
	stripes(grid, ramp, 0);
	CHECK(!fo_grid_is_bars(grid));
	stripes(grid, mirrored, 0);
	CHECK(!fo_grid_is_bars(grid));
	/* stripes with a face in the middle of them: the columns aren't one brightness any more */
	stripes(grid, SMPTE_75, 0);
	for (int y = 2; y < 7; y++)
		for (int x = 5; x < 11; x++)
			grid[y * FO_GRID + x] = (uint8_t)(60 + y * 25);
	CHECK(!fo_grid_is_bars(grid));
}

static void test_saved_picture(void)
{
	uint8_t slate[FO_GRID_BYTES], seen[FO_GRID_BYTES], other[FO_GRID_BYTES];
	picture(slate, 99);
	memcpy(seen, slate, sizeof(seen));
	CHECK(fo_grid_distance(slate, seen) == 0);
	CHECK(fo_grid_matches(slate, seen, 0));
	/* the same slate through an encoder: every point a little off */
	for (int i = 0; i < FO_GRID_BYTES; i++)
		seen[i] = (uint8_t)(slate[i] + (i % 2 ? 4 : -4));
	CHECK(fo_grid_distance(slate, seen) == 4);
	CHECK(fo_grid_matches(slate, seen, FO_TOLERANCE_DEFAULT));
	CHECK(!fo_grid_matches(slate, seen, 3));
	CHECK(fo_grid_matches(slate, seen, -5) == false); /* a negative tolerance is none */
	picture(other, 100);
	CHECK(fo_grid_distance(slate, other) > 30);
	CHECK(!fo_grid_matches(slate, other, FO_TOLERANCE_DEFAULT));

	/* through the settings file and back */
	char hex[FO_GRID_HEX];
	uint8_t back[FO_GRID_BYTES];
	fo_grid_to_hex(slate, hex);
	CHECK(strlen(hex) == 512);
	CHECK(fo_grid_from_hex(hex, back));
	CHECK(memcmp(slate, back, sizeof(back)) == 0);
	CHECK(!fo_grid_from_hex("abc", back));
	CHECK(!fo_grid_from_hex(NULL, back));
	hex[10] = 'x';
	CHECK(!fo_grid_from_hex(hex, back));
}

/* ------------------------------------------------------------- the show -- */

#define SOURCES 4
static const char *const NAMES[SOURCES] = {"Club cam", "DJ cam", "Crowd cam", "Roof cam"};

struct show {
	struct fo_config config;
	struct fo_state state;
	bool signal[SOURCES];
	long frozen[SOURCES];
	const uint8_t *grid[SOURCES];
	bool gone[SOURCES]; /* not sampled at all (removed, or disconnected on purpose) */
	char program[FO_NAME_MAX];
	uint64_t now;
	int switches;
	struct fo_decision last;
};

static struct show show;

/* Scenes: "Club" shows Club cam; "DJ" shows DJ cam; "Wide" shows Crowd cam and Roof cam; "BRB" and "Logo" show no
 * feed at all. */
static const char *const CLUB_SOURCES[] = {"Club cam"};
static const char *const DJ_SOURCES[] = {"DJ cam"};
static const char *const WIDE_SOURCES[] = {"Crowd cam", "Roof cam"};
static const struct fo_scene SCENES[] = {
	{"Club", CLUB_SOURCES, 1}, {"DJ", DJ_SOURCES, 1}, {"Wide", WIDE_SOURCES, 2},
	{"BRB", NULL, 0},          {"Logo", NULL, 0},
};
#define SCENE_COUNT (sizeof(SCENES) / sizeof(SCENES[0]))

static void fallback(const char *a, const char *b, const char *c)
{
	const char *list[] = {a, b, c};
	show.config.scene_count = 0;
	for (int i = 0; i < 3; i++) {
		if (list[i])
			snprintf(show.config.scenes[show.config.scene_count++], FO_NAME_MAX, "%s", list[i]);
	}
}

static struct fo_rule *rule(const char *source)
{
	for (size_t i = 0; i < show.config.rule_count; i++) {
		if (strcmp(show.config.rules[i].source, source) == 0)
			return &show.config.rules[i];
	}
	struct fo_rule *r = &show.config.rules[show.config.rule_count++];
	fo_rule_defaults(r, source);
	return r;
}

static void start(const char *program)
{
	memset(&show, 0, sizeof(show));
	fo_config_defaults(&show.config);
	show.config.enabled = true;
	fo_state_init(&show.state);
	for (int i = 0; i < SOURCES; i++)
		show.signal[i] = true;
	snprintf(show.program, sizeof(show.program), "%s", program);
	show.now = 100000;
	fallback("BRB", NULL, NULL);
}

/* the operator cuts to a scene */
static void cut(const char *scene)
{
	snprintf(show.program, sizeof(show.program), "%s", scene);
}

/* one tick of the plugin, ms later; the switch it asks for happens at once */
static enum fo_action tick(uint64_t ms)
{
	show.now += ms;
	struct fo_sample samples[SOURCES];
	size_t count = 0;
	for (int i = 0; i < SOURCES; i++) {
		if (show.gone[i])
			continue;
		samples[count].source = NAMES[i];
		samples[count].has_signal = show.signal[i];
		samples[count].frozen_ms = show.frozen[i];
		samples[count].grid = show.grid[i];
		count++;
	}
	fo_observe(&show.state, &show.config, samples, count, show.now);
	const struct fo_world world = {show.program, SCENES, SCENE_COUNT};
	struct fo_decision d;
	fo_decide(&show.state, &show.config, &world, show.now, &d);
	if (d.action != FO_NONE) {
		CHECK_STR(d.from, show.program);
		fo_committed(&show.state, &d, show.now);
		snprintf(show.program, sizeof(show.program), "%s", d.to);
		show.switches++;
		show.last = d;
	}
	return d.action;
}

/* ticks every 500 ms for a while; returns how many switches happened */
static int run(uint64_t ms)
{
	const int before = show.switches;
	for (uint64_t t = 0; t < ms; t += 500)
		tick(500);
	return show.switches - before;
}

static void test_dead_feed_on_program(void)
{
	start("Club");
	CHECK(run(10000) == 0);        /* all is well */
	show.signal[0] = false;        /* Club cam drops */
	CHECK(tick(500) == FO_NONE);   /* the plugin sees it */
	CHECK(tick(1500) == FO_NONE);  /* 1.5 s down: a blip so far */
	CHECK(tick(500) == FO_SWITCH); /* 2 s: gone */
	CHECK_STR(show.program, "BRB");
	CHECK_STR(show.last.from, "Club");
	CHECK_STR(show.last.source, "Club cam");
	CHECK(show.last.reason == FO_SIGNAL_LOST);
	CHECK_STR(fo_reason_name(show.last.reason), "signal-lost");
	CHECK_STR(fo_decision_mode(&show.last), "list");
	/* and it stays there: no return unless asked for, no second switch */
	show.signal[0] = true;
	CHECK(run(60000) == 0);
	CHECK_STR(show.program, "BRB");
}

static void test_a_blip_is_not_a_failover(void)
{
	start("Club");
	show.signal[0] = false;
	run(1500);
	show.signal[0] = true;
	CHECK(run(10000) == 0);
	CHECK_STR(show.program, "Club");
	/* the delay counts from the start of each outage, not from the first one */
	show.signal[0] = false;
	CHECK(run(1500) == 0);
}

static void test_dead_feed_off_program_changes_nothing(void)
{
	start("DJ");
	show.signal[0] = false; /* Club cam, not on program */
	CHECK(run(30000) == 0);
	CHECK_STR(show.program, "DJ");
	/* until someone cuts to it: then it is on program and already long dead */
	cut("Club");
	CHECK(tick(500) == FO_SWITCH);
	CHECK_STR(show.program, "BRB");
}

static void test_walks_the_fallback_list(void)
{
	start("Club");
	fallback("DJ", "Wide", "Logo");
	show.signal[0] = false;
	show.signal[1] = false; /* DJ cam is down too: skip "DJ" */
	CHECK(run(2500) == 1);
	CHECK_STR(show.program, "Wide");
	/* then the fallback itself loses a feed: on to the next one that works */
	show.signal[3] = false; /* Roof cam, in "Wide" */
	CHECK(run(6000) == 1);
	CHECK_STR(show.program, "Logo");
	/* nothing left that is better: it stays */
	CHECK(run(20000) == 0);

	/* every fallback is unfit or missing: stay on the dead scene rather than jump somewhere unknown */
	start("Club");
	fallback("DJ", "No such scene", "Club");
	show.signal[0] = false;
	show.signal[1] = false;
	CHECK(run(20000) == 0);
	CHECK_STR(show.program, "Club");
	/* the moment one becomes fit, it goes */
	show.signal[1] = true;
	CHECK(tick(500) == FO_SWITCH);
	CHECK_STR(show.program, "DJ");
}

static void test_a_sources_own_scene(void)
{
	start("Club");
	fallback("BRB", NULL, NULL);
	snprintf(rule("Club cam")->target, FO_NAME_MAX, "%s", "DJ");
	show.signal[0] = false;
	CHECK(run(2500) == 1);
	CHECK_STR(show.program, "DJ");
	CHECK(show.last.by_target);
	CHECK_STR(fo_decision_mode(&show.last), "target");

	/* its own scene is unfit: the list instead */
	start("Club");
	snprintf(rule("Club cam")->target, FO_NAME_MAX, "%s", "DJ");
	show.signal[0] = false;
	show.signal[1] = false;
	CHECK(run(2500) == 1);
	CHECK_STR(show.program, "BRB");
	CHECK(!show.last.by_target);
}

static void test_colour_bars_and_saved_pictures(void)
{
	uint8_t bars[FO_GRID_BYTES], slate[FO_GRID_BYTES], live[FO_GRID_BYTES];
	stripes(bars, SMPTE_75, 2);
	picture(slate, 5);
	picture(live, 6);

	/* the encoder keeps sending, but it is bars: recognised by default */
	start("Club");
	show.grid[0] = live;
	CHECK(run(5000) == 0);
	show.grid[0] = bars;
	CHECK(run(2500) == 1);
	CHECK(show.last.reason == FO_BARS);
	CHECK_STR(fo_reason_name(show.last.reason), "color-bars");

	/* a saved "no input" slate sends that source to its own scene */
	start("Club");
	struct fo_rule *r = rule("Club cam");
	r->has_reference = true;
	memcpy(r->reference, slate, sizeof(slate));
	snprintf(r->target, FO_NAME_MAX, "%s", "Logo");
	show.grid[0] = live;
	CHECK(run(5000) == 0);
	show.grid[0] = slate;
	CHECK(run(2500) == 1);
	CHECK_STR(show.program, "Logo");
	CHECK(show.last.reason == FO_IMAGE_MATCH);
	CHECK_STR(fo_reason_name(show.last.reason), "image-match");

	/* bars switched off for a source that shows bars on purpose */
	start("Club");
	rule("Club cam")->on_bars = false;
	show.grid[0] = bars;
	CHECK(run(20000) == 0);
}

static void test_frozen_only_when_asked(void)
{
	start("Club");
	show.frozen[0] = 60000;
	CHECK(run(20000) == 0); /* a still slide is not a dead feed unless they say so */
	rule("Club cam")->on_frozen = true;
	CHECK(run(2500) == 1);
	CHECK(show.last.reason == FO_FROZEN);
	CHECK_STR(fo_reason_name(show.last.reason), "frozen-picture");
	start("Club");
	rule("Club cam")->on_frozen = true;
	show.frozen[0] = FO_FROZEN_DEFAULT_MS - 1;
	CHECK(run(20000) == 0);
}

static void test_unwatched_sources(void)
{
	start("Club");
	rule("Club cam")->watch = false;
	show.signal[0] = false;
	CHECK(run(20000) == 0);
	/* "only the sources I list" */
	start("DJ");
	show.config.watch_all = false;
	rule("Club cam");
	show.signal[1] = false; /* DJ cam has no rule */
	CHECK(run(20000) == 0);
	cut("Club");
	show.signal[0] = false;
	CHECK(run(2500) == 1);
	/* a source that isn't sampled at all (removed, or disconnected on purpose) is nobody's problem */
	start("Club");
	show.signal[0] = false;
	run(1000);
	show.gone[0] = true;
	CHECK(run(20000) == 0);
	CHECK(fo_source_reason(&show.state, "Club cam") == FO_OK);
}

static void test_going_back(void)
{
	start("Club");
	show.config.return_when_back = true;
	show.signal[0] = false;
	CHECK(run(2500) == 1);
	CHECK_STR(show.program, "BRB");
	show.signal[0] = true;
	CHECK(run(5000) == 0); /* seen healthy for 4.5 s: not 5 s yet */
	CHECK(tick(500) == FO_RETURN);
	CHECK_STR(show.program, "Club");
	CHECK_STR(show.last.from, "BRB");
	CHECK_STR(show.last.source, "Club cam");
	CHECK_STR(fo_decision_mode(&show.last), "return");
	CHECK(run(30000) == 0);

	/* it flaps while away: the 5 s start again each time */
	show.signal[0] = false;
	run(2500);
	CHECK_STR(show.program, "BRB");
	show.signal[0] = true;
	run(3000);
	show.signal[0] = false;
	run(1000);
	show.signal[0] = true;
	CHECK(run(4500) == 0);
	CHECK(run(1000) == 1);
	CHECK_STR(show.program, "Club");

	/* two hops away, then all healthy: straight back to where the show was */
	start("Club");
	show.config.return_when_back = true;
	fallback("Wide", "Logo", NULL);
	show.signal[0] = false;
	run(2500);
	CHECK_STR(show.program, "Wide");
	show.signal[2] = false;
	run(6000);
	CHECK_STR(show.program, "Logo");
	show.signal[2] = true;
	CHECK(run(10000) == 0); /* Club cam is still down */
	show.signal[0] = true;
	CHECK(run(6000) == 1);
	CHECK_STR(show.program, "Club");
}

static void test_the_operator_wins(void)
{
	/* cutting back to the dead scene: they want it; the plugin leaves it alone */
	start("Club");
	show.signal[0] = false;
	run(2500);
	CHECK_STR(show.program, "BRB");
	run(4000);
	cut("Club");
	CHECK(run(60000) == 0);
	CHECK_STR(show.program, "Club");
	/* once that feed has recovered, the scene is watched again */
	show.signal[0] = true;
	run(5000);
	show.signal[0] = false;
	CHECK(run(2500) == 1);
	CHECK_STR(show.program, "BRB");

	/* the operator moves on by hand after a failover: nothing is "returned" behind their back */
	start("Club");
	show.config.return_when_back = true;
	show.signal[0] = false;
	run(2500);
	run(4000);
	cut("DJ");
	show.signal[0] = true;
	CHECK(run(60000) == 0);
	CHECK_STR(show.program, "DJ");

	/* leaving an overridden scene and coming back later is a fresh start */
	start("Club");
	show.signal[0] = false;
	run(2500);
	run(4000);
	cut("Club"); /* override */
	run(5000);
	cut("DJ");
	run(5000);
	cut("Club"); /* back on the dead feed, with no override this time */
	CHECK(run(1000) == 1);
	CHECK_STR(show.program, "BRB");
}

static void test_switch_still_on_its_way(void)
{
	/* OBS takes a moment to make the switch: program still reads the old scene for a tick or two. That is not
	 * the operator cutting back. */
	start("Club");
	show.signal[0] = false;
	run(2000);
	show.now += 500;
	struct fo_sample s = {"Club cam", false, 0, NULL};
	fo_observe(&show.state, &show.config, &s, 1, show.now);
	struct fo_world world = {"Club", SCENES, SCENE_COUNT};
	struct fo_decision d;
	fo_decide(&show.state, &show.config, &world, show.now, &d);
	CHECK(d.action == FO_SWITCH);
	fo_committed(&show.state, &d, show.now);
	for (int i = 0; i < 3; i++) { /* still "Club" on program */
		show.now += 500;
		fo_observe(&show.state, &show.config, &s, 1, show.now);
		fo_decide(&show.state, &show.config, &world, show.now, &d);
		CHECK(d.action == FO_NONE);
	}
	CHECK(show.state.overridden[0] == '\0');
	CHECK(show.state.active);
	world.program = "BRB";
	show.now += 500;
	fo_observe(&show.state, &show.config, &s, 1, show.now);
	fo_decide(&show.state, &show.config, &world, show.now, &d);
	CHECK(d.action == FO_NONE);
	CHECK(show.state.active);
}

static void test_off_quiet_and_limits(void)
{
	/* off is off */
	start("Club");
	show.config.enabled = false;
	show.signal[0] = false;
	CHECK(run(20000) == 0);
	/* switched on with the feed already long dead: it goes at once */
	show.config.enabled = true;
	CHECK(tick(500) == FO_SWITCH);

	/* start-up: sources need a moment to connect */
	start("Club");
	show.signal[0] = false;
	fo_state_quiet(&show.state, show.now, 15000);
	CHECK(run(14500) == 0);
	CHECK(run(1000) == 1);

	/* no delay at all */
	start("Club");
	show.config.delay_ms = 0;
	show.signal[0] = false;
	CHECK(tick(500) == FO_SWITCH);

	/* an unknown program scene, or none: nothing to decide */
	start("Somewhere else");
	show.signal[0] = false;
	CHECK(run(10000) == 0);
	start("");
	show.signal[0] = false;
	CHECK(run(10000) == 0);

	/* never back to back: after a switch, three seconds of nothing */
	start("Club");
	fallback("DJ", "BRB", NULL);
	show.signal[0] = false;
	show.config.delay_ms = 0;
	CHECK(tick(500) == FO_SWITCH);
	show.signal[1] = false;
	CHECK(tick(500) == FO_NONE);
	CHECK(tick(2000) == FO_NONE);
	CHECK(tick(500) == FO_SWITCH);
	CHECK_STR(show.program, "BRB");
}

static void test_config(void)
{
	struct fo_config c;
	fo_config_defaults(&c);
	CHECK(!c.enabled);
	CHECK(c.delay_ms == 2000);
	CHECK(c.watch_all);
	CHECK(!c.return_when_back);
	struct fo_rule r;
	CHECK(fo_rule_for(&c, "Any cam", &r));
	CHECK(r.on_signal_lost && r.on_bars && !r.on_frozen && !r.has_reference && !*r.target);

	c.delay_ms = -5;
	c.frozen_ms = 5;
	c.tolerance = 900;
	c.return_after_ms = -1;
	c.scene_count = 4;
	snprintf(c.scenes[0], FO_NAME_MAX, "BRB");
	snprintf(c.scenes[1], FO_NAME_MAX, "%s", "");
	snprintf(c.scenes[2], FO_NAME_MAX, "BRB");
	snprintf(c.scenes[3], FO_NAME_MAX, "Logo");
	c.rule_count = 3;
	fo_rule_defaults(&c.rules[0], "Club cam");
	fo_rule_defaults(&c.rules[1], "");
	fo_rule_defaults(&c.rules[2], "Club cam");
	c.rules[2].watch = false;
	fo_config_sanitize(&c);
	CHECK(c.delay_ms == 0);
	CHECK(c.frozen_ms == 1000);
	CHECK(c.tolerance == 64);
	CHECK(c.return_after_ms == 0);
	CHECK(c.scene_count == 2);
	CHECK_STR(c.scenes[0], "BRB");
	CHECK_STR(c.scenes[1], "Logo");
	CHECK(c.rule_count == 1);
	CHECK(c.rules[0].watch); /* the first rule for a source wins */
	c.scene_count = 1000;
	c.rule_count = 1000;
	fo_config_sanitize(&c);
	CHECK(c.scene_count <= FO_MAX_SCENES);
	CHECK(c.rule_count <= FO_MAX_RULES);

	/* more sources than the table holds: the extra ones are simply not tracked */
	struct fo_state st;
	fo_state_init(&st);
	fo_config_defaults(&c);
	static char names[FO_MAX_SOURCES + 8][FO_NAME_MAX];
	static struct fo_sample many[FO_MAX_SOURCES + 8];
	for (int i = 0; i < FO_MAX_SOURCES + 8; i++) {
		snprintf(names[i], FO_NAME_MAX, "cam %d", i);
		many[i].source = names[i];
		many[i].has_signal = false;
	}
	fo_observe(&st, &c, many, FO_MAX_SOURCES + 8, 1000);
	CHECK(st.count == FO_MAX_SOURCES);
	CHECK(fo_source_reason(&st, "cam 0") == FO_SIGNAL_LOST);
	CHECK(fo_source_reason(&st, "cam 70") == FO_OK);
	CHECK(!fo_source_is_down(&st, &c, "cam 0", 2500));
	CHECK(fo_source_is_down(&st, &c, "cam 0", 3000));
}

int main(void)
{
	test_bars();
	test_saved_picture();
	test_dead_feed_on_program();
	test_a_blip_is_not_a_failover();
	test_dead_feed_off_program_changes_nothing();
	test_walks_the_fallback_list();
	test_a_sources_own_scene();
	test_colour_bars_and_saved_pictures();
	test_frozen_only_when_asked();
	test_unwatched_sources();
	test_going_back();
	test_the_operator_wins();
	test_switch_still_on_its_way();
	test_off_quiet_and_limits();
	test_config();
	if (failures) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("ctd-failover-tests: all passed\n");
	return 0;
}
