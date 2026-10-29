/*
 * The issue engine (SPEC 6.1, D1 to D3): a rules engine over every CTD source's stats and its server profile's poller,
 * once a second, on its own thread. An issue has a severity, the source or path it is about, when it was first and
 * last seen, a plain-language title and the likely cause. It is raised only after its condition has held for the
 * rule's time, there is one per (subject, rule), and it clears once the condition has been gone for 10 s (D3).
 *
 * Raised and cleared issues go to the OBS log, obs-websocket (IssueRaised, IssueCleared) and CTD Relay
 * (obs.issue.raised, obs.issue.cleared); the CTD Hub dock and the sources' stats read the current list.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum issue_severity {
	ISSUE_INFO,
	ISSUE_WARNING,
	ISSUE_CRITICAL,
};

/* D4: one click in the hub, one vendor request (ApplyFix) */
struct issue_action {
	char id[24];     /* "raise-latency", "hardware-decode", "kick", "copy-url", "undo" */
	char label[160]; /* "Raise this source's SRT latency to 400 ms (from 120)" */
};

#define ISSUE_MAX_ACTIONS 3

struct issue {
	char *id;   /* "<rule>|<subject>": stable while it lasts */
	char *rule; /* "camera-offline", "upstream-loss", ... */
	enum issue_severity severity;
	char *source; /* the CTD source it is about, or "" for a path */
	char *server_id;
	char *path;
	char *title;           /* "Upstream packet loss on mob944" */
	char *cause;           /* the likely cause, and what to do */
	int64_t first_seen_ms; /* Unix epoch ms */
	int64_t last_seen_ms;
	struct issue_action actions[ISSUE_MAX_ACTIONS];
	int action_count;
	bool applied_fix; /* not a problem but a fix that was applied, kept so it can be undone (rule "fix-applied") */
};

struct issue_list {
	struct issue *items; /* newest first */
	size_t count;
	uint64_t seq; /* goes up whenever an issue is raised or cleared */
};

void issues_start(void);
void issues_stop(void);

/* A copy of the raised issues; free it with issue_list_free. */
void issues_get(struct issue_list *out);
void issue_list_free(struct issue_list *list);
uint64_t issues_seq(void);

const char *issue_severity_name(enum issue_severity severity);

/*
 * D4: carries out one of an issue's actions (or "undo" on an applied fix). Blocking (a kick talks to the server):
 * call it off the UI thread. message gets what was done, or why not; url gets a publish URL for "copy-url".
 */
bool issues_apply(const char *issue_id, const char *action_id, char *message, size_t message_size, char *url,
		  size_t url_size);
/* H8: kicks the publisher of a path on a profile that allows changes ("" for the default profile); blocks on the API */
bool issues_kick(const char *server_id, const char *path, char *message, size_t message_size);

#ifdef __cplusplus
}
#endif
