/*
 * Sync group controller. See sync-group.h.
 */

#include "sync-group.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define WINDOW_NS 5000000000ull  /* S11: the median's window, and how long the first lock measures */
#define OFF_HOLD_NS 2000000000ull /* S11: how long the median must be off before the delay moves */
#define DAY_S 86400.0

static const char *const state_names[] = {"no-timecode", "measuring", "locked", "adjusting", "freewheel", "at-max"};

const char *sync_state_name(enum sync_state state)
{
	return state >= SYNC_NO_TIMECODE && state <= SYNC_AT_MAX ? state_names[state] : "?";
}

bool sync_state_lost(enum sync_state state)
{
	return state == SYNC_FREEWHEEL || state == SYNC_AT_MAX;
}

void sync_group_init(struct sync_group *g)
{
	memset(g, 0, sizeof(*g));
	g->mode = SYNC_ALIGN_TO_SLOWEST;
	g->max_delay_ms = 1000;
}

void sync_member_join(struct sync_group *g, int i, double starting_delay_ms)
{
	struct sync_member *m = &g->members[i];
	memset(m, 0, sizeof(*m));
	m->used = true;
	m->delay_ms = starting_delay_ms; /* S13: a member that comes back keeps its delay as the starting guess */
}

/* a - b in seconds, across midnight */
static double day_difference(double a, double b)
{
	double d = a - b;
	if (d > DAY_S / 2)
		d -= DAY_S;
	if (d < -DAY_S / 2)
		d += DAY_S;
	return d;
}

/* the timecode arriving now: the pin, moved on by the time since */
static double raw_now(const struct sync_input *in, uint64_t now_ns)
{
	return in->tc_seconds + (double)((int64_t)now_ns - (int64_t)in->pin_ns) / 1e9;
}

static int compare_doubles(const void *a, const void *b)
{
	const double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : x > y;
}

/* the median of the samples from the last 5 s */
static double median(const struct sync_member *m, uint64_t now_ns)
{
	double recent[SYNC_SAMPLES];
	int n = 0;
	for (int i = 0; i < SYNC_SAMPLES; i++)
		if (m->sample_ns[i] && now_ns - m->sample_ns[i] <= WINDOW_NS)
			recent[n++] = m->samples[i];
	if (!n)
		return m->delay_ms;
	qsort(recent, (size_t)n, sizeof(double), compare_doubles);
	return n % 2 ? recent[n / 2] : (recent[n / 2 - 1] + recent[n / 2]) / 2.0;
}

void sync_group_update(struct sync_group *g, const struct sync_input *in, uint64_t now_ns, int64_t tod_ns,
		       struct sync_output *out)
{
	/* the reference: the earliest timecode arriving now, freewheeling members included (S13) */
	bool have_reference = false;
	double reference = 0;
	int reference_index = -1;
	for (int i = 0; i < SYNC_MAX_MEMBERS; i++) {
		if (!g->members[i].used || !in[i].pinned)
			continue;
		const double raw = raw_now(&in[i], now_ns);
		if (!have_reference || day_difference(raw, reference) < 0) {
			reference = raw;
			reference_index = i;
			have_reference = true;
		}
	}
	const double max_ms = g->max_delay_ms > 0 ? g->max_delay_ms : 1000;

	for (int i = 0; i < SYNC_MAX_MEMBERS; i++) {
		struct sync_member *m = &g->members[i];
		if (!m->used)
			continue;
		m->reference = i == reference_index && g->mode == SYNC_ALIGN_TO_SLOWEST;
		out[i].delay_ms = m->delay_ms;
		out[i].jump = false;
		out[i].changed = false;
		if (!in[i].timecode) {
			/* S13: freewheel on the last delay */
			m->state = m->locked ? SYNC_FREEWHEEL : SYNC_NO_TIMECODE;
			m->off_since_ns = 0;
			m->measuring_since_ns = 0;
			continue;
		}

		/* S13: timecode back after a freewheel: the samples from before say nothing about now. Measure afresh for 5 s,
		 * holding the delay, then move (smoothly) if it must. */
		if (m->state == SYNC_FREEWHEEL) {
			memset(m->sample_ns, 0, sizeof(m->sample_ns));
			m->relock_since_ns = now_ns;
			m->off_since_ns = 0;
		}

		/* the delay this member should have, measured now */
		double wanted_ms;
		if (g->mode == SYNC_FIXED_LATENCY) {
			const double latency_ms =
				day_difference((double)tod_ns / 1e9, raw_now(&in[i], now_ns)) * 1000.0;
			wanted_ms = g->fixed_latency_ms - latency_ms;
		} else {
			wanted_ms = day_difference(raw_now(&in[i], now_ns), reference) * 1000.0;
		}
		m->samples[m->next] = wanted_ms;
		m->sample_ns[m->next] = now_ns;
		m->next = (m->next + 1) % SYNC_SAMPLES;
		m->offset_ms = median(m, now_ns);

		const double fps = in[i].fps > 1 ? in[i].fps : 30.0;
		const double half_frame_ms = 500.0 / fps;
		const bool over = m->offset_ms > max_ms + half_frame_ms;
		double target = m->offset_ms < 0 ? 0 : m->offset_ms > max_ms ? max_ms : m->offset_ms;

		if (!m->locked) {
			/* S11: measure for 5 s, then lock, at once */
			if (!m->measuring_since_ns)
				m->measuring_since_ns = now_ns;
			if (now_ns - m->measuring_since_ns < WINDOW_NS) {
				m->state = SYNC_MEASURING;
				continue;
			}
			m->locked = true;
			out[i].jump = true;
			out[i].changed = true;
			m->jumps++;
			m->delay_ms = out[i].delay_ms = target;
			m->state = over ? SYNC_AT_MAX : SYNC_ADJUSTING; /* locked once the delay has landed */
			continue;
		}
		if (m->relock_since_ns) {
			if (now_ns - m->relock_since_ns < WINDOW_NS) {
				m->state = SYNC_ADJUSTING;
				continue;
			}
			m->relock_since_ns = 0;
			if (fabs(target - m->delay_ms) > half_frame_ms) {
				/* five seconds measured already: no need to wait out the two */
				m->delay_ms = out[i].delay_ms = target;
				out[i].changed = true;
			}
		}

		/* S11: after the lock, move only when it has been more than half a frame off for 2 s, and then ramp */
		if (out[i].changed) {
			/* just moved, above */
		} else if (fabs(target - m->delay_ms) > half_frame_ms) {
			if (!m->off_since_ns) {
				m->off_since_ns = now_ns;
			} else if (now_ns - m->off_since_ns >= OFF_HOLD_NS) {
				m->delay_ms = out[i].delay_ms = target;
				out[i].changed = true;
				m->off_since_ns = 0;
			}
		} else {
			m->off_since_ns = 0;
		}
		/* locked only while on target: off by more than half a frame (waiting out the 2 s, or ramping) is adjusting */
		const bool on_target = !m->off_since_ns && !out[i].changed && in[i].settled;
		m->state = over ? SYNC_AT_MAX : on_target ? SYNC_LOCKED : SYNC_ADJUSTING;
	}
}
