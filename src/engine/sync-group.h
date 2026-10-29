/*
 * A sync group's controller (SPEC S9 to S14): plain arithmetic, no FFmpeg and no libobs, so the OBS plugin and the
 * probe's group mode run the same code. The host keeps one struct sync_group per group, feeds it each member's
 * timecode about ten times a second, and applies the group delay it hands back on top of the member's own delays (S14).
 *
 * Align to slowest (S10, the default): each member's "raw" timecode is the one arriving now, its last pin moved on by
 * the time since. The member with the earliest is the reference and gets no delay; every other member is delayed by how
 * far ahead it is. Fixed latency: every member is shown at time of day minus N ms (needs the timecode to be time of
 * day and the machine's clock NTP synced).
 *
 * S11: the offset is the median over 5 s. The first lock may jump; after it, the delay changes only when the median has
 * been more than half a frame off for 2 s, and then the host ramps to it. S12: a member that needs more than the max is
 * held at the max (sync lost). S13: a member without timecode freewheels on its last delay (sync lost), and others keep
 * using its last pin, moved on, as a reference; it relocks when timecode returns.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SYNC_MAX_MEMBERS 16
#define SYNC_SAMPLES 64 /* 5 s at 10 a second, with room */

enum sync_mode {
	SYNC_ALIGN_TO_SLOWEST,
	SYNC_FIXED_LATENCY,
};

enum sync_state {
	SYNC_NO_TIMECODE, /* no timecode since it joined */
	SYNC_MEASURING,   /* timecode: measuring for the first lock */
	SYNC_LOCKED,
	SYNC_ADJUSTING, /* locked, moving to a new delay */
	SYNC_FREEWHEEL, /* lost its timecode: holding its last delay (sync lost) */
	SYNC_AT_MAX,    /* needs more than the group's max delay: held at the max (sync lost) */
};

const char *sync_state_name(enum sync_state state);
bool sync_state_lost(enum sync_state state);

/* one member's timecode, as the host sees it this round */
struct sync_input {
	bool timecode;     /* its timecode is locked now */
	bool pinned;       /* it has had timecode at some point, so tc_seconds and pin_ns mean something */
	double tc_seconds; /* the timecode at its pin, in seconds since midnight */
	uint64_t pin_ns;   /* that moment on the host clock, before any delay */
	double fps;
	bool settled; /* the host has finished moving to the delay last handed out */
};

struct sync_member {
	bool used;
	enum sync_state state;
	double samples[SYNC_SAMPLES]; /* the delay it should have, ms, as measured each round */
	uint64_t sample_ns[SYNC_SAMPLES];
	int next;
	double offset_ms; /* the median over the last 5 s */
	double delay_ms;  /* the group delay handed out */
	bool locked;      /* it has locked once */
	bool reference;   /* the earliest timecode this round */
	uint64_t measuring_since_ns;
	uint64_t off_since_ns;    /* the median has been more than half a frame off since then */
	uint64_t relock_since_ns; /* S13: timecode came back then; measuring afresh */
	long jumps;               /* delays applied at once: only the first lock should */
};

struct sync_group {
	enum sync_mode mode;
	int max_delay_ms;     /* S12: 1000 by default, up to 5000 */
	int fixed_latency_ms; /* S10: fixed latency mode's N */
	struct sync_member members[SYNC_MAX_MEMBERS];
};

/* what to do with one member */
struct sync_output {
	double delay_ms; /* the group delay */
	bool jump;       /* apply at once (the first lock) instead of ramping */
	bool changed;    /* different from last round */
};

void sync_group_init(struct sync_group *g);
/* a member joins (or rejoins) in slot i: nothing carried over */
void sync_member_join(struct sync_group *g, int i, double starting_delay_ms);

/*
 * One round. in[i] is read for every slot in use. now_ns is the host clock; tod_ns the time of day (ns since local
 * midnight), used in fixed latency mode only.
 */
void sync_group_update(struct sync_group *g, const struct sync_input *in, uint64_t now_ns, int64_t tod_ns,
		       struct sync_output *out);

#ifdef __cplusplus
}
#endif
