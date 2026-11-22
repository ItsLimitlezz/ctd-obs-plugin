/* CTD Producer dock logic: see producer-core.h. No libobs, no Qt, no logging. */

#include "producer-core.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------- platforms --- */

/* CTD Relay's platform ids (ctd-relay docs/API.md, src.platform). Colours are the platforms' own, moved towards
 * light where their brand colour is too dark to read on the Night theme (Twitch's purple, Facebook's blue). YouTube's
 * red is moved away from the dock's tally red, which only ever means DOWN. */
static const struct producer_platform platforms[] = {
	{"twitch", "Twitch", "TW", 0xa970ff},
	{"youtube", "YouTube", "YT", 0xff4e45},
	{"kick", "Kick", "KI", 0x53fc18},
	{"facebook", "Facebook", "FB", 0x4c9bff},
	{"x", "X", "X", 0xe7e9ea},
	{"instagram", "Instagram", "IG", 0xf56aa0},
	{"tiktok", "TikTok", "TT", 0xfe2c55},
	{"custom", "Custom", "RT", 0x8ba3af},
	{"core", "CTD Relay", "CTD", 0x39e7ff},
};
static const struct producer_platform unknown_platform = {"", "", "\xc2\xb7\xc2\xb7", 0x8ba3af};

static char lower(char c)
{
	return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

static bool same_nocase(const char *a, const char *b)
{
	while (*a && *b) {
		if (lower(*a) != lower(*b))
			return false;
		a++;
		b++;
	}
	return *a == *b;
}

const struct producer_platform *producer_platform(const char *id)
{
	if (id && *id) {
		for (size_t i = 0; i < sizeof(platforms) / sizeof(platforms[0]); i++) {
			if (same_nocase(platforms[i].id, id))
				return &platforms[i];
		}
	}
	return &unknown_platform;
}

static int hex_value(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	c = lower(c);
	return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

bool producer_parse_color(const char *text, uint32_t *rgb)
{
	if (!text || text[0] != '#' || strlen(text) != 7)
		return false;
	uint32_t value = 0;
	for (int i = 1; i < 7; i++) {
		const int digit = hex_value(text[i]);
		if (digit < 0)
			return false;
		value = (value << 4) | (uint32_t)digit;
	}
	if (rgb)
		*rgb = value;
	return true;
}

/* relative luminance (sRGB, WCAG) of 0xRRGGBB, 0..1 */
static double luminance(uint32_t rgb)
{
	double channel[3];
	for (int i = 0; i < 3; i++) {
		const double c = (double)((rgb >> (16 - 8 * i)) & 0xff) / 255.0;
		channel[i] = c <= 0.03928 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
	}
	return 0.2126 * channel[0] + 0.7152 * channel[1] + 0.0722 * channel[2];
}

uint32_t producer_readable_color(uint32_t rgb)
{
	/* about 4.5:1 against the chat's background (#08111a, luminance about 0.006) */
	const double wanted = 0.20;
	rgb &= 0xffffff;
	if (luminance(rgb) >= wanted)
		return rgb;
	/* mix with white in steps; white itself always passes */
	for (int step = 1; step <= 16; step++) {
		uint32_t mixed = 0;
		for (int i = 0; i < 3; i++) {
			const uint32_t c = (rgb >> (16 - 8 * i)) & 0xff;
			const uint32_t m = c + (255 - c) * (uint32_t)step / 16;
			mixed = (mixed << 8) | m;
		}
		if (luminance(mixed) >= wanted)
			return mixed;
	}
	return 0xffffff;
}

/* ------------------------------------------------------------- viewers --- */

static bool is(const char *phase, const char *name)
{
	return phase && strcmp(phase, name) == 0;
}

enum producer_state producer_dest_state(const char *phase, bool live, bool down)
{
	if (live || is(phase, "live"))
		return PRODUCER_LIVE;
	if (is(phase, "waiting-for-feed"))
		return PRODUCER_WAITING;
	if (is(phase, "recovering") || is(phase, "retrying"))
		return PRODUCER_RECOVERING;
	if (is(phase, "error"))
		return PRODUCER_DOWN;
	if (is(phase, "preparing") || is(phase, "connecting") || is(phase, "confirming"))
		/* CTD Relay keeps `down` set while a retry works its way back up through these steps */
		return down ? PRODUCER_RECOVERING : PRODUCER_STARTING;
	/* idle, disabled, ending, or a phase this build doesn't know: down still means "should be up and isn't" */
	return down ? PRODUCER_DOWN : PRODUCER_OFFLINE;
}

long long producer_dest_viewers(const struct producer_dest *dest)
{
	if (!dest || !dest->live || !dest->has_viewers)
		return -1;
	if (!(dest->viewers >= 0.0) || dest->viewers > 9.0e15) /* NaN fails the first test */
		return -1;
	return (long long)floor(dest->viewers);
}

bool producer_total_viewers(const struct producer_dest *dests, size_t count, long long *total)
{
	long long sum = 0;
	bool known = false;
	for (size_t i = 0; dests && i < count; i++) {
		const long long viewers = producer_dest_viewers(&dests[i]);
		if (viewers < 0)
			continue;
		sum += viewers;
		known = true;
	}
	if (total)
		*total = known ? sum : 0;
	return known;
}

void producer_format_count(long long n, char *out, size_t size)
{
	if (!out || !size)
		return;
	if (n < 0) {
		snprintf(out, size, "\xe2\x80\x94");
	} else if (n < 1000) {
		snprintf(out, size, "%lld", n);
	} else if (n < 10000) {
		snprintf(out, size, "%lld,%03lld", n / 1000, n % 1000);
	} else {
		/* one decimal, cut rather than rounded, so 999,950 reads 999.9K and never "1000.0K" */
		const bool millions = n >= 1000000;
		const long long unit = millions ? 1000000 : 1000;
		const long long tenths = n / (unit / 10);
		if (tenths % 10)
			snprintf(out, size, "%lld.%lld%c", tenths / 10, tenths % 10, millions ? 'M' : 'K');
		else
			snprintf(out, size, "%lld%c", tenths / 10, millions ? 'M' : 'K');
	}
}

/* ------------------------------------------------------------- chat --- */

static struct producer_line *line_at(struct producer_chat *chat, size_t index)
{
	return &chat->lines[(chat->head + index) % PRODUCER_CHAT_KEEP];
}

static void free_line(struct producer_line *line)
{
	free(line->author);
	free(line->text);
	memset(line, 0, sizeof(*line));
}

/* A copy of at most `max` bytes of text as one clean line: control characters (line breaks, tabs) and Unicode's line
 * and paragraph separators become spaces, runs of spaces at the ends go, and a text that is cut ends in "…" on a
 * UTF-8 boundary. Never NULL unless out of memory. */
static char *clean_copy(const char *text, size_t max)
{
	if (!text)
		text = "";
	while (*text == ' ' || (unsigned char)*text < 0x20) {
		if (!*text)
			break;
		text++;
	}
	const size_t length = strlen(text);
	size_t keep = length;
	bool cut = false;
	if (keep > max) {
		keep = max;
		/* back off to the start of the character the cut would split */
		while (keep > 0 && ((unsigned char)text[keep] & 0xc0) == 0x80)
			keep--;
		cut = true;
	}
	char *out = malloc(keep + 4);
	if (!out)
		return NULL;
	size_t n = 0;
	for (size_t i = 0; i < keep; i++) {
		const unsigned char c = (unsigned char)text[i];
		if (c < 0x20 || c == 0x7f) {
			out[n++] = ' ';
		} else if (c == 0xe2 && i + 2 < keep && (unsigned char)text[i + 1] == 0x80 &&
			   ((unsigned char)text[i + 2] == 0xa8 || (unsigned char)text[i + 2] == 0xa9)) {
			/* U+2028 / U+2029 */
			out[n++] = ' ';
			i += 2;
		} else if (c == 0xef && i + 2 < keep && (unsigned char)text[i + 1] == 0xb7 &&
			   (unsigned char)text[i + 2] >= 0x90 && (unsigned char)text[i + 2] <= 0xaf) {
			/* U+FDD0..U+FDEF: QTextCursor::insertText starts a new block at U+FDD0 and U+FDD1 */
			out[n++] = ' ';
			i += 2;
		} else {
			out[n++] = (char)c;
		}
	}
	while (n > 0 && out[n - 1] == ' ')
		n--;
	if (cut) {
		memcpy(out + n, "\xe2\x80\xa6", 3);
		n += 3;
	}
	out[n] = '\0';
	return out;
}

static void copy_fixed(char *out, size_t size, const char *text)
{
	snprintf(out, size, "%s", text ? text : "");
}

struct producer_chat *producer_chat_create(void)
{
	struct producer_chat *chat = calloc(1, sizeof(*chat));
	if (chat)
		chat->next_seq = 1;
	return chat;
}

void producer_chat_clear(struct producer_chat *chat)
{
	if (!chat)
		return;
	for (size_t i = 0; i < chat->count; i++)
		free_line(line_at(chat, i));
	chat->head = 0;
	chat->count = 0;
	/* next_seq keeps counting: a line added after a clear is still newer than anything shown before it */
}

void producer_chat_destroy(struct producer_chat *chat)
{
	producer_chat_clear(chat);
	free(chat);
}

size_t producer_chat_count(const struct producer_chat *chat)
{
	return chat ? chat->count : 0;
}

const struct producer_line *producer_chat_at(const struct producer_chat *chat, size_t index)
{
	if (!chat || index >= chat->count)
		return NULL;
	return &chat->lines[(chat->head + index) % PRODUCER_CHAT_KEEP];
}

uint64_t producer_chat_oldest_seq(const struct producer_chat *chat)
{
	const struct producer_line *line = producer_chat_at(chat, 0);
	return line ? line->seq : 0;
}

uint64_t producer_chat_newest_seq(const struct producer_chat *chat)
{
	const struct producer_line *line = chat && chat->count ? producer_chat_at(chat, chat->count - 1) : NULL;
	return line ? line->seq : 0;
}

static bool holds_id(const struct producer_chat *chat, const char *id)
{
	/* newest first: a repeat is nearly always of something recent */
	for (size_t i = chat->count; i > 0; i--) {
		if (strcmp(producer_chat_at(chat, i - 1)->id, id) == 0)
			return true;
	}
	return false;
}

bool producer_chat_add(struct producer_chat *chat, const char *id, const struct producer_line_in *in)
{
	if (!chat || !in || !id || !*id || strlen(id) >= PRODUCER_ID_MAX || holds_id(chat, id))
		return false;
	char *author = clean_copy(in->author, PRODUCER_AUTHOR_MAX);
	char *text = clean_copy(in->text, PRODUCER_TEXT_MAX);
	if (!author || !text) {
		free(author);
		free(text);
		return false;
	}
	if (chat->count == PRODUCER_CHAT_KEEP) {
		free_line(line_at(chat, 0));
		chat->head = (chat->head + 1) % PRODUCER_CHAT_KEEP;
		chat->count--;
	}
	struct producer_line *line = line_at(chat, chat->count++);
	memset(line, 0, sizeof(*line));
	line->seq = chat->next_seq++;
	copy_fixed(line->id, sizeof(line->id), id);
	copy_fixed(line->platform, sizeof(line->platform), in->platform);
	for (char *c = line->platform; *c; c++)
		*c = lower(*c);
	line->author = author;
	line->text = text;
	line->has_color = producer_parse_color(in->color, &line->color);
	line->join = in->join;
	line->event = in->event || in->join; /* a presence line is always a notice, never a comment */
	line->highlighted = in->highlighted;
	line->action = in->action;
	line->ts = in->ts;
	return true;
}

/* whether `id` could ever be a line of its own: not empty, short enough, and not one of the first `kept` lines */
static bool holdable(struct producer_chat *chat, size_t kept, const char *id)
{
	if (!id || !*id || strlen(id) >= PRODUCER_ID_MAX)
		return false;
	for (size_t i = 0; i < kept; i++)
		if (strcmp(line_at(chat, i)->id, id) == 0)
			return false;
	return true;
}

void producer_chat_sync(struct producer_chat *chat, const char *const *ids, size_t count, producer_load_cb load,
			void *ctx, struct producer_sync *out)
{
	struct producer_sync result = {0};
	if (out)
		*out = result;
	if (!chat)
		return;
	if (!ids)
		count = 0;
	/* only the newest KEEP can end up in the ring */
	size_t first = count > PRODUCER_CHAT_KEEP ? count - PRODUCER_CHAT_KEEP : 0;

	/* one pass over both lists: a held line stays when its id is the next one in the list */
	const size_t held = chat->count;
	size_t next = first; /* the list item the next kept line has to match */
	size_t kept = 0;
	for (size_t i = 0; i < held; i++) {
		struct producer_line *line = line_at(chat, i);
		/* ids the ring can never hold (none, too long, or a repeat of one kept) are passed over, not matched */
		while (next < count && !(ids[next] && strcmp(line->id, ids[next]) == 0) &&
		       !holdable(chat, kept, ids[next]))
			next++;
		if (next < count && ids[next] && strcmp(line->id, ids[next]) == 0) {
			if (kept != i) {
				*line_at(chat, kept) = *line;
				memset(line, 0, sizeof(*line));
			}
			kept++;
			next++;
		} else {
			if (kept)
				result.removed_inner++;
			else
				result.removed_front++;
			free_line(line);
		}
	}
	chat->count = kept;
	if (!kept)
		chat->head = 0;

	/* the rest of the list is new */
	for (size_t i = next; i < count; i++) {
		struct producer_line_in in = {0};
		if (!ids[i] || !*ids[i] || !load || !load(ctx, i, &in))
			continue;
		const uint64_t oldest_before = producer_chat_oldest_seq(chat);
		const size_t count_before = chat->count;
		if (!producer_chat_add(chat, ids[i], &in))
			continue;
		result.appended++;
		/* a full ring gave up its oldest line to make room */
		if (count_before == PRODUCER_CHAT_KEEP && producer_chat_oldest_seq(chat) != oldest_before)
			result.removed_front++;
	}
	if (out)
		*out = result;
}

bool producer_line_visible(const struct producer_line *line, const struct producer_filter *filter)
{
	if (!line)
		return false;
	if (!filter)
		return true;
	if (filter->hide_joins && line->join)
		return false;
	for (size_t i = 0; filter->hidden_platforms && i < filter->hidden_count; i++) {
		if (filter->hidden_platforms[i] && same_nocase(filter->hidden_platforms[i], line->platform))
			return false;
	}
	return true;
}

size_t producer_chat_visible_after(const struct producer_chat *chat, uint64_t seq, const struct producer_filter *filter)
{
	size_t visible = 0;
	/* newest first, so it stops at the first line that is old enough */
	for (size_t i = producer_chat_count(chat); i > 0; i--) {
		const struct producer_line *line = producer_chat_at(chat, i - 1);
		if (line->seq <= seq)
			break;
		if (producer_line_visible(line, filter))
			visible++;
	}
	return visible;
}
