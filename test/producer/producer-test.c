/* ctd-producer-tests: the CTD Producer dock's logic (src/ui/producer-core.c): viewer totals with unknown counts,
 * destination states, platform styles, and the merged chat ring (order, moderation, the 500-line limit, filters).
 * No libobs, no Qt. Run with ctest (test/producer/CMakeLists.txt). */
#include "producer-core.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                            \
	do {                                                                   \
		if (!(cond)) {                                                 \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                            \
		}                                                              \
	} while (0)

#define CHECK_STR(got, want)                                                                               \
	do {                                                                                               \
		const char *got_ = (got), *want_ = (want);                                                 \
		if (strcmp(got_, want_) != 0) {                                                            \
			printf("FAIL %s:%d: got \"%s\", want \"%s\"\n", __FILE__, __LINE__, got_, want_); \
			failures++;                                                                        \
		}                                                                                          \
	} while (0)

/* ------------------------------------------------------------- viewers --- */

static struct producer_dest dest(bool live, bool has, double viewers)
{
	struct producer_dest d = {live, has, viewers};
	return d;
}

static void test_dest_viewers(void)
{
	struct producer_dest d = dest(true, true, 214);
	CHECK(producer_dest_viewers(&d) == 214);
	d = dest(true, true, 0); /* a real zero is a count */
	CHECK(producer_dest_viewers(&d) == 0);
	d = dest(true, false, 0); /* CTD Relay sent null: unknown, not zero */
	CHECK(producer_dest_viewers(&d) == -1);
	d = dest(false, true, 99); /* a count left over from before it went down */
	CHECK(producer_dest_viewers(&d) == -1);
	d = dest(true, true, -1);
	CHECK(producer_dest_viewers(&d) == -1);
	d = dest(true, true, NAN);
	CHECK(producer_dest_viewers(&d) == -1);
	d = dest(true, true, 12.9);
	CHECK(producer_dest_viewers(&d) == 12);
	CHECK(producer_dest_viewers(NULL) == -1);
}

static void test_total_viewers(void)
{
	long long total = -5;
	/* the same cases as CTD Relay's dashboard: known counts of live destinations add up, the rest is left out */
	struct producer_dest mixed[] = {dest(true, true, 214), dest(true, false, 0), dest(true, true, 6),
					dest(false, true, 99)};
	CHECK(producer_total_viewers(mixed, 4, &total));
	CHECK(total == 220);

	/* nothing known: no total at all, so the dock shows a dash and never 0 */
	struct producer_dest unknown[] = {dest(true, false, 0), dest(false, false, 0)};
	total = -5;
	CHECK(!producer_total_viewers(unknown, 2, &total));
	CHECK(total == 0);

	/* one live platform that reports zero viewers: that is a known total of 0 */
	struct producer_dest zero[] = {dest(true, true, 0), dest(true, false, 0)};
	CHECK(producer_total_viewers(zero, 2, &total));
	CHECK(total == 0);

	CHECK(!producer_total_viewers(NULL, 0, &total));
	CHECK(!producer_total_viewers(mixed, 0, &total));
	CHECK(producer_total_viewers(mixed, 4, NULL)); /* a caller that only asks whether one is known */

	struct producer_dest bad[] = {dest(true, true, NAN), dest(true, true, -3), dest(true, true, 40)};
	CHECK(producer_total_viewers(bad, 3, &total));
	CHECK(total == 40);
}

static void test_format_count(void)
{
	char out[32];
	producer_format_count(0, out, sizeof(out));
	CHECK_STR(out, "0");
	producer_format_count(214, out, sizeof(out));
	CHECK_STR(out, "214");
	producer_format_count(999, out, sizeof(out));
	CHECK_STR(out, "999");
	producer_format_count(1000, out, sizeof(out));
	CHECK_STR(out, "1,000");
	producer_format_count(1234, out, sizeof(out));
	CHECK_STR(out, "1,234");
	producer_format_count(9999, out, sizeof(out));
	CHECK_STR(out, "9,999");
	producer_format_count(10000, out, sizeof(out));
	CHECK_STR(out, "10K");
	producer_format_count(15300, out, sizeof(out));
	CHECK_STR(out, "15.3K");
	producer_format_count(999950, out, sizeof(out));
	CHECK_STR(out, "999.9K");
	producer_format_count(1000000, out, sizeof(out));
	CHECK_STR(out, "1M");
	producer_format_count(2400000, out, sizeof(out));
	CHECK_STR(out, "2.4M");
	producer_format_count(-1, out, sizeof(out));
	CHECK_STR(out, "\xe2\x80\x94");
	/* a buffer too small never overflows */
	char tiny[3] = {'x', 'x', 'x'};
	producer_format_count(123456, tiny, sizeof(tiny));
	CHECK(tiny[2] == '\0');
	producer_format_count(5, NULL, 0);
}

static void test_dest_state(void)
{
	CHECK(producer_dest_state("live", true, false) == PRODUCER_LIVE);
	CHECK(producer_dest_state("live", false, false) == PRODUCER_LIVE);
	CHECK(producer_dest_state("idle", false, false) == PRODUCER_OFFLINE);
	CHECK(producer_dest_state("disabled", false, false) == PRODUCER_OFFLINE);
	CHECK(producer_dest_state("ending", false, false) == PRODUCER_OFFLINE);
	CHECK(producer_dest_state("preparing", false, false) == PRODUCER_STARTING);
	CHECK(producer_dest_state("connecting", false, false) == PRODUCER_STARTING);
	CHECK(producer_dest_state("confirming", false, false) == PRODUCER_STARTING);
	/* a retry on its way back up is still down for CTD Relay, so it reads as recovering, not as a first start */
	CHECK(producer_dest_state("connecting", false, true) == PRODUCER_RECOVERING);
	CHECK(producer_dest_state("recovering", false, true) == PRODUCER_RECOVERING);
	CHECK(producer_dest_state("retrying", false, true) == PRODUCER_RECOVERING);
	CHECK(producer_dest_state("waiting-for-feed", false, true) == PRODUCER_WAITING);
	CHECK(producer_dest_state("error", false, true) == PRODUCER_DOWN);
	CHECK(producer_dest_state("error", false, false) == PRODUCER_DOWN);
	/* a phase from a newer CTD Relay: down decides */
	CHECK(producer_dest_state("something-new", false, true) == PRODUCER_DOWN);
	CHECK(producer_dest_state("something-new", false, false) == PRODUCER_OFFLINE);
	CHECK(producer_dest_state(NULL, false, false) == PRODUCER_OFFLINE);
	CHECK(producer_dest_state("", false, false) == PRODUCER_OFFLINE);
}

/* ------------------------------------------------------------- platforms --- */

static void test_platforms(void)
{
	CHECK_STR(producer_platform("twitch")->name, "Twitch");
	CHECK_STR(producer_platform("twitch")->badge, "TW");
	CHECK_STR(producer_platform("YouTube")->id, "youtube"); /* case doesn't matter */
	CHECK_STR(producer_platform("instagram")->badge, "IG");
	CHECK_STR(producer_platform("tiktok")->badge, "TT");
	CHECK_STR(producer_platform("x")->name, "X");
	/* unknown, empty and NULL all get the neutral entry, never NULL */
	CHECK_STR(producer_platform("myspace")->id, "");
	CHECK_STR(producer_platform("")->id, "");
	CHECK(producer_platform(NULL) == producer_platform("myspace"));
	CHECK(*producer_platform(NULL)->badge);
	/* every known platform has its own badge */
	const char *ids[] = {"twitch", "youtube", "kick", "facebook", "x", "instagram", "tiktok", "custom", "core"};
	for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
		CHECK_STR(producer_platform(ids[i])->id, ids[i]);
		for (size_t j = i + 1; j < sizeof(ids) / sizeof(ids[0]); j++)
			CHECK(strcmp(producer_platform(ids[i])->badge, producer_platform(ids[j])->badge) != 0);
	}
}

static void test_colors(void)
{
	uint32_t rgb = 0;
	CHECK(producer_parse_color("#1E90FF", &rgb) && rgb == 0x1e90ff);
	CHECK(producer_parse_color("#ffffff", &rgb) && rgb == 0xffffff);
	CHECK(producer_parse_color("#000000", NULL));
	CHECK(!producer_parse_color("", &rgb));
	CHECK(!producer_parse_color(NULL, &rgb));
	CHECK(!producer_parse_color("1E90FF", &rgb));
	CHECK(!producer_parse_color("#1E90F", &rgb));
	CHECK(!producer_parse_color("#1E90FFAA", &rgb));
	CHECK(!producer_parse_color("#GG90FF", &rgb));

	/* light colours are left alone; dark ones are lifted, keeping some of their hue */
	CHECK(producer_readable_color(0xffffff) == 0xffffff);
	CHECK(producer_readable_color(0x39e7ff) == 0x39e7ff);
	CHECK(producer_readable_color(0x000000) != 0x000000);
	const uint32_t blue = producer_readable_color(0x0000ff);
	CHECK(blue != 0x0000ff);
	CHECK((blue & 0xff) == 0xff);                 /* still the bluest channel */
	CHECK(((blue >> 16) & 0xff) < (blue & 0xff)); /* and not just white */
	/* every platform colour is readable as it is */
	const char *ids[] = {"twitch", "youtube", "kick", "facebook", "x", "instagram", "tiktok", "custom", "core"};
	for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++)
		CHECK(producer_readable_color(producer_platform(ids[i])->rgb) == producer_platform(ids[i])->rgb);
}

/* ------------------------------------------------------------- chat --- */

/* A stand-in for the CTD Relay client's list of recent chat: ids oldest first, and the line behind each. */
#define FAKE_MAX 1200

struct fake_message {
	char id[PRODUCER_ID_MAX];
	char platform[PRODUCER_PLATFORM_MAX];
	char author[32];
	char text[64];
	bool join;
};

static struct fake_message fake[FAKE_MAX];
static const char *fake_ids[FAKE_MAX];
static size_t fake_count;
static int loads; /* how many lines the ring asked for */

static void fake_reset(void)
{
	fake_count = 0;
	loads = 0;
}

static void fake_push(const char *id, const char *platform, const char *author, const char *text)
{
	struct fake_message *m = &fake[fake_count];
	memset(m, 0, sizeof(*m));
	snprintf(m->id, sizeof(m->id), "%s", id);
	snprintf(m->platform, sizeof(m->platform), "%s", platform);
	snprintf(m->author, sizeof(m->author), "%s", author);
	snprintf(m->text, sizeof(m->text), "%s", text);
	fake_count++;
	for (size_t i = 0; i < fake_count; i++)
		fake_ids[i] = fake[i].id;
}

static void fake_remove(size_t index)
{
	memmove(&fake[index], &fake[index + 1], (fake_count - index - 1) * sizeof(fake[0]));
	fake_count--;
	for (size_t i = 0; i < fake_count; i++)
		fake_ids[i] = fake[i].id;
}

static bool fake_load(void *ctx, size_t index, struct producer_line_in *out)
{
	(void)ctx;
	loads++;
	out->platform = fake[index].platform;
	out->author = fake[index].author;
	out->text = fake[index].text;
	out->join = fake[index].join;
	out->color = "";
	return true;
}

static struct producer_sync sync(struct producer_chat *chat)
{
	struct producer_sync result;
	producer_chat_sync(chat, fake_ids, fake_count, fake_load, NULL, &result);
	return result;
}

/* the ring's ids, oldest first, joined with spaces */
static const char *ids_of(const struct producer_chat *chat)
{
	static char out[4096];
	out[0] = '\0';
	for (size_t i = 0; i < producer_chat_count(chat); i++) {
		if (i)
			strcat(out, " ");
		strcat(out, producer_chat_at(chat, i)->id);
	}
	return out;
}

static bool seqs_rise(const struct producer_chat *chat)
{
	for (size_t i = 1; i < producer_chat_count(chat); i++) {
		if (producer_chat_at(chat, i)->seq <= producer_chat_at(chat, i - 1)->seq)
			return false;
	}
	return true;
}

static void test_chat_merge_order(void)
{
	struct producer_chat *chat = producer_chat_create();
	CHECK(producer_chat_count(chat) == 0);
	CHECK(producer_chat_oldest_seq(chat) == 0 && producer_chat_newest_seq(chat) == 0);
	CHECK(producer_chat_at(chat, 0) == NULL);

	/* three platforms talking over each other: one list, in CTD Relay's order */
	fake_reset();
	fake_push("01A", "twitch", "DJFan", "this set is fire");
	fake_push("01B", "instagram", "dj_fan", "hi from IG");
	fake_push("01C", "youtube", "Mia", "hello");
	fake_push("01D", "twitch", "DJFan", "again");
	struct producer_sync r = sync(chat);
	CHECK(r.appended == 4 && r.removed_front == 0 && r.removed_inner == 0);
	CHECK_STR(ids_of(chat), "01A 01B 01C 01D");
	CHECK_STR(producer_chat_at(chat, 0)->platform, "twitch");
	CHECK_STR(producer_chat_at(chat, 1)->platform, "instagram");
	CHECK_STR(producer_chat_at(chat, 2)->author, "Mia");
	CHECK_STR(producer_chat_at(chat, 3)->text, "again");
	CHECK(seqs_rise(chat));
	CHECK(producer_chat_oldest_seq(chat) == 1 && producer_chat_newest_seq(chat) == 4);

	/* nothing changed: nothing is loaded again and every line keeps its seq */
	loads = 0;
	r = sync(chat);
	CHECK(r.appended == 0 && r.removed_front == 0 && r.removed_inner == 0);
	CHECK(loads == 0);
	CHECK(producer_chat_newest_seq(chat) == 4);

	/* one new message: only that one is loaded */
	fake_push("01E", "kick", "Rob", "yo");
	r = sync(chat);
	CHECK(r.appended == 1 && r.removed_front == 0 && r.removed_inner == 0);
	CHECK(loads == 1);
	CHECK_STR(ids_of(chat), "01A 01B 01C 01D 01E");
	CHECK(producer_chat_newest_seq(chat) == 5);
	CHECK(seqs_rise(chat));

	producer_chat_destroy(chat);
}

static void test_chat_moderation(void)
{
	struct producer_chat *chat = producer_chat_create();
	fake_reset();
	fake_push("01A", "twitch", "a", "one");
	fake_push("01B", "twitch", "troll", "two");
	fake_push("01C", "youtube", "c", "three");
	fake_push("01D", "twitch", "troll", "four");
	fake_push("01E", "youtube", "e", "five");
	sync(chat);
	const uint64_t seq_c = producer_chat_at(chat, 2)->seq;

	/* a moderator deleted one message in the middle */
	fake_remove(1);
	struct producer_sync r = sync(chat);
	CHECK(r.removed_inner == 1 && r.removed_front == 0 && r.appended == 0);
	CHECK_STR(ids_of(chat), "01A 01C 01D 01E");
	CHECK(producer_chat_at(chat, 1)->seq == seq_c); /* the lines that stayed are the same lines */
	CHECK(seqs_rise(chat));

	/* a ban removed another one while a new message came in */
	fake_remove(2);
	fake_push("01F", "kick", "f", "six");
	r = sync(chat);
	CHECK(r.removed_inner == 1 && r.appended == 1);
	CHECK_STR(ids_of(chat), "01A 01C 01E 01F");
	CHECK(seqs_rise(chat));

	/* the oldest message deleted: nothing held is older, so it counts as gone from the front */
	fake_remove(0);
	r = sync(chat);
	CHECK(r.removed_front == 1 && r.removed_inner == 0 && r.appended == 0);
	CHECK_STR(ids_of(chat), "01C 01E 01F");

	/* the whole chat cleared (a profile change, or every platform cleared) */
	fake_reset();
	r = sync(chat);
	CHECK(r.removed_front == 3 && r.removed_inner == 0 && r.appended == 0);
	CHECK(producer_chat_count(chat) == 0);
	CHECK(producer_chat_oldest_seq(chat) == 0);

	/* and what comes after a clear is still newer than everything before it */
	fake_push("02A", "twitch", "a", "back");
	r = sync(chat);
	CHECK(r.appended == 1);
	CHECK(producer_chat_newest_seq(chat) > seq_c);

	producer_chat_destroy(chat);
}

static void test_chat_replaced(void)
{
	struct producer_chat *chat = producer_chat_create();
	fake_reset();
	fake_push("01A", "twitch", "a", "one");
	fake_push("01B", "twitch", "b", "two");
	sync(chat);

	/* another profile's chat: nothing in common */
	fake_reset();
	fake_push("09X", "kick", "x", "other");
	fake_push("09Y", "kick", "y", "profile");
	struct producer_sync r = sync(chat);
	CHECK(r.removed_front == 2 && r.removed_inner == 0 && r.appended == 2);
	CHECK_STR(ids_of(chat), "09X 09Y");
	CHECK(seqs_rise(chat));

	/* a list with a line in the middle the ring never saw: the result still matches the list, in its order */
	fake_reset();
	fake_push("09X", "kick", "x", "other");
	fake_push("09XX", "kick", "late", "squeezed in");
	fake_push("09Y", "kick", "y", "profile");
	r = sync(chat);
	CHECK_STR(ids_of(chat), "09X 09XX 09Y");
	CHECK(r.removed_inner == 1 && r.appended == 2);
	CHECK(seqs_rise(chat));

	/* the same id twice in the list is one line */
	fake_reset();
	fake_push("0AA", "twitch", "a", "one");
	fake_push("0AB", "twitch", "b", "two");
	fake_push("0AA", "twitch", "a", "one");
	sync(chat);
	CHECK_STR(ids_of(chat), "0AA 0AB");

	/* lines without an id can't be followed, so they are left out */
	fake_reset();
	fake_push("", "twitch", "a", "no id");
	fake_push("0BA", "twitch", "b", "ok");
	r = sync(chat);
	CHECK_STR(ids_of(chat), "0BA");
	CHECK(r.appended == 1);

	/* NULL list and NULL callback are an empty list and "skip everything" */
	producer_chat_sync(chat, NULL, 5, fake_load, NULL, &r);
	CHECK(producer_chat_count(chat) == 0);
	fake_reset();
	fake_push("0CA", "twitch", "a", "x");
	producer_chat_sync(chat, fake_ids, fake_count, NULL, NULL, &r);
	CHECK(producer_chat_count(chat) == 0 && r.appended == 0);
	producer_chat_sync(NULL, fake_ids, fake_count, fake_load, NULL, &r);
	producer_chat_sync(chat, fake_ids, fake_count, fake_load, NULL, NULL); /* no result wanted */
	CHECK(producer_chat_count(chat) == 1);

	producer_chat_destroy(chat);
}

static void make_id(char *out, size_t size, int n)
{
	snprintf(out, size, "01ID%06d", n);
}

static void test_chat_ring(void)
{
	struct producer_chat *chat = producer_chat_create();
	char id[PRODUCER_ID_MAX], text[32];

	/* the client's list holds at most KEEP too: fill it, then keep sliding it one message at a time */
	fake_reset();
	for (int n = 0; n < PRODUCER_CHAT_KEEP; n++) {
		make_id(id, sizeof(id), n);
		snprintf(text, sizeof(text), "message %d", n);
		fake_push(id, n % 2 ? "twitch" : "youtube", "someone", text);
	}
	struct producer_sync r = sync(chat);
	CHECK(r.appended == PRODUCER_CHAT_KEEP && r.removed_front == 0);
	CHECK(producer_chat_count(chat) == PRODUCER_CHAT_KEEP);
	CHECK_STR(producer_chat_at(chat, 0)->id, "01ID000000");

	for (int n = PRODUCER_CHAT_KEEP; n < PRODUCER_CHAT_KEEP + 700; n++) {
		fake_remove(0);
		make_id(id, sizeof(id), n);
		snprintf(text, sizeof(text), "message %d", n);
		fake_push(id, "twitch", "someone", text);
		loads = 0;
		r = sync(chat);
		CHECK(r.appended == 1 && r.removed_front == 1 && r.removed_inner == 0);
		CHECK(loads == 1);
		CHECK(producer_chat_count(chat) == PRODUCER_CHAT_KEEP);
	}
	/* the last KEEP, oldest first, wherever the ring's start has moved to */
	make_id(id, sizeof(id), 700);
	CHECK_STR(producer_chat_at(chat, 0)->id, id);
	CHECK_STR(producer_chat_at(chat, 0)->text, "message 700");
	make_id(id, sizeof(id), PRODUCER_CHAT_KEEP + 699);
	CHECK_STR(producer_chat_at(chat, PRODUCER_CHAT_KEEP - 1)->id, id);
	CHECK(producer_chat_at(chat, PRODUCER_CHAT_KEEP) == NULL);
	CHECK(seqs_rise(chat));
	CHECK(producer_chat_newest_seq(chat) == PRODUCER_CHAT_KEEP + 700);
	CHECK(producer_chat_oldest_seq(chat) == 701);

	/* a deletion in a ring whose start is in the middle of the array */
	fake_remove(250);
	r = sync(chat);
	CHECK(r.removed_inner == 1 && r.appended == 0);
	CHECK(producer_chat_count(chat) == PRODUCER_CHAT_KEEP - 1);
	CHECK(seqs_rise(chat));
	for (size_t i = 0; i < producer_chat_count(chat); i++)
		CHECK(strcmp(producer_chat_at(chat, i)->id, fake_ids[i]) == 0);

	/* a list longer than the ring: only its newest KEEP are held */
	producer_chat_clear(chat);
	fake_reset();
	for (int n = 0; n < PRODUCER_CHAT_KEEP + 120; n++) {
		make_id(id, sizeof(id), n);
		fake_push(id, "kick", "someone", "x");
	}
	loads = 0;
	r = sync(chat);
	CHECK(producer_chat_count(chat) == PRODUCER_CHAT_KEEP);
	CHECK(loads == PRODUCER_CHAT_KEEP); /* the 120 that would only be pushed out again are never loaded */
	make_id(id, sizeof(id), 120);
	CHECK_STR(producer_chat_at(chat, 0)->id, id);
	CHECK(seqs_rise(chat));

	/* adding straight to a full ring pushes the oldest out */
	struct producer_line_in in = {0};
	in.platform = "TWITCH";
	in.author = "late";
	in.text = "one more";
	in.color = "#1E90FF";
	CHECK(producer_chat_add(chat, "01ZZ", &in));
	CHECK(producer_chat_count(chat) == PRODUCER_CHAT_KEEP);
	make_id(id, sizeof(id), 121);
	CHECK_STR(producer_chat_at(chat, 0)->id, id);
	const struct producer_line *last = producer_chat_at(chat, PRODUCER_CHAT_KEEP - 1);
	CHECK_STR(last->id, "01ZZ");
	CHECK_STR(last->platform, "twitch"); /* stored in lower case */
	CHECK(last->has_color && last->color == 0x1e90ff);
	CHECK(!producer_chat_add(chat, "01ZZ", &in)); /* the same id again */
	CHECK(!producer_chat_add(chat, "", &in));
	CHECK(!producer_chat_add(chat, NULL, &in));
	CHECK(!producer_chat_add(chat, "01ZY", NULL));

	producer_chat_destroy(chat);
	producer_chat_destroy(NULL);
}

static void test_chat_text(void)
{
	struct producer_chat *chat = producer_chat_create();
	struct producer_line_in in = {0};

	/* one line per message, whatever a viewer typed */
	in.platform = "twitch";
	in.author = "  Spacey  ";
	in.text = "\n line one\r\nline two\tend \xe2\x80\xa8next\x7f ";
	CHECK(producer_chat_add(chat, "01A", &in));
	const struct producer_line *line = producer_chat_at(chat, 0);
	CHECK_STR(line->author, "Spacey");
	CHECK_STR(line->text, "line one  line two end  next");
	CHECK(!line->has_color && !line->join && !line->event);

	/* NULL strings are empty strings */
	memset(&in, 0, sizeof(in));
	CHECK(producer_chat_add(chat, "01B", &in));
	line = producer_chat_at(chat, 1);
	CHECK_STR(line->author, "");
	CHECK_STR(line->text, "");
	CHECK_STR(line->platform, "");

	/* a very long message is cut on a character boundary and marked */
	char longtext[PRODUCER_TEXT_MAX * 2 + 8];
	size_t n = 0;
	while (n + 2 < PRODUCER_TEXT_MAX * 2) { /* "é" over and over: two bytes each */
		longtext[n++] = '\xc3';
		longtext[n++] = '\xa9';
	}
	longtext[n] = '\0';
	in.text = longtext;
	CHECK(producer_chat_add(chat, "01C", &in));
	line = producer_chat_at(chat, 2);
	const size_t length = strlen(line->text);
	CHECK(length <= PRODUCER_TEXT_MAX + 3);
	CHECK(length >= PRODUCER_TEXT_MAX - 4);
	CHECK(strcmp(line->text + length - 3, "\xe2\x80\xa6") == 0);
	CHECK((length - 3) % 2 == 0); /* whole characters only */

	/* a join is always a notice */
	memset(&in, 0, sizeof(in));
	in.platform = "instagram";
	in.author = "dj_fan";
	in.text = "dj_fan joined";
	in.join = true;
	CHECK(producer_chat_add(chat, "01D", &in));
	line = producer_chat_at(chat, 3);
	CHECK(line->join && line->event);

	/* an id too long to hold is refused rather than cut (a cut id could match another line) */
	char longid[PRODUCER_ID_MAX + 8];
	memset(longid, 'A', sizeof(longid) - 1);
	longid[sizeof(longid) - 1] = '\0';
	CHECK(!producer_chat_add(chat, longid, &in));

	/* U+FDD0 / U+FDD1 start a new text block in Qt (one block per shown line): spaces, like line breaks */
	memset(&in, 0, sizeof(in));
	in.text = "a\xef\xb7\x90"
		  "b\xef\xb7\x91"
		  "c";
	CHECK(producer_chat_add(chat, "01E", &in));
	CHECK_STR(producer_chat_at(chat, 4)->text, "a b c");

	producer_chat_destroy(chat);
}

/* ids the ring can never hold (a repeat, an empty one) are passed over: the lines after them stay as they are, so a
 * sync never drops and re-adds them (each re-add rebuilt the whole chat view, up to ten times a second) */
static void test_chat_unholdable_ids(void)
{
	struct producer_chat *chat = producer_chat_create();
	fake_reset();
	fake_push("0DA", "twitch", "a", "one");
	fake_push("0DB", "twitch", "b", "two");
	fake_push("0DA", "twitch", "a", "one"); /* a repeat */
	fake_push("", "twitch", "x", "no id");  /* one the ring can't hold */
	sync(chat);
	const uint64_t seq_b = producer_chat_at(chat, 1)->seq;
	struct producer_sync r = sync(chat); /* nothing changed */
	CHECK(r.appended == 0 && r.removed_inner == 0 && r.removed_front == 0);
	for (int n = 0; n < 3; n++) {
		char id[8];
		snprintf(id, sizeof(id), "0DC%d", n);
		fake_push(id, "twitch", "c", "more");
		r = sync(chat);
		CHECK(r.appended == 1 && r.removed_inner == 0 && r.removed_front == 0);
	}
	CHECK(producer_chat_at(chat, 1)->seq == seq_b);
	CHECK_STR(ids_of(chat), "0DA 0DB 0DC0 0DC1 0DC2");
	producer_chat_destroy(chat);
}

static void test_chat_filters(void)
{
	struct producer_chat *chat = producer_chat_create();
	fake_reset();
	fake_push("01A", "twitch", "a", "one");
	fake_push("01B", "instagram", "b", "b joined");
	fake[1].join = true;
	fake_push("01C", "instagram", "c", "three");
	fake_push("01D", "youtube", "d", "four");
	fake_push("01E", "twitch", "e", "five");
	sync(chat);

	/* everything on: the chat is the whole merged list */
	struct producer_filter all = {NULL, 0, false};
	for (size_t i = 0; i < 5; i++)
		CHECK(producer_line_visible(producer_chat_at(chat, i), &all));
	CHECK(producer_line_visible(producer_chat_at(chat, 0), NULL));
	CHECK(!producer_line_visible(NULL, &all));
	CHECK(producer_chat_visible_after(chat, 0, &all) == 5);
	CHECK(producer_chat_visible_after(chat, 0, NULL) == 5);

	struct producer_filter no_joins = {NULL, 0, true};
	CHECK(!producer_line_visible(producer_chat_at(chat, 1), &no_joins));
	CHECK(producer_line_visible(producer_chat_at(chat, 2), &no_joins));
	CHECK(producer_chat_visible_after(chat, 0, &no_joins) == 4);

	const char *hidden[] = {"Instagram", "kick"};
	struct producer_filter no_ig = {hidden, 2, false};
	CHECK(producer_line_visible(producer_chat_at(chat, 0), &no_ig));
	CHECK(!producer_line_visible(producer_chat_at(chat, 1), &no_ig));
	CHECK(!producer_line_visible(producer_chat_at(chat, 2), &no_ig));
	CHECK(producer_chat_visible_after(chat, 0, &no_ig) == 3);

	/* "N new messages": what arrived after the line the list is stopped at, counting only what would be shown */
	const uint64_t stopped_at = producer_chat_at(chat, 1)->seq;
	CHECK(producer_chat_visible_after(chat, stopped_at, &all) == 3);
	CHECK(producer_chat_visible_after(chat, stopped_at, &no_ig) == 2);
	CHECK(producer_chat_visible_after(chat, producer_chat_newest_seq(chat), &all) == 0);
	CHECK(producer_chat_visible_after(chat, UINT64_MAX, &all) == 0);
	CHECK(producer_chat_visible_after(NULL, 0, &all) == 0);

	/* new messages while stopped keep counting */
	fake_push("01F", "instagram", "f", "six");
	fake_push("01G", "twitch", "g", "seven");
	sync(chat);
	CHECK(producer_chat_visible_after(chat, stopped_at, &all) == 5);
	CHECK(producer_chat_visible_after(chat, stopped_at, &no_ig) == 3);

	producer_chat_destroy(chat);
}

int main(void)
{
	test_dest_viewers();
	test_total_viewers();
	test_format_count();
	test_dest_state();
	test_platforms();
	test_colors();
	test_chat_merge_order();
	test_chat_moderation();
	test_chat_replaced();
	test_chat_ring();
	test_chat_text();
	test_chat_unholdable_ids();
	test_chat_filters();
	if (failures) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("ctd-producer-tests: all checks passed\n");
	return 0;
}
