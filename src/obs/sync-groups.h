/*
 * Sync groups in OBS (SPEC S9 to S15): a thread that, ten times a second, gathers every CTD source that names a sync
 * group, runs each group's controller (src/engine/sync-group.c) on their timecode, and hands each member its group
 * delay. The group settings (mode, max delay, fixed latency) live in sync-groups.json in the plugin's config folder;
 * membership is a source setting.
 */

#pragma once

#include "sync-group.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What a source tells the manager each round ("void get_sync(in ptr info)") */
struct ctd_sync_info {
	char group[64];    /* empty: in no group */
	bool engine;       /* running (not paused or idle) */
	bool timecode;     /* locked now */
	bool pinned;       /* has had timecode */
	double tc_seconds; /* at the pin, seconds since midnight */
	uint64_t pin_ns;   /* host clock */
	double fps;
	bool settled; /* its delays have arrived */
	int delay_ms; /* the group delay it holds now */
	char timecode_text[16];
};

/* for the CTD Hub and GetSyncGroups (S15) */
struct ctd_sync_member_view {
	char source[256];
	enum sync_state state;
	char timecode[16];
	double offset_ms;
	double delay_ms;
	bool reference;
};

struct ctd_sync_group_view {
	char name[64];
	enum sync_mode mode;
	int max_delay_ms;
	int fixed_latency_ms;
	int count;
	struct ctd_sync_member_view members[SYNC_MAX_MEMBERS];
};

void sync_groups_start(void);
void sync_groups_stop(void);

/* every group that has members, up to max; returns how many */
size_t sync_groups_snapshot(struct ctd_sync_group_view *out, size_t max);
/* changes a group's settings (a negative value keeps that one); saved at once. mode: 0 align, 1 fixed latency */
bool sync_groups_configure(const char *name, int mode, int max_delay_ms, int fixed_latency_ms);

#ifdef __cplusplus
}
#endif
