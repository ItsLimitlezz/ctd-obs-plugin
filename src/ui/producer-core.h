/*
 * The CTD Producer dock's logic with no OBS and no Qt in it: which viewer counts are known and what they add up to,
 * what state a destination is in, how each platform is labelled and coloured, and the merged chat (one list of lines
 * from every platform, oldest first, the last PRODUCER_CHAT_KEEP of them).
 *
 * Pure C, so it is tested on its own (test/producer). The dock (src/ui/producer-dock.cpp) feeds it from the CTD Relay
 * client's snapshot and draws what it holds. Nothing here logs, and nothing here should: chat text is never logged.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------- platforms --- */

struct producer_platform {
	const char *id;    /* CTD Relay's platform id: "twitch", "youtube", ... */
	const char *name;  /* "Twitch" */
	const char *badge; /* two or three capitals for the badge in front of a chat line: "TW" */
	uint32_t rgb;      /* the platform's colour, readable on the Night theme's dark surfaces */
};

/* Never NULL: an id this build doesn't know (or NULL) gets the neutral entry, whose id and name are empty and whose
 * badge is "··". Ids are matched without regard to case. */
const struct producer_platform *producer_platform(const char *id);

/* "#RRGGBB" (as CTD Relay sends a chat author's colour) to 0xRRGGBB. False for anything else. */
bool producer_parse_color(const char *text, uint32_t *rgb);
/* The same colour, lightened until it can be read on the dock's dark background (a viewer's pure blue or black name
 * would vanish). Colours that are already light enough come back unchanged. */
uint32_t producer_readable_color(uint32_t rgb);

/* ------------------------------------------------------------- viewers --- */

enum producer_state {
	PRODUCER_OFFLINE,    /* not started, ended, switched off, or skipped by test mode */
	PRODUCER_STARTING,   /* on its way to live for the first time */
	PRODUCER_LIVE,       /* reaching the platform */
	PRODUCER_RECOVERING, /* was up or should be, and CTD Relay is bringing it back */
	PRODUCER_WAITING,    /* waiting for OBS's feed */
	PRODUCER_DOWN,       /* failed; CTD Relay is not retrying */
};

/* A destination's state from CTD Relay's status fields (docs/API.md of ctd-relay: phase, live, down). */
enum producer_state producer_dest_state(const char *phase, bool live, bool down);

struct producer_dest {
	bool live;
	bool has_viewers; /* the status carried a number (CTD Relay sends null, which is no number, while unknown) */
	double viewers;
};

/* The count to show on a destination: only while it is live and the platform reports one. -1 while unknown, which
 * is shown as a dash and never as 0. */
long long producer_dest_viewers(const struct producer_dest *dest);
/* Every platform's viewers added up: the sum of the known counts. False (and *total 0) when none is known. */
bool producer_total_viewers(const struct producer_dest *dests, size_t count, long long *total);
/* 214 -> "214", 1234 -> "1,234", 15300 -> "15.3K", 2400000 -> "2.4M". Negative -> "—". */
void producer_format_count(long long n, char *out, size_t size);

/* ------------------------------------------------------------- chat --- */

#define PRODUCER_CHAT_KEEP 500
#define PRODUCER_ID_MAX 64
#define PRODUCER_PLATFORM_MAX 24
#define PRODUCER_TEXT_MAX 600 /* bytes of one line's text kept (cut on a UTF-8 boundary, "…" added) */
#define PRODUCER_AUTHOR_MAX 80

struct producer_line {
	uint64_t seq; /* 1 for the first line ever added, then counting up; never reused, so newer lines sort higher */
	char id[PRODUCER_ID_MAX]; /* CTD Relay's envelope id */
	char platform[PRODUCER_PLATFORM_MAX];
	char *author; /* display name; never NULL */
	char *text;   /* one line: line breaks and control characters are spaces; never NULL */
	uint32_t color;
	bool has_color;   /* the author has a colour of their own */
	bool join;        /* a "<name> joined" presence line, not a comment */
	bool event;       /* a platform notice shown in chat (a sub, a raid, ...) */
	bool highlighted; /* a highlighted or paid message */
	bool action;      /* a /me message */
	int64_t ts;       /* CTD Relay's time for it, Unix epoch ms */
};

/* What the dock hands over for a new line. Strings are copied; NULL counts as empty. */
struct producer_line_in {
	const char *platform;
	const char *author;
	const char *text;
	const char *color; /* "#RRGGBB" or empty */
	bool join, event, highlighted, action;
	int64_t ts;
};

struct producer_chat {
	struct producer_line lines[PRODUCER_CHAT_KEEP]; /* a ring: lines[(head + i) % KEEP] is the i-th oldest */
	size_t head;
	size_t count;
	uint64_t next_seq;
};

/* Heap-allocated (it is about 70 KB). Free with producer_chat_destroy. */
struct producer_chat *producer_chat_create(void);
void producer_chat_destroy(struct producer_chat *chat);
void producer_chat_clear(struct producer_chat *chat);

size_t producer_chat_count(const struct producer_chat *chat);
/* The i-th oldest line (0 = oldest), or NULL past the end. Valid until the next sync, add or clear. */
const struct producer_line *producer_chat_at(const struct producer_chat *chat, size_t index);
/* seq of the oldest and newest line held; 0 while empty */
uint64_t producer_chat_oldest_seq(const struct producer_chat *chat);
uint64_t producer_chat_newest_seq(const struct producer_chat *chat);

/* Adds one line as the newest. When the ring is full the oldest line goes. False (nothing added) when id is empty
 * or a line with that id is already held. */
bool producer_chat_add(struct producer_chat *chat, const char *id, const struct producer_line_in *in);

/* Fills *out for the line at `index` of the list given to producer_chat_sync. Return false to skip that line. The
 * strings only have to live until the callback is called again or the sync returns. */
typedef bool (*producer_load_cb)(void *ctx, size_t index, struct producer_line_in *out);

struct producer_sync {
	size_t removed_front; /* lines dropped from the old end, before any line that stayed (they scrolled out) */
	size_t removed_inner; /* lines dropped after a line that stayed (a moderator removed them): redraw the list */
	size_t appended;      /* new lines, all newer than everything kept */
};

/*
 * Makes the ring match CTD Relay's list of recent chat: `ids` are the envelope ids, oldest first, as the client's
 * snapshot holds them (only the newest PRODUCER_CHAT_KEEP count). Lines already held keep their seq; lines whose id
 * is gone are dropped; ids not held yet are loaded with `load` and added as the newest, in the list's order. The list
 * is in arrival order, which is CTD Relay's own merge of every platform's chat, so the ring stays chronological
 * across platforms.
 *
 * The work is one pass over both lists plus the new lines, so a sync after one new message costs a few hundred string
 * comparisons and one allocation.
 */
void producer_chat_sync(struct producer_chat *chat, const char *const *ids, size_t count, producer_load_cb load,
			void *ctx, struct producer_sync *out);

/* What the user chose to hide. */
struct producer_filter {
	const char *const *hidden_platforms; /* platform ids */
	size_t hidden_count;
	bool hide_joins;
};

bool producer_line_visible(const struct producer_line *line, const struct producer_filter *filter);
/* Visible lines newer than `seq`: the "N new messages" count while the list is scrolled up. */
size_t producer_chat_visible_after(const struct producer_chat *chat, uint64_t seq,
				   const struct producer_filter *filter);

#ifdef __cplusplus
}
#endif
