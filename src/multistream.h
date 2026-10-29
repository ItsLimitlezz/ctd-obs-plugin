/*
 * CTD Relay client (SPEC section 5.5): the plugin's one connection for everything after OBS.
 *
 * Two threads, started with the module, do all the talking:
 * - the client thread: pairing (H18), the profile list (H19), the chosen profile's status, chat, ingest (H20) and
 *   overlays (H21), its registration on the hub with a heartbeat, messages to other apps such as CTD Overlay (H22), and
 *   the hub's shared timecode clock (API v1.3), and OBS's own events (feeds, outputs, scenes) sent to the hub for its
 *   other apps, such as CTD Live mobile (API v1.4);
 * - the events thread: one Server-Sent Events stream, GET /events?profile=<id>, resumed with Last-Event-ID, which keeps
 *   chat, feed bitrates, destinations and profiles current, and raises destination down/up alerts (H23, H24, H25).
 *
 * It never touches playback: the API being down, slow or refusing only changes what the dock shows (H26). The dock reads
 * a snapshot and asks for things with the ms_* calls; nothing here blocks the caller. Settings (server address, token,
 * chosen profile) live in the plugin's own config file, never in a scene collection, never logged.
 */

#pragma once

#include <obs-data.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MS_DEFAULT_SERVER "192.168.1.50:30080"

enum ms_link {
	MS_DISCONNECTED, /* no token */
	MS_PAIRING,      /* waiting for the user to approve the code in CTD Relay */
	MS_CONNECTED,    /* token accepted */
	MS_ERROR,        /* token refused, or the server can't be reached; see message */
};

/* Asking CTD Overlay for an overlay (H22) */
enum ms_overlay_request {
	MS_OVERLAY_IDLE,
	MS_OVERLAY_SENDING, /* finding CTD Overlay on the hub and sending overlay.create */
	MS_OVERLAY_WAITING, /* sent; waiting for its overlay.created reply */
	MS_OVERLAY_DONE,    /* replied with an overlay: see overlay_created */
	MS_OVERLAY_FAILED,  /* see overlay_message */
};

/* Sending OBS events to the hub (API v1.4) */
enum ms_events {
	MS_EVENTS_IDLE,        /* nothing sent yet */
	MS_EVENTS_OK,          /* the last batch was accepted */
	MS_EVENTS_NEED_SCOPE,  /* the token lacks obs:write: pair again */
	MS_EVENTS_FAILING,     /* the last batch failed; it is retried */
	MS_EVENTS_UNSUPPORTED, /* this CTD Relay has no events endpoint (before API v1.4) */
};

struct ms_snapshot {
	enum ms_link link;
	char *message;              /* one line for the dock, e.g. "Connected" or "Can't reach 192.168.1.50:30080" */
	char *server;               /* the address as the user typed it */
	char *pairing_code;         /* while pairing */
	char *approve_url;          /* while pairing */
	char *profile_id;           /* the chosen profile, or empty */
	bool profile_locked;        /* the token is limited to one profile: no choosing */
	bool events_live;           /* the event stream is connected */
	obs_data_array_t *profiles; /* GET /profiles items, kept current by profile.status / profiles.changed */
	obs_data_t *status;         /* GET /status for the chosen profile, kept current by the events */
	obs_data_array_t *overlays; /* GET /profiles/{id}/overlays items */
	obs_data_t *ingest;         /* the last ingest asked for with ms_request_ingest, or NULL */
	uint64_t ingest_seq;        /* goes up each time a requested ingest arrives (or fails: ingest NULL) */
	obs_data_array_t *chat;     /* recent chat.message envelopes, oldest first, at most 200, moderation applied */
	uint64_t chat_seq;          /* goes up whenever chat changes */
	obs_data_array_t *alerts;   /* recent alerts, newest last: {ts, level ("down", "up", "notice"), text} */
	uint64_t alerts_seq;
	enum ms_overlay_request overlay_request;
	char *overlay_message;       /* the outcome, for the dock */
	obs_data_t *overlay_created; /* the overlay CTD Overlay made (a resource: id, name, url, width, height, meta) */
	uint64_t overlay_seq;        /* goes up on every change of the request */
	enum ms_events events_state;
	long long events_sent;    /* events the hub accepted since OBS started */
	bool share_obs_websocket; /* API v1.6: OBS's WebSocket settings are shared with the hub (the user ticked it) */
	char *obs_websocket_message; /* the outcome of the last share, for the dock */
	uint64_t seq;                /* goes up whenever anything changes */
};

/* The hub's shared clock (GET /clock), synced NTP-style and run locally between samples. */
struct ms_clock {
	bool valid;             /* synced at least once; false while the server has no /clock */
	int64_t now_ms;         /* the hub's time now, Unix epoch ms */
	int64_t run_started_ms; /* when the profile's session went live (epoch ms), or 0 */
	double fps;             /* the timecode frame rate (the landscape feed's nominal fps) */
	int utc_offset_min;     /* the hub's local time zone */
	double delay_ms;        /* round trip of the sample in use: the sync is good to about half of it */
};

/* "HH:MM:SS:FF", non-drop-frame. wrap_day keeps hours below 24 (time of day); otherwise they keep counting (run time). */
void ms_timecode(int64_t ms, double fps, bool wrap_day, char *out, size_t size);

void ms_init(void);
void ms_shutdown(void);

/* A copy of the current state; free it with ms_snapshot_free. */
void ms_snapshot_get(struct ms_snapshot *out);
void ms_snapshot_free(struct ms_snapshot *snapshot);

/* "192.168.1.50:30080", "http://nas.local:30080" or a full ".../api/v1" URL. Changing it disconnects. */
void ms_set_server(const char *server);
/* Asks CTD Relay for a pairing code (H18). */
void ms_start_pairing(void);
/* Forgets the token. */
void ms_disconnect(void);
/* "This OBS goes to profile X" (H19). */
void ms_set_profile(const char *profile_id);
/* Fetches the chosen profile's ingest (stream server and keys); it arrives in the snapshot (ingest_seq). */
void ms_request_ingest(void);
/* Asks the profile's CTD Overlay for a new overlay (overlay.create); the outcome arrives in the snapshot. */
void ms_request_overlay(const char *template_id, const char *canvas);
/* Refreshes everything now instead of at the next poll. */
void ms_refresh(void);
/* The shared clock as of now. Cheap: meant for a readout that redraws every frame. */
void ms_clock_get(struct ms_clock *out);
/* API v1.6: shares OBS's WebSocket settings (password included) with CTD Relay, so CTD Live mobile can control OBS
 * through it; off revokes them. Saved with the connection. */
void ms_set_share_obs_websocket(bool on);
/* Any thread: queues one OBS event for the hub (obs.feed.state, obs.feed.stats, obs.output, obs.scene), stamped
 * with the hub's clock. data is copied. Dropped quietly while not connected or without the obs:write scope. */
void ms_post_event(const char *type, obs_data_t *data);

#ifdef __cplusplus
}
#endif
