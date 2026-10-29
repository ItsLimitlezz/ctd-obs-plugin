/*
 * CTD Relay client. See multistream.h.
 */

#include "multistream.h"
#include "http-client.h"
#include "obsws-config.h"

#include <obs-module.h>
#include <plugin-support.h>
#include <util/bmem.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include <libavformat/avio.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#define NS_PER_S 1000000000ull
#define PROFILES_EVERY_NS (60 * NS_PER_S) /* a safety net: profiles.changed / profile.status keep them current */
#define STATUS_EVERY_NS (30 * NS_PER_S)   /* a safety net: the events keep it current */
#define OVERLAYS_EVERY_NS (15 * NS_PER_S)
#define HEARTBEAT_EVERY_NS (30 * NS_PER_S) /* the hub wants one at least every 60 s */
#define OVERLAY_REPLY_TIMEOUT_NS (30 * NS_PER_S)
#define CHAT_KEEP 200
#define ALERTS_KEEP 30
#define EVENTS_IDLE_TIMEOUT_S 45 /* the server pings every 15 s */
#define EVENT_TYPES "chat.*,destination.*,feed.status,stream.status,profile.*,profiles.changed,app.*,system.*"
/* the shared clock: a burst of samples a second apart, the lowest-delay one wins; a fresh burst every few minutes */
#define CLOCK_SAMPLES 8
#define CLOCK_BURST 6
#define CLOCK_BURST_EVERY_NS (1 * NS_PER_S)
#define CLOCK_EVERY_NS (60 * NS_PER_S) /* between bursts: keeps the session fields current */
#define CLOCK_RESYNC_NS (5 * 60 * NS_PER_S)
#define CLOCK_RETRY_NS (60 * NS_PER_S) /* the server has no /clock (before API v1.3), or it failed */
/* OBS events to the hub (API v1.4): batched, at most one POST a second, the oldest dropped past the cap */
#define EVENTS_PER_POST 50
#define EVENTS_KEEP 500
#define EVENTS_EVERY_NS (1 * NS_PER_S)
/* a scene change or a feed's state goes at once (CTD Relay re-emits on arrival, so the send is the latency), but
 * never sooner than this after the last POST: at most two a second, inside the hub's 120 a minute */
#define EVENTS_URGENT_GAP_NS (NS_PER_S / 2)
#define EVENTS_BACKOFF_NS (10 * NS_PER_S) /* rate limited, or the hub is down */
#define EVENTS_UNSUPPORTED_NS (300 * NS_PER_S) /* a CTD Relay before API v1.4 */
#define MAX_RELEASES 8
#define RELEASE_RETRY_NS (30 * NS_PER_S)
#define RELEASE_GIVE_UP_S (3 * 24 * 3600) /* an old server that hasn't answered in three days is let go */

static struct {
	pthread_mutex_t mutex;
	pthread_t thread;
	bool thread_running;
	pthread_t events_thread;
	bool events_thread_running;
	os_event_t *wake; /* auto-reset */
	volatile bool stopping;
	volatile bool alive;          /* between ms_init and ms_shutdown: the dock may outlive the client by a moment */
	volatile bool events_restart; /* the server, token or profile changed: drop the event stream and open a new one */

	/* everything below is guarded by mutex */
	char *server;
	char *token;
	char *profile_id;
	bool profile_locked;
	enum ms_link link;
	char *message;
	bool token_refused; /* 401: stop polling until the user connects again */

	bool pairing_requested;
	char *pairing_id;
	char *poll_secret;
	char *pairing_code;
	char *approve_url;
	int pairing_interval_s;
	uint64_t pairing_expires_ns;

	obs_data_array_t *profiles;
	obs_data_t *status;
	obs_data_array_t *overlays;
	obs_data_t *ingest;
	uint64_t ingest_seq;
	bool ingest_requested;
	obs_data_array_t *chat;
	uint64_t chat_seq;
	obs_data_array_t *alerts;
	uint64_t alerts_seq;

	/* the event stream */
	char *last_event_id; /* the resume cursor; belongs to profile_id */
	bool events_live;
	bool resync_needed;  /* system.reset, or a stream that couldn't resume: refetch status and chat */
	bool profiles_dirty; /* profiles.changed */
	bool overlays_dirty; /* apps.changed, or CTD Overlay made one */

	/* overlay.create (H22) */
	enum ms_overlay_request overlay_request;
	bool overlay_send;
	char *overlay_template;
	char *overlay_canvas;
	char *overlay_message_id; /* the hub's id of our overlay.create, which the reply names in replyTo */
	uint64_t overlay_deadline_ns;
	char *overlay_message;
	obs_data_t *overlay_created;
	uint64_t overlay_seq;

	/* the shared clock: server epoch ms = local monotonic ms + clock_offset_ms */
	struct {
		double offset_ms;
		double delay_ms;
	} clock_samples[CLOCK_SAMPLES];
	int clock_count;
	int clock_next;
	bool clock_valid;
	double clock_offset_ms;
	double clock_delay_ms;
	int64_t clock_run_started_ms;
	double clock_fps;
	int clock_utc_offset_min;
	bool clock_dirty; /* stream.status: the session started or ended */
	bool clock_reset; /* the server or profile changed: start a new burst */

	/* OBS events to the hub (API v1.4) */
	obs_data_array_t *events; /* envelopes waiting to go, oldest first */
	long long events_boot_ms; /* when the client started, so event ids are unique across restarts */
	unsigned long long events_next_id;
	enum ms_events events_state;
	long long events_sent;
	bool events_need_scope; /* 403 missing_scope: nothing is queued until the user pairs again */
	bool events_register;   /* 404: the app isn't registered (yet), so the heartbeat goes first */
	bool events_not_found;  /* the last batch got a 404: a second one means no events endpoint at all */
	bool events_urgent;     /* an event that shouldn't wait for the next batch is queued */
	/* the id this OBS registers under (saved): only the token that first registered an id may use it, so a new pairing
	 * may need a new one */
	char *app_id;
	bool app_id_renewed; /* already moved to a new id for this token */
	/* Disconnect or a server change: the dropped connection is unregistered on its own server with its own token, after
	 * telling that server to forget OBS's WebSocket settings if it may have them. Saved, oldest first, each tried until
	 * it is answered. */
	struct release {
		char *base;
		char *token;
		char *app_id;
		bool revoke;      /* PUT obs-websocket {enabled:false} before the DELETE */
		int64_t queued_s; /* epoch seconds */
	} releases[MAX_RELEASES];
	size_t release_count;
	/* API v1.6: OBS's WebSocket settings shared with the hub, so CTD Live mobile can control OBS through it. Off
	 * unless the user ticks it in the dock: it hands OBS's WebSocket password to CTD Relay. */
	bool share_obsws;
	bool obsws_on_relay; /* saved: the hub may hold them (shared or tried, and no revoke answered since) */
	bool obsws_dirty;    /* the setting changed: tell the hub now (or tell it to forget) */
	uint64_t obsws_sent; /* the fingerprint of the settings last sent (a change is sent at once) */
	char *obsws_message; /* the outcome, for the dock */

	bool refresh_requested;
	bool reset_requested; /* the server or profile changed: start over */
	uint64_t seq;
} ms;

/* ------------------------------------------------------------- helpers --- */

static void set_string(char **field, const char *value)
{
	bfree(*field);
	*field = value && *value ? bstrdup(value) : NULL;
}

static bool same(const char *a, const char *b)
{
	return a && b && strcmp(a, b) == 0;
}

static void release_array(obs_data_array_t **array)
{
	obs_data_array_release(*array);
	*array = NULL;
}

static void release_data(obs_data_t **data)
{
	obs_data_release(*data);
	*data = NULL;
}

/* a private copy, so a snapshot the dock holds never changes under it */
static obs_data_t *clone_data(obs_data_t *data)
{
	return data ? obs_data_create_from_json(obs_data_get_json(data)) : obs_data_create();
}

/* "192.168.1.50:30080" → "http://192.168.1.50:30080/api/v1" */
static char *api_base(const char *server)
{
	struct dstr url = {0};
	if (!server || !*server)
		return NULL;
	if (strstr(server, "://"))
		dstr_copy(&url, server);
	else
		dstr_printf(&url, "http://%s", server);
	while (url.len && url.array[url.len - 1] == '/')
		dstr_resize(&url, url.len - 1);
	if (url.len < 7 || strcmp(url.array + url.len - 7, "/api/v1") != 0)
		dstr_cat(&url, "/api/v1");
	return url.array;
}

/* profile ids are ULIDs or slugs; anything else is refused rather than put in a URL */
static bool safe_id(const char *id)
{
	if (!id || !*id || strlen(id) > 64)
		return false;
	for (const char *c = id; *c; c++) {
		if (!isalnum((unsigned char)*c) && *c != '-' && *c != '_')
			return false;
	}
	return true;
}

/* this machine's name as [a-z0-9-], for "ctd-obs-plugin-<machine>" (H22) */
static void machine_name(char *out, size_t size)
{
	char host[128] = "";
#ifdef _WIN32
	const char *env = getenv("COMPUTERNAME");
	if (env)
		snprintf(host, sizeof(host), "%s", env);
#else
	if (gethostname(host, sizeof(host) - 1) != 0)
		host[0] = 0;
#endif
	size_t n = 0;
	for (const char *c = host; *c && n + 1 < size; c++) {
		const char lower = (char)tolower((unsigned char)*c);
		if (isalnum((unsigned char)lower))
			out[n++] = lower;
		else if (n && out[n - 1] != '-')
			out[n++] = '-';
	}
	while (n && out[n - 1] == '-')
		n--;
	out[n] = 0;
}

/* the id this OBS registers under: the saved one, or one made from the machine's name */
static void app_id(char *out, size_t size)
{
	pthread_mutex_lock(&ms.mutex);
	const bool saved = ms.app_id && *ms.app_id;
	if (saved)
		snprintf(out, size, "%s", ms.app_id);
	pthread_mutex_unlock(&ms.mutex);
	if (saved)
		return;
	char machine[64];
	machine_name(machine, sizeof(machine));
	snprintf(out, size, "ctd-obs-plugin%s%.25s", *machine ? "-" : "", machine);
}

static void save_settings(void);

/* call with the mutex held: a new id (at most 40 characters, [a-z0-9-]), saved with the connection */
static void renew_app_id(void)
{
	char machine[64];
	machine_name(machine, sizeof(machine));
	char id[48];
	snprintf(id, sizeof(id), "ctd-obs-plugin%s%.18s-%06llx", *machine ? "-" : "", machine,
		 (unsigned long long)(os_gettime_ns() / 1000) & 0xffffffull);
	set_string(&ms.app_id, id);
	ms.app_id_renewed = true;
	save_settings();
}

static int interrupt_cb(void *opaque)
{
	UNUSED_PARAMETER(opaque);
	return os_atomic_load_bool(&ms.stopping);
}

static int events_interrupt_cb(void *opaque)
{
	UNUSED_PARAMETER(opaque);
	return os_atomic_load_bool(&ms.stopping) || os_atomic_load_bool(&ms.events_restart);
}

static enum http_status call(const char *method, const char *url, const char *bearer, const char *body,
			     obs_data_t **reply)
{
	struct http_call c = {
		.method = method,
		.url = url,
		.bearer = bearer,
		.body = body,
		.interrupt = interrupt_cb,
	};
	return http_call_json(&c, reply);
}

static enum http_status get_json(const char *base, const char *path, const char *token, obs_data_t **reply)
{
	struct dstr url = {0};
	dstr_printf(&url, "%s%s", base, path);
	const enum http_status status = call("GET", url.array, token, NULL, reply);
	dstr_free(&url);
	return status;
}

/* ------------------------------------------------------------- settings --- */

static char *config_file(void)
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	return obs_module_config_path("multistream.json");
}

static void load_settings(void)
{
	char *file = config_file();
	obs_data_t *data = file ? obs_data_create_from_json_file_safe(file, "bak") : NULL;
	bfree(file);
	if (!data)
		return;
	set_string(&ms.server, obs_data_get_string(data, "server"));
	set_string(&ms.token, obs_data_get_string(data, "token"));
	set_string(&ms.profile_id, obs_data_get_string(data, "profile_id"));
	ms.profile_locked = obs_data_get_bool(data, "profile_locked");
	set_string(&ms.app_id, obs_data_get_string(data, "app_id"));
	ms.share_obsws = obs_data_get_bool(data, "share_obs_websocket");
	/* settings from before this was saved: if sharing was on, the hub may have them */
	ms.obsws_on_relay = obs_data_has_user_value(data, "obs_websocket_on_relay")
				    ? obs_data_get_bool(data, "obs_websocket_on_relay")
				    : ms.share_obsws;
	obs_data_array_t *releases = obs_data_get_array(data, "releases");
	const size_t count = releases ? obs_data_array_count(releases) : 0;
	for (size_t i = 0; i < count && ms.release_count < MAX_RELEASES; i++) {
		obs_data_t *item = obs_data_array_item(releases, i);
		const char *base = obs_data_get_string(item, "base");
		const char *token = obs_data_get_string(item, "token");
		if (*base && *token) {
			struct release *r = &ms.releases[ms.release_count++];
			r->base = bstrdup(base);
			r->token = bstrdup(token);
			r->app_id = bstrdup(obs_data_get_string(item, "app_id"));
			r->revoke = obs_data_get_bool(item, "revoke");
			r->queued_s = obs_data_get_int(item, "queued_s");
		}
		obs_data_release(item);
	}
	obs_data_array_release(releases);
	obs_data_release(data);
}

/* call with the mutex held */
static void save_settings(void)
{
	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "server", ms.server ? ms.server : "");
	obs_data_set_string(data, "token", ms.token ? ms.token : "");
	obs_data_set_string(data, "profile_id", ms.profile_id ? ms.profile_id : "");
	obs_data_set_bool(data, "profile_locked", ms.profile_locked);
	if (ms.app_id && *ms.app_id)
		obs_data_set_string(data, "app_id", ms.app_id);
	obs_data_set_bool(data, "share_obs_websocket", ms.share_obsws);
	obs_data_set_bool(data, "obs_websocket_on_relay", ms.obsws_on_relay);
	if (ms.release_count) {
		obs_data_array_t *releases = obs_data_array_create();
		for (size_t i = 0; i < ms.release_count; i++) {
			const struct release *r = &ms.releases[i];
			obs_data_t *item = obs_data_create();
			obs_data_set_string(item, "base", r->base);
			obs_data_set_string(item, "token", r->token);
			obs_data_set_string(item, "app_id", r->app_id ? r->app_id : "");
			obs_data_set_bool(item, "revoke", r->revoke);
			obs_data_set_int(item, "queued_s", r->queued_s);
			obs_data_array_push_back(releases, item);
			obs_data_release(item);
		}
		obs_data_set_array(data, "releases", releases);
		obs_data_array_release(releases);
	}
	char *file = config_file();
	if (file && !obs_data_save_json_safe(data, file, "tmp", "bak"))
		blog(LOG_WARNING, "[ctd-live] couldn't save the CTD Relay settings");
	bfree(file);
	obs_data_release(data);
}

/* call with the mutex held */
static void set_link(enum ms_link link, const char *message)
{
	ms.link = link;
	set_string(&ms.message, message);
	ms.seq++;
}

/* call with the mutex held */
static void set_overlay_request(enum ms_overlay_request state, const char *message)
{
	ms.overlay_request = state;
	set_string(&ms.overlay_message, message);
	ms.overlay_seq++;
	ms.seq++;
}

/* call with the mutex held: forget everything that belongs to one server and profile */
static void clear_profile_state(void)
{
	release_data(&ms.status);
	release_array(&ms.overlays);
	release_data(&ms.ingest);
	release_array(&ms.chat);
	release_array(&ms.alerts);
	set_string(&ms.last_event_id, NULL);
	ms.clock_count = ms.clock_next = 0;
	ms.clock_valid = false;
	ms.clock_reset = true;
	release_array(&ms.events); /* they belong to the old server or profile */
	ms.chat_seq++;
	ms.alerts_seq++;
	ms.seq++;
	os_atomic_set_bool(&ms.events_restart, true);
}

static void clear_pairing(void)
{
	ms.pairing_requested = false;
	set_string(&ms.pairing_id, NULL);
	set_string(&ms.poll_secret, NULL);
	set_string(&ms.pairing_code, NULL);
	set_string(&ms.approve_url, NULL);
}

/* call with the mutex held */
static void add_alert(const char *level, const char *text)
{
	obs_data_array_t *alerts = obs_data_array_create();
	const size_t old = ms.alerts ? obs_data_array_count(ms.alerts) : 0;
	for (size_t i = old >= ALERTS_KEEP ? old - ALERTS_KEEP + 1 : 0; i < old; i++) {
		obs_data_t *item = obs_data_array_item(ms.alerts, i);
		obs_data_array_push_back(alerts, item);
		obs_data_release(item);
	}
	obs_data_t *alert = obs_data_create();
	obs_data_set_int(alert, "ts", (long long)(os_gettime_ns() / 1000000));
	obs_data_set_string(alert, "level", level);
	obs_data_set_string(alert, "text", text);
	obs_data_array_push_back(alerts, alert);
	obs_data_release(alert);
	release_array(&ms.alerts);
	ms.alerts = alerts;
	ms.alerts_seq++;
	ms.seq++;
}

/* ------------------------------------------------------------- pairing --- */

static void start_pairing(const char *base)
{
	char machine[64];
	machine_name(machine, sizeof(machine));

	/* obs_data can't hold an array of strings, so the body is written by hand (the machine name is [a-z0-9-]) */
	struct dstr json = {0};
	dstr_printf(&json,
		    "{\"appName\":\"CTD OBS plugin (%s)\",\"kind\":\"ctd-obs-plugin\",\"scopes\":[\"status:read\","
		    "\"chat:read\",\"ingest:read\",\"apps:read\",\"apps:write\",\"obs:write\"],\"profileId\":null}",
		    *machine ? machine : "this PC");
	struct dstr url = {0};
	dstr_printf(&url, "%s/pairing", base);
	obs_data_t *reply = NULL;
	const enum http_status status = call("POST", url.array, NULL, json.array, &reply);
	dstr_free(&url);
	dstr_free(&json);

	pthread_mutex_lock(&ms.mutex);
	ms.pairing_requested = false;
	if (status == HTTP_OK && reply && *obs_data_get_string(reply, "pairingId")) {
		set_string(&ms.pairing_id, obs_data_get_string(reply, "pairingId"));
		set_string(&ms.poll_secret, obs_data_get_string(reply, "pollSecret"));
		set_string(&ms.pairing_code, obs_data_get_string(reply, "code"));
		set_string(&ms.approve_url, obs_data_get_string(reply, "approveUrl"));
		const int interval = (int)obs_data_get_int(reply, "interval");
		ms.pairing_interval_s = interval > 0 && interval < 60 ? interval : 3;
		const long long expires = obs_data_get_int(reply, "expiresIn");
		ms.pairing_expires_ns = os_gettime_ns() + (uint64_t)(expires > 0 ? expires : 600) * NS_PER_S;
		struct dstr message = {0};
		dstr_printf(&message, "Approve code %s in CTD Relay → Settings → API", ms.pairing_code);
		set_link(MS_PAIRING, message.array);
		dstr_free(&message);
		blog(LOG_INFO, "[ctd-live] CTD Relay: pairing started, waiting for approval");
	} else {
		struct dstr message = {0};
		dstr_printf(&message, "Couldn't start pairing: %s", http_status_text(status));
		set_link(MS_ERROR, message.array);
		dstr_free(&message);
	}
	pthread_mutex_unlock(&ms.mutex);
	obs_data_release(reply);
}

static void poll_pairing(const char *base, const char *pairing_id, const char *poll_secret)
{
	struct dstr url = {0};
	dstr_printf(&url, "%s/pairing/%s/token", base, pairing_id);
	obs_data_t *body = obs_data_create();
	obs_data_set_string(body, "pollSecret", poll_secret);
	obs_data_t *reply = NULL;
	const enum http_status status = call("POST", url.array, NULL, obs_data_get_json(body), &reply);
	obs_data_release(body);
	dstr_free(&url);

	pthread_mutex_lock(&ms.mutex);
	if (!same(ms.pairing_id, pairing_id)) {
		/* cancelled meanwhile */
	} else if (status == HTTP_OK && reply && strcmp(obs_data_get_string(reply, "status"), "approved") == 0 &&
		   *obs_data_get_string(reply, "token")) {
		set_string(&ms.token, obs_data_get_string(reply, "token"));
		ms.app_id_renewed = false; /* a new token may need a new id, once */
		const char *profile = obs_data_get_string(reply, "profileId");
		ms.profile_locked = profile && *profile;
		if (ms.profile_locked)
			set_string(&ms.profile_id, profile);
		ms.token_refused = false;
		ms.events_need_scope = false; /* a new token: it may carry obs:write */
		ms.events_state = MS_EVENTS_IDLE;
		clear_pairing();
		clear_profile_state();
		save_settings();
		set_link(MS_CONNECTED, "Connected");
		ms.refresh_requested = true;
		blog(LOG_INFO, "[ctd-live] CTD Relay: connected");
	} else if (status == HTTP_FORBIDDEN) {
		clear_pairing();
		set_link(MS_DISCONNECTED, "The connection was denied in CTD Relay");
	} else if (status == HTTP_OTHER_4XX || status == HTTP_NOT_FOUND || os_gettime_ns() > ms.pairing_expires_ns) {
		clear_pairing();
		set_link(MS_DISCONNECTED, "The code expired: connect again");
	}
	pthread_mutex_unlock(&ms.mutex);
	obs_data_release(reply);
}

/* ------------------------------------------------------------- polling --- */

/* Notes a call's outcome on the link: a refused token stops polling until the user connects again. */
static void note_result(enum http_status status, const char *server)
{
	pthread_mutex_lock(&ms.mutex);
	if (status == HTTP_OK) {
		/* also from the start-up "Connecting…", which is already MS_CONNECTED */
		if (ms.link != MS_CONNECTED || !same(ms.message, "Connected"))
			set_link(MS_CONNECTED, "Connected");
	} else if (status == HTTP_UNAUTHORIZED) {
		if (!ms.token_refused)
			blog(LOG_WARNING, "[ctd-live] CTD Relay refused the token");
		ms.token_refused = true;
		set_link(MS_ERROR, "CTD Relay refused the token: connect again");
	} else if (status == HTTP_UNREACHABLE || status == HTTP_TLS_FAILED) {
		struct dstr message = {0};
		if (status == HTTP_TLS_FAILED)
			dstr_printf(&message, "CTD Relay at %s: %s", server ? server : "", http_status_text(status));
		else
			dstr_printf(&message, "Can't reach CTD Relay at %s", server ? server : "");
		if (ms.link != MS_ERROR || !same(ms.message, message.array))
			set_link(MS_ERROR, message.array);
		dstr_free(&message);
	}
	pthread_mutex_unlock(&ms.mutex);
}

/* a failure that may pass on its own: no answer, a server error, a refused secure connection (a certificate renewed
 * late), an unreadable reply */
static bool transient(enum http_status status)
{
	return status == HTTP_UNREACHABLE || status == HTTP_SERVER_ERROR || status == HTTP_TLS_FAILED ||
	       status == HTTP_BAD_RESPONSE;
}

static void fetch_profiles(const char *base, const char *token, const char *server)
{
	obs_data_t *reply = NULL;
	const enum http_status status = get_json(base, "/profiles", token, &reply);
	note_result(status, server);
	if (status != HTTP_OK || !reply) {
		obs_data_release(reply);
		return;
	}
	obs_data_array_t *items = obs_data_get_array(reply, "items");
	pthread_mutex_lock(&ms.mutex);
	release_array(&ms.profiles);
	ms.profiles = items;
	/* nothing chosen yet: start on the default profile, so there is something to show */
	if (!safe_id(ms.profile_id) && items && obs_data_array_count(items)) {
		const char *pick = NULL;
		for (size_t i = 0; i < obs_data_array_count(items) && !pick; i++) {
			obs_data_t *item = obs_data_array_item(items, i);
			if (strcmp(obs_data_get_string(item, "id"), "default") == 0)
				pick = "default";
			obs_data_release(item);
		}
		obs_data_t *first = obs_data_array_item(items, 0);
		set_string(&ms.profile_id, pick ? pick : obs_data_get_string(first, "id"));
		obs_data_release(first);
		save_settings();
		ms.resync_needed = true;
		os_atomic_set_bool(&ms.events_restart, true);
	}
	ms.seq++;
	pthread_mutex_unlock(&ms.mutex);
	obs_data_release(reply);
}

static void fetch_status(const char *base, const char *token, const char *profile, const char *server)
{
	struct dstr path = {0};
	dstr_printf(&path, "/status?profile=%s", profile);
	obs_data_t *reply = NULL;
	const enum http_status status = get_json(base, path.array, token, &reply);
	dstr_free(&path);
	note_result(status, server);
	pthread_mutex_lock(&ms.mutex);
	if (status == HTTP_OK && same(ms.profile_id, profile)) {
		release_data(&ms.status);
		ms.status = reply;
		reply = NULL;
		ms.seq++;
	}
	pthread_mutex_unlock(&ms.mutex);
	obs_data_release(reply);
}

static void fetch_overlays(const char *base, const char *token, const char *profile)
{
	struct dstr path = {0};
	dstr_printf(&path, "/profiles/%s/overlays", profile);
	obs_data_t *reply = NULL;
	const enum http_status status = get_json(base, path.array, token, &reply);
	dstr_free(&path);
	if (status != HTTP_OK || !reply) {
		obs_data_release(reply);
		return;
	}
	obs_data_array_t *items = obs_data_get_array(reply, "items");
	pthread_mutex_lock(&ms.mutex);
	if (same(ms.profile_id, profile)) {
		release_array(&ms.overlays);
		ms.overlays = items;
		items = NULL;
		ms.seq++;
	}
	pthread_mutex_unlock(&ms.mutex);
	obs_data_array_release(items);
	obs_data_release(reply);
}

static void fetch_ingest(const char *base, const char *token, const char *profile)
{
	struct dstr path = {0};
	dstr_printf(&path, "/profiles/%s/ingest", profile);
	obs_data_t *reply = NULL;
	const enum http_status status = get_json(base, path.array, token, &reply);
	dstr_free(&path);
	pthread_mutex_lock(&ms.mutex);
	release_data(&ms.ingest);
	if (status == HTTP_OK) {
		ms.ingest = reply;
		reply = NULL;
	} else if (status == HTTP_FORBIDDEN) {
		set_string(&ms.message, "This connection may not read stream keys (it needs the ingest:read scope)");
	}
	ms.ingest_seq++;
	ms.seq++;
	pthread_mutex_unlock(&ms.mutex);
	obs_data_release(reply);
}

/* the whole recent chat (on connect, a profile change, or system.reset); the events add to it from then on */
static void fetch_chat(const char *base, const char *token, const char *profile)
{
	struct dstr path = {0};
	dstr_printf(&path, "/chat?profile=%s&limit=%d", profile, CHAT_KEEP);
	obs_data_t *reply = NULL;
	const enum http_status status = get_json(base, path.array, token, &reply);
	dstr_free(&path);
	if (status != HTTP_OK || !reply) {
		obs_data_release(reply);
		return;
	}
	obs_data_array_t *items = obs_data_get_array(reply, "items");
	pthread_mutex_lock(&ms.mutex);
	if (same(ms.profile_id, profile)) {
		release_array(&ms.chat);
		ms.chat = items ? items : obs_data_array_create();
		items = NULL;
		ms.chat_seq++;
		ms.seq++;
	}
	pthread_mutex_unlock(&ms.mutex);
	obs_data_array_release(items);
	obs_data_release(reply);
}

static double mono_ms(void)
{
	return (double)os_gettime_ns() / 1e6;
}

static long long epoch_ms(void)
{
	struct timespec ts;
	if (timespec_get(&ts, TIME_UTC) != TIME_UTC)
		return 0;
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* One sample of the hub's clock (API v1.3). The sums run on the monotonic clock, so a jump in this PC's wall clock
 * can't skew them; t0 goes out as epoch ms only because that is what the API echoes. Returns false if it failed. */
static bool fetch_clock(const char *base, const char *token, const char *profile)
{
	struct dstr path = {0};
	dstr_printf(&path, "/clock?profile=%s&t0=%lld", profile, epoch_ms());
	obs_data_t *reply = NULL;
	const double t0 = mono_ms();
	const enum http_status status = get_json(base, path.array, token, &reply);
	const double t3 = mono_ms();
	dstr_free(&path);
	const double received = reply ? (double)obs_data_get_int(reply, "receivedAt") : 0;
	const double sent = reply ? (double)obs_data_get_int(reply, "sentAt") : 0;
	if (status != HTTP_OK || received <= 0 || sent < received) {
		pthread_mutex_lock(&ms.mutex);
		if (status == HTTP_NOT_FOUND && ms.clock_valid) {
			ms.clock_valid = false; /* the server went back to a version without /clock */
			ms.seq++;
		}
		pthread_mutex_unlock(&ms.mutex);
		obs_data_release(reply);
		return false;
	}

	const double offset = ((received - t0) + (sent - t3)) / 2.0;
	const double delay = (t3 - t0) - (sent - received);
	obs_data_t *session = obs_data_get_obj(reply, "session");

	pthread_mutex_lock(&ms.mutex);
	if (same(ms.profile_id, profile)) {
		ms.clock_samples[ms.clock_next].offset_ms = offset;
		ms.clock_samples[ms.clock_next].delay_ms = delay;
		ms.clock_next = (ms.clock_next + 1) % CLOCK_SAMPLES;
		if (ms.clock_count < CLOCK_SAMPLES)
			ms.clock_count++;
		int best = 0;
		for (int i = 1; i < ms.clock_count; i++) {
			if (ms.clock_samples[i].delay_ms < ms.clock_samples[best].delay_ms)
				best = i;
		}
		ms.clock_offset_ms = ms.clock_samples[best].offset_ms;
		ms.clock_delay_ms = ms.clock_samples[best].delay_ms;
		ms.clock_run_started_ms = session ? (int64_t)obs_data_get_int(session, "startedAt") : 0;
		const double fps = obs_data_get_double(reply, "fps");
		ms.clock_fps = fps > 0 && fps <= 240 ? fps : 30;
		ms.clock_utc_offset_min = (int)obs_data_get_int(reply, "utcOffsetMinutes");
		if (!ms.clock_valid)
			blog(LOG_INFO, "[ctd-live] CTD Relay: clock synced (round trip %.1f ms)", delay);
		ms.clock_valid = true;
		ms.seq++;
	}
	pthread_mutex_unlock(&ms.mutex);
	obs_data_release(session);
	obs_data_release(reply);
	return true;
}

/* H22: registers this OBS on the hub, so other apps (CTD Overlay) can find it and message it. Only the token that first
 * registered an id may use it: refused with a 409 (which FFmpeg reports as "another 4xx"; a 429 has its own code), the
 * id belongs to an earlier pairing, so this OBS moves to a new one, once per token. Returns true when registered. */
static bool heartbeat(const char *base, const char *token)
{
	char machine[64];
	machine_name(machine, sizeof(machine));
	char id[48];
	app_id(id, sizeof(id));
	struct dstr body = {0};
	dstr_printf(&body,
		    "{\"name\":\"CTD OBS plugin (%s)\",\"kind\":\"ctd-obs-plugin\",\"version\":\"%s\","
		    "\"capabilities\":[\"obs.ingest\",\"obs.overlays\"],\"resources\":[]}",
		    *machine ? machine : "this PC", PLUGIN_VERSION);
	struct dstr url = {0};
	dstr_printf(&url, "%s/apps/%s", base, id);
	/* best effort: without apps:write only messaging is lost */
	const enum http_status status = call("PUT", url.array, token, body.array, NULL);
	dstr_free(&url);
	dstr_free(&body);
	if (status != HTTP_OTHER_4XX)
		return status == HTTP_OK;
	pthread_mutex_lock(&ms.mutex);
	if (!ms.app_id_renewed) {
		renew_app_id();
		ms.events_register = true; /* register under it at the next round */
		blog(LOG_INFO, "[ctd-live] CTD Relay: \"%s\" belongs to an earlier connection; registering as \"%s\"",
		     id, ms.app_id);
	}
	pthread_mutex_unlock(&ms.mutex);
	return false;
}

/*
 * API v1.6: PUT /apps/{appId}/obs-websocket. share: OBS's WebSocket settings (port, whether it asks for a password, the
 * password itself) and this machine's LAN address, read now. Not share: tells the hub to forget the password and stop
 * connecting (enabled false, no auth). The password never reaches a log; this plugin's own copies of it are wiped
 * (libobs's and FFmpeg's are beyond reach, and obs-websocket keeps it in memory anyway). Returns the seconds until the
 * next try: soon after a failure that may pass, later after one that won't (or a success: the 5-minute refresh).
 */
static uint64_t send_obs_websocket(const char *base, const char *token, const char *server, bool share)
{
	struct obsws_config ws;
	obsws_config_read(&ws);
	if (share && !ws.found) {
		pthread_mutex_lock(&ms.mutex);
		set_string(&ms.obsws_message,
			   "OBS's WebSocket settings weren't found (obs-websocket, OBS 28 or later)");
		ms.seq++;
		pthread_mutex_unlock(&ms.mutex);
		obsws_config_free(&ws);
		return 300;
	}
	/* by hand, not obs_data: it has no array of strings, and its copy of the password would be one more */
	struct dstr json = {0};
	dstr_reserve(&json, 256 + (ws.password ? strlen(ws.password) * 6 : 0)); /* never grown, so never copied */
	dstr_printf(&json, "{\"enabled\":%s,\"port\":%d,\"authRequired\":%s", share && ws.enabled ? "true" : "false",
		    ws.port, share && ws.auth_required ? "true" : "false");
	if (share && ws.auth_required) {
		dstr_cat(&json, ",\"password\":\"");
		for (const unsigned char *c = (const unsigned char *)ws.password; *c; c++) {
			if (*c == '"' || *c == '\\')
				dstr_catf(&json, "\\%c", *c);
			else if (*c < 0x20)
				dstr_catf(&json, "\\u%04x", *c);
			else
				dstr_cat_ch(&json, (char)*c);
		}
		dstr_cat_ch(&json, '"');
	}
	char address[64];
	if (share && obsws_local_address(server, address, sizeof(address)))
		dstr_catf(&json, ",\"addresses\":[\"%s\"]", address);
	dstr_cat_ch(&json, '}');

	char id[48];
	app_id(id, sizeof(id));
	struct dstr url = {0};
	dstr_printf(&url, "%s/apps/%s/obs-websocket", base, id);
	obs_data_t *reply = NULL;
	const enum http_status status = call("PUT", url.array, token, json.array, &reply);
	dstr_free(&url);
	memset(json.array, 0, json.len); /* it held the password */
	dstr_free(&json);

	/* a failure that may pass is tried again soon. This is only sent once registered, so any other answer to a revoke
	 * settles it (an older hub, or a token that can't: neither holds the settings). A share that got no answer may
	 * have arrived, so a revoke is owed for it as well. */
	const bool passing = transient(status);
	uint64_t next_s = passing ? 30 : 300;
	pthread_mutex_lock(&ms.mutex);
	if (!same(ms.token, token)) { /* disconnected meanwhile: this answer is about a connection that's gone */
		pthread_mutex_unlock(&ms.mutex);
		obs_data_release(reply);
		obsws_config_free(&ws);
		return 30;
	}
	const bool was_on_relay = ms.obsws_on_relay;
	struct dstr message = {0};
	if (share)
		ms.obsws_sent = ws.fingerprint; /* failures are retried on the clock, not on every check */
	if (passing || (share && status == HTTP_OK))
		ms.obsws_on_relay = true;
	if (status == HTTP_OK && share) {
		dstr_printf(&message, "Shared with CTD Relay: OBS at %s:%lld%s", obs_data_get_string(reply, "host"),
			    obs_data_get_int(reply, "port"), ws.enabled ? "" : " (OBS's WebSocket server is off)");
	} else if (!share && !passing) {
		dstr_copy(&message,
			  status == HTTP_OK ? "Not shared: CTD Relay was told to forget OBS's password" : "Not shared");
		ms.obsws_sent = 0;
		ms.obsws_on_relay = false;
	} else if (!share) {
		dstr_printf(&message, "Couldn't tell CTD Relay to forget OBS's password (%s); trying again",
			    http_status_text(status));
	} else if (status == HTTP_NOT_FOUND) {
		/* not registered (the next registration sends this again), or a CTD Relay before v1.6 */
		dstr_copy(&message, "Waiting for CTD Relay (it needs API v1.6 for this)");
	} else if (status == HTTP_FORBIDDEN) {
		dstr_copy(&message, "This connection may not do this (obs:write): disconnect and connect again");
	} else {
		dstr_printf(&message, "Couldn't share with CTD Relay (%s); trying again", http_status_text(status));
	}
	if (ms.obsws_on_relay != was_on_relay)
		save_settings();
	set_string(&ms.obsws_message, message.array);
	ms.seq++;
	pthread_mutex_unlock(&ms.mutex);
	dstr_free(&message);
	obs_data_release(reply);
	obsws_config_free(&ws);
	return next_s;
}

/* call with the mutex held: wipes the token */
static void free_release(struct release *r)
{
	if (r->token)
		memset(r->token, 0, strlen(r->token));
	bfree(r->base);
	bfree(r->token);
	bfree(r->app_id);
	memset(r, 0, sizeof(*r));
}

/* call with the mutex held: removes releases[i], keeping the order */
static void drop_release(size_t i)
{
	free_release(&ms.releases[i]);
	memmove(&ms.releases[i], &ms.releases[i + 1], (ms.release_count - i - 1) * sizeof(ms.releases[0]));
	ms.release_count--;
	memset(&ms.releases[ms.release_count], 0, sizeof(ms.releases[0]));
}

/* call with the mutex held: the connection is being dropped (Disconnect, or a new server), so queue its unregistering,
 * on its own server, with its own token and id. What the hub may hold stays owed (obsws_on_relay) for the next
 * connection too: unregistering alone may not make every hub forget, and the next one may be the same hub. */
static void queue_release(void)
{
	if (!ms.token || !*ms.token)
		return;
	char *base = api_base(ms.server);
	if (!base)
		return;
	for (size_t i = 0; i < ms.release_count; i++) {
		if (same(ms.releases[i].base, base) && same(ms.releases[i].token, ms.token)) {
			bfree(base);
			return;
		}
	}
	if (ms.release_count == MAX_RELEASES) {
		blog(LOG_WARNING,
		     "[ctd-live] CTD Relay: too many old connections never answered; forgetting the oldest");
		drop_release(0);
	}
	char id[48];
	if (ms.app_id && *ms.app_id) {
		snprintf(id, sizeof(id), "%s", ms.app_id);
	} else {
		char machine[64];
		machine_name(machine, sizeof(machine));
		snprintf(id, sizeof(id), "ctd-obs-plugin%s%.25s", *machine ? "-" : "", machine);
	}
	struct release *r = &ms.releases[ms.release_count++];
	r->base = base;
	r->token = bstrdup(ms.token);
	r->app_id = bstrdup(id);
	r->revoke = ms.share_obsws || ms.obsws_on_relay;
	r->queued_s = epoch_ms() / 1000;
}

/*
 * The oldest dropped connection (see queue_release): tells its server to forget OBS's WebSocket settings (if it may
 * have them), then unregisters this OBS there, so the next pairing can use the same id. Any answer settles a step; with
 * none (the server down, a refused secure connection, OBS closing) it stays saved and is tried again, for up to three
 * days. Returns false when that try failed (wait before the next).
 */
static bool release_app(void)
{
	pthread_mutex_lock(&ms.mutex);
	if (!ms.release_count) {
		pthread_mutex_unlock(&ms.mutex);
		return true;
	}
	struct release r = {
		.base = bstrdup(ms.releases[0].base),
		.token = bstrdup(ms.releases[0].token),
		.app_id = bstrdup(ms.releases[0].app_id),
		.revoke = ms.releases[0].revoke,
		.queued_s = ms.releases[0].queued_s,
	};
	pthread_mutex_unlock(&ms.mutex);

	struct dstr url = {0};
	enum http_status status = HTTP_OK;
	if (r.revoke && r.app_id && *r.app_id) {
		dstr_printf(&url, "%s/apps/%s/obs-websocket", r.base, r.app_id);
		status = call("PUT", url.array, r.token, "{\"enabled\":false,\"port\":4455,\"authRequired\":false}",
			      NULL);
	}
	const bool revoked = !transient(status);
	if (revoked && r.app_id && *r.app_id) {
		dstr_printf(&url, "%s/apps/%s", r.base, r.app_id);
		status = call("DELETE", url.array, r.token, NULL, NULL);
	}
	dstr_free(&url);
	const bool answered = revoked && !transient(status);
	const bool expired = !answered && epoch_ms() / 1000 - r.queued_s > RELEASE_GIVE_UP_S;

	pthread_mutex_lock(&ms.mutex);
	for (size_t i = 0; i < ms.release_count; i++) {
		struct release *q = &ms.releases[i];
		if (!same(q->base, r.base) || !same(q->token, r.token))
			continue;
		if (answered || expired) {
			if (expired)
				blog(LOG_WARNING,
				     "[ctd-live] CTD Relay at %s never answered in three days; it may still list this OBS",
				     r.base);
			drop_release(i);
			save_settings();
		} else if (revoked && q->revoke) {
			q->revoke = false; /* that part is done */
			save_settings();
		}
		break;
	}
	free_release(&r);
	pthread_mutex_unlock(&ms.mutex);
	return answered || expired;
}

/* call with the mutex held: puts a failed batch back in front of anything queued since, within the cap */
static void requeue_events(obs_data_array_t *batch)
{
	obs_data_array_t *events = obs_data_array_create();
	const size_t queued = ms.events ? obs_data_array_count(ms.events) : 0;
	const size_t room = EVENTS_KEEP > queued ? EVENTS_KEEP - queued : 0;
	const size_t count = obs_data_array_count(batch);
	for (size_t i = count > room ? count - room : 0; i < count; i++) {
		obs_data_t *item = obs_data_array_item(batch, i);
		obs_data_array_push_back(events, item);
		obs_data_release(item);
	}
	for (size_t i = 0; i < queued; i++) {
		obs_data_t *item = obs_data_array_item(ms.events, i);
		obs_data_array_push_back(events, item);
		obs_data_release(item);
	}
	release_array(&ms.events);
	ms.events = events;
}

/*
 * API v1.4: sends up to EVENTS_PER_POST queued OBS events to POST /apps/{appId}/events. Returns how long to wait before
 * the next batch. The hub re-emits them on /events for its other apps (CTD Live mobile).
 */
static uint64_t send_events(const char *base, const char *token, const char *profile)
{
	pthread_mutex_lock(&ms.mutex);
	const size_t queued = ms.events ? obs_data_array_count(ms.events) : 0;
	if (!queued || ms.events_need_scope) {
		pthread_mutex_unlock(&ms.mutex);
		return EVENTS_EVERY_NS;
	}
	obs_data_array_t *batch = obs_data_array_create();
	obs_data_array_t *rest = obs_data_array_create();
	for (size_t i = 0; i < queued; i++) {
		obs_data_t *item = obs_data_array_item(ms.events, i);
		obs_data_array_push_back(i < EVENTS_PER_POST ? batch : rest, item);
		obs_data_release(item);
	}
	release_array(&ms.events);
	ms.events = rest;
	pthread_mutex_unlock(&ms.mutex);

	char id[48];
	app_id(id, sizeof(id));
	obs_data_t *body = obs_data_create();
	obs_data_set_array(body, "events", batch);
	struct dstr url = {0};
	dstr_printf(&url, "%s/apps/%s/events?profile=%s", base, id, profile);
	obs_data_t *reply = NULL;
	const enum http_status status = call("POST", url.array, token, obs_data_get_json(body), &reply);
	dstr_free(&url);
	obs_data_release(body);

	uint64_t wait = EVENTS_EVERY_NS;
	pthread_mutex_lock(&ms.mutex);
	const enum ms_events before = ms.events_state;
	if (status == HTTP_OK) {
		ms.events_sent += reply ? obs_data_get_int(reply, "accepted") : 0;
		ms.events_state = MS_EVENTS_OK;
		if (before != MS_EVENTS_OK)
			blog(LOG_INFO, "[ctd-live] CTD Relay: sending OBS events");
	} else if (status == HTTP_FORBIDDEN) {
		/* the token predates obs:write: stop until the user pairs again */
		ms.events_need_scope = true;
		ms.events_state = MS_EVENTS_NEED_SCOPE;
		release_array(&ms.events);
		blog(LOG_WARNING, "[ctd-live] CTD Relay: this connection may not send OBS events (obs:write); "
				  "pair again to send them");
	} else if (status == HTTP_BAD_REQUEST || status == HTTP_BAD_RESPONSE) {
		ms.events_state = MS_EVENTS_FAILING; /* this batch can't be fixed by sending it again */
	} else if (status == HTTP_NOT_FOUND && ms.events_not_found) {
		/* 404 again after registering: this CTD Relay has no events endpoint (before API v1.4). The backlog
		 * would only be stale by the time it does; ask again now and then. */
		ms.events_state = MS_EVENTS_UNSUPPORTED;
		release_array(&ms.events);
		wait = EVENTS_UNSUPPORTED_NS;
		if (before != MS_EVENTS_UNSUPPORTED)
			blog(LOG_INFO, "[ctd-live] CTD Relay doesn't take OBS events yet (it needs API v1.4); "
				       "asking again every 5 minutes");
	} else {
		/* not registered yet (404), registered by an earlier pairing (409), rate limited, or the hub is down: the
		 * same batch again, later */
		if (status == HTTP_NOT_FOUND) {
			ms.events_register = true;
			ms.events_not_found = true;
		} else {
			if (status == HTTP_OTHER_4XX)
				ms.events_register =
					true; /* the heartbeat finds out whether the id is someone else's */
			wait = EVENTS_BACKOFF_NS;
		}
		requeue_events(batch);
		ms.events_state = MS_EVENTS_FAILING;
		if (before != MS_EVENTS_FAILING)
			blog(LOG_INFO, "[ctd-live] CTD Relay: couldn't send OBS events (%s); retrying",
			     http_status_text(status));
	}
	if (status != HTTP_NOT_FOUND)
		ms.events_not_found = false;
	if (ms.events_state != before || status == HTTP_OK)
		ms.seq++; /* the dock shows the count */
	pthread_mutex_unlock(&ms.mutex);
	obs_data_array_release(batch);
	obs_data_release(reply);
	return wait;
}

/* H22: find the profile's CTD Overlay on the hub and send it overlay.create; its reply comes as an app.message */
static void send_overlay_request(const char *base, const char *token, const char *profile, const char *template_id,
				 const char *canvas)
{
	struct dstr path = {0};
	dstr_printf(&path, "/apps?profile=%s", profile);
	obs_data_t *reply = NULL;
	enum http_status status = get_json(base, path.array, token, &reply);
	dstr_free(&path);

	char *to = NULL;
	obs_data_array_t *apps = reply ? obs_data_get_array(reply, "items") : NULL;
	for (size_t i = 0; apps && i < obs_data_array_count(apps); i++) {
		obs_data_t *app = obs_data_array_item(apps, i);
		/* an online CTD Overlay connection first, any CTD Overlay connection otherwise */
		if (strcmp(obs_data_get_string(app, "kind"), "ctd-overlay") == 0 &&
		    (!to || obs_data_get_bool(app, "online"))) {
			bfree(to);
			to = bstrdup(obs_data_get_string(app, "id"));
		}
		obs_data_release(app);
	}
	obs_data_array_release(apps);
	obs_data_release(reply);
	reply = NULL;

	if (status != HTTP_OK || !to) {
		pthread_mutex_lock(&ms.mutex);
		set_overlay_request(MS_OVERLAY_FAILED,
				    status == HTTP_OK ? "CTD Overlay isn't connected to this profile on the hub"
						      : "Couldn't look up CTD Overlay on the hub");
		pthread_mutex_unlock(&ms.mutex);
		bfree(to);
		return;
	}

	obs_data_t *message = obs_data_create();
	obs_data_set_string(message, "to", to);
	obs_data_set_string(message, "type", "overlay.create");
	obs_data_set_string(message, "profileId", profile);
	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "template", template_id);
	obs_data_set_string(data, "canvas", canvas);
	obs_data_set_obj(message, "data", data);
	obs_data_release(data);
	struct dstr url = {0};
	dstr_printf(&url, "%s/messages", base);
	status = call("POST", url.array, token, obs_data_get_json(message), &reply);
	dstr_free(&url);
	obs_data_release(message);

	pthread_mutex_lock(&ms.mutex);
	if (status == HTTP_OK && reply && *obs_data_get_string(reply, "id")) {
		set_string(&ms.overlay_message_id, obs_data_get_string(reply, "id"));
		ms.overlay_deadline_ns = os_gettime_ns() + OVERLAY_REPLY_TIMEOUT_NS;
		set_overlay_request(MS_OVERLAY_WAITING, "Asked CTD Overlay; waiting for its overlay…");
		blog(LOG_INFO, "[ctd-live] CTD Relay: asked %s for a '%s' overlay (%s)", to, template_id, canvas);
	} else {
		struct dstr text = {0};
		dstr_printf(&text, "Couldn't send the request to CTD Overlay: %s",
			    status == HTTP_FORBIDDEN ? "this connection may not message other apps (apps:write)"
						     : http_status_text(status));
		set_overlay_request(MS_OVERLAY_FAILED, text.array);
		dstr_free(&text);
	}
	pthread_mutex_unlock(&ms.mutex);
	obs_data_release(reply);
	bfree(to);
}

/* ------------------------------------------------------------- events --- */

/* call with the mutex held: status with one array item replaced (matched on key) or added */
static void update_status_item(const char *array_name, const char *key, obs_data_t *item)
{
	obs_data_t *status = clone_data(ms.status);
	obs_data_array_t *array = obs_data_get_array(status, array_name);
	if (!array) {
		array = obs_data_array_create();
		obs_data_set_array(status, array_name, array);
	}
	const char *wanted = obs_data_get_string(item, key);
	bool replaced = false;
	for (size_t i = 0; i < obs_data_array_count(array) && !replaced; i++) {
		obs_data_t *existing = obs_data_array_item(array, i);
		if (strcmp(obs_data_get_string(existing, key), wanted) == 0) {
			obs_data_array_erase(array, i);
			obs_data_array_insert(array, i, item);
			replaced = true;
		}
		obs_data_release(existing);
	}
	if (!replaced)
		obs_data_array_push_back(array, item);
	obs_data_array_release(array);
	release_data(&ms.status);
	ms.status = status;
	ms.seq++;
}

/* call with the mutex held: chat without the messages a moderation event removed */
static void remove_chat(bool (*matches)(obs_data_t *data, obs_data_t *event), obs_data_t *event)
{
	if (!ms.chat)
		return;
	obs_data_array_t *kept = obs_data_array_create();
	bool removed = false;
	for (size_t i = 0; i < obs_data_array_count(ms.chat); i++) {
		obs_data_t *envelope = obs_data_array_item(ms.chat, i);
		obs_data_t *data = obs_data_get_obj(envelope, "data");
		if (data && matches(data, event))
			removed = true;
		else
			obs_data_array_push_back(kept, envelope);
		obs_data_release(data);
		obs_data_release(envelope);
	}
	if (removed) {
		release_array(&ms.chat);
		ms.chat = kept;
		ms.chat_seq++;
		ms.seq++;
	} else {
		obs_data_array_release(kept);
	}
}

static bool is_deleted_message(obs_data_t *data, obs_data_t *event)
{
	return strcmp(obs_data_get_string(data, "id"), obs_data_get_string(event, "messageId")) == 0;
}

static bool is_cleared_user(obs_data_t *data, obs_data_t *event)
{
	obs_data_t *user = obs_data_get_obj(data, "user");
	const char *user_id = obs_data_get_string(event, "userId");
	const char *user_name = obs_data_get_string(event, "userName");
	bool match = false;
	if (*user_id)
		match = strcmp(obs_data_get_string(user, "id"), user_id) == 0;
	else if (*user_name)
		match = strcmp(obs_data_get_string(user, "name"), user_name) == 0 &&
			strcmp(obs_data_get_string(data, "platform"), obs_data_get_string(event, "platform")) == 0;
	obs_data_release(user);
	return match;
}

static bool is_cleared_platform(obs_data_t *data, obs_data_t *event)
{
	const char *destination = obs_data_get_string(event, "destinationId");
	if (*destination)
		return strcmp(obs_data_get_string(data, "destinationId"), destination) == 0;
	return strcmp(obs_data_get_string(data, "platform"), obs_data_get_string(event, "platform")) == 0;
}

/* call with the mutex held */
static void add_chat(obs_data_t *envelope)
{
	obs_data_array_t *chat = obs_data_array_create();
	const size_t old = ms.chat ? obs_data_array_count(ms.chat) : 0;
	for (size_t i = old >= CHAT_KEEP ? old - CHAT_KEEP + 1 : 0; i < old; i++) {
		obs_data_t *item = obs_data_array_item(ms.chat, i);
		obs_data_array_push_back(chat, item);
		obs_data_release(item);
	}
	obs_data_array_push_back(chat, envelope);
	release_array(&ms.chat);
	ms.chat = chat;
	ms.chat_seq++;
	ms.seq++;
}

/* call with the mutex held */
static void update_profile(obs_data_t *profile)
{
	if (!ms.profiles)
		return;
	obs_data_array_t *profiles = obs_data_array_create();
	for (size_t i = 0; i < obs_data_array_count(ms.profiles); i++) {
		obs_data_t *item = obs_data_array_item(ms.profiles, i);
		const bool match = strcmp(obs_data_get_string(item, "id"), obs_data_get_string(profile, "id")) == 0;
		obs_data_array_push_back(profiles, match ? profile : item);
		obs_data_release(item);
	}
	release_array(&ms.profiles);
	ms.profiles = profiles;
	ms.seq++;
}

/* call with the mutex held: CTD Overlay's reply to our overlay.create (H22) */
static void handle_app_message(obs_data_t *message)
{
	if (ms.overlay_request != MS_OVERLAY_WAITING ||
	    !same(obs_data_get_string(message, "replyTo"), ms.overlay_message_id) ||
	    strcmp(obs_data_get_string(message, "type"), "overlay.created") != 0)
		return;
	obs_data_t *reply = obs_data_get_obj(message, "data");
	obs_data_t *resource = reply ? obs_data_get_obj(reply, "resource") : NULL;
	if (reply && obs_data_get_bool(reply, "ok") && resource) {
		release_data(&ms.overlay_created);
		ms.overlay_created = resource;
		resource = NULL;
		struct dstr text = {0};
		dstr_printf(&text, "CTD Overlay made \"%s\"", obs_data_get_string(ms.overlay_created, "name"));
		set_overlay_request(MS_OVERLAY_DONE, text.array);
		dstr_free(&text);
		ms.overlays_dirty = true;
	} else {
		const char *why = reply ? obs_data_get_string(reply, "error") : "";
		struct dstr text = {0};
		dstr_printf(&text, "CTD Overlay couldn't make it%s%s", *why ? ": " : "", why);
		set_overlay_request(MS_OVERLAY_FAILED, text.array);
		dstr_free(&text);
	}
	obs_data_release(resource);
	obs_data_release(reply);
}

static void handle_event(const char *type, obs_data_t *envelope)
{
	obs_data_t *data = obs_data_get_obj(envelope, "data");
	if (!data)
		data = obs_data_create();
	pthread_mutex_lock(&ms.mutex);

	if (strcmp(type, "chat.message") == 0) {
		add_chat(envelope);
	} else if (strcmp(type, "chat.message.deleted") == 0) {
		remove_chat(is_deleted_message, data);
	} else if (strcmp(type, "chat.user.cleared") == 0) {
		remove_chat(is_cleared_user, data);
	} else if (strcmp(type, "chat.cleared") == 0) {
		remove_chat(is_cleared_platform, data);
	} else if (strcmp(type, "stream.status") == 0) {
		obs_data_t *status = clone_data(ms.status);
		obs_data_set_obj(status, "session", data);
		release_data(&ms.status);
		ms.status = status;
		ms.clock_dirty = true; /* the run timecode starts from the session's startedAt */
		ms.seq++;
	} else if (strcmp(type, "feed.status") == 0) {
		update_status_item("feeds", "feed", data);
	} else if (strcmp(type, "destination.status") == 0) {
		update_status_item("destinations", "id", data);
	} else if (strcmp(type, "destination.down") == 0) {
		/* H25: "Instagram went down, recovering: Instagram ended the live (copyright)…" */
		struct dstr text = {0};
		dstr_printf(&text, "%s went down", obs_data_get_string(data, "name"));
		if (*obs_data_get_string(data, "phase"))
			dstr_catf(&text, ", %s", obs_data_get_string(data, "phase"));
		if (*obs_data_get_string(data, "reason"))
			dstr_catf(&text, ": %s", obs_data_get_string(data, "reason"));
		add_alert("down", text.array);
		blog(LOG_WARNING, "[ctd-live] CTD Relay: %s", text.array);
		dstr_free(&text);
	} else if (strcmp(type, "destination.up") == 0) {
		struct dstr text = {0};
		dstr_printf(&text, "%s is back", obs_data_get_string(data, "name"));
		const long long down_for = obs_data_get_int(data, "downForSec");
		if (down_for > 0)
			dstr_catf(&text, " after %lld s", down_for);
		add_alert("up", text.array);
		blog(LOG_INFO, "[ctd-live] CTD Relay: %s", text.array);
		dstr_free(&text);
	} else if (strcmp(type, "system.notice") == 0) {
		add_alert("notice", obs_data_get_string(data, "message"));
	} else if (strcmp(type, "profile.status") == 0) {
		update_profile(data);
	} else if (strcmp(type, "profiles.changed") == 0) {
		ms.profiles_dirty = true;
	} else if (strcmp(type, "apps.changed") == 0) {
		ms.overlays_dirty = true;
	} else if (strcmp(type, "app.message") == 0) {
		handle_app_message(data);
	} else if (strcmp(type, "system.reset") == 0) {
		ms.resync_needed = true;
	}

	pthread_mutex_unlock(&ms.mutex);
	obs_data_release(data);
	os_event_signal(ms.wake); /* dirty flags are acted on by the client thread */
}

/* One Server-Sent Events frame: remember the cursor, then act on it. */
static void dispatch_event(const char *id, const char *event, const char *data, const char *profile)
{
	if (id && *id) {
		pthread_mutex_lock(&ms.mutex);
		if (same(ms.profile_id, profile))
			set_string(&ms.last_event_id, id);
		pthread_mutex_unlock(&ms.mutex);
	}
	if (!data || !*data)
		return;
	obs_data_t *envelope = obs_data_create_from_json(data);
	if (!envelope)
		return;
	const char *type = obs_data_get_string(envelope, "type");
	if (!*type)
		type = event ? event : "";
	/* the stream is for one profile, plus events about no single profile */
	obs_data_t *src = obs_data_get_obj(envelope, "src");
	const char *event_profile = src ? obs_data_get_string(src, "profile") : "";
	if (!*event_profile || strcmp(event_profile, profile) == 0)
		handle_event(type, envelope);
	obs_data_release(src);
	obs_data_release(envelope);
}

struct sse_parser {
	struct dstr line;
	struct dstr data;
	char id[96];
	char event[64];
};

static void sse_line(struct sse_parser *p, const char *profile)
{
	char *line = p->line.array ? p->line.array : "";
	size_t len = p->line.len;
	if (len && line[len - 1] == '\r')
		line[--len] = 0;
	if (!len) {
		/* a blank line ends the event */
		dispatch_event(p->id, p->event, p->data.array, profile);
		dstr_free(&p->data);
		p->event[0] = 0;
		return;
	}
	if (line[0] == ':')
		return; /* a comment: the server's ping */
	char *value = strchr(line, ':');
	const char *field = line;
	if (value) {
		*value++ = 0;
		if (*value == ' ')
			value++;
	} else {
		value = "";
	}
	if (strcmp(field, "data") == 0) {
		if (p->data.len)
			dstr_cat_ch(&p->data, '\n');
		dstr_cat(&p->data, value);
	} else if (strcmp(field, "id") == 0) {
		snprintf(p->id, sizeof(p->id), "%s", value);
	} else if (strcmp(field, "event") == 0) {
		snprintf(p->event, sizeof(p->event), "%s", value);
	}
}

static void *ms_events_thread(void *unused)
{
	UNUSED_PARAMETER(unused);
	os_set_thread_name("ctd-multistream-events");

	while (!os_atomic_load_bool(&ms.stopping)) {
		pthread_mutex_lock(&ms.mutex);
		char *base = api_base(ms.server);
		char *server = ms.server ? bstrdup(ms.server) : NULL;
		char *token = ms.token && !ms.token_refused ? bstrdup(ms.token) : NULL;
		char *profile = safe_id(ms.profile_id) ? bstrdup(ms.profile_id) : NULL;
		char *cursor = ms.last_event_id ? bstrdup(ms.last_event_id) : NULL;
		pthread_mutex_unlock(&ms.mutex);
		os_atomic_set_bool(&ms.events_restart, false);

		bool backoff = true;
		if (base && token && profile) {
			struct dstr url = {0};
			dstr_printf(&url, "%s/events?profile=%s&types=%s", base, profile, EVENT_TYPES);
			struct http_call c = {.url = url.array, .bearer = token, .interrupt = events_interrupt_cb};
			AVIOContext *io = NULL;
			const enum http_status status = http_events_open(&c, cursor, EVENTS_IDLE_TIMEOUT_S, &io);
			dstr_free(&url);
			if (status == HTTP_UNAUTHORIZED)
				note_result(status, server);
			if (status == HTTP_OK) {
				pthread_mutex_lock(&ms.mutex);
				ms.events_live = true;
				/* without a cursor the stream starts from now: chat and status come from the REST calls */
				if (!cursor)
					ms.resync_needed = true;
				ms.seq++;
				pthread_mutex_unlock(&ms.mutex);
				os_event_signal(ms.wake);
				blog(LOG_INFO, "[ctd-live] CTD Relay: event stream open%s", cursor ? " (resumed)" : "");

				struct sse_parser parser = {0};
				unsigned char buffer[8192];
				for (;;) {
					const int got = avio_read_partial(io, buffer, sizeof(buffer));
					if (got <= 0)
						break; /* closed, timed out (no ping), or interrupted */
					for (int i = 0; i < got; i++) {
						if (buffer[i] == '\n') {
							sse_line(&parser, profile);
							dstr_free(&parser.line);
						} else if (parser.line.len < 256 * 1024) {
							dstr_cat_ch(&parser.line, (char)buffer[i]);
						}
					}
				}
				dstr_free(&parser.line);
				dstr_free(&parser.data);
				avio_closep(&io);
				pthread_mutex_lock(&ms.mutex);
				ms.events_live = false;
				ms.seq++;
				pthread_mutex_unlock(&ms.mutex);
				backoff = !os_atomic_load_bool(&ms.events_restart);
			}
		}
		bfree(base);
		bfree(server);
		bfree(token);
		bfree(profile);
		bfree(cursor);

		/* wait before dialling again, unless something changed that wants a new stream now */
		for (int i = 0; backoff && i < 30 && !os_atomic_load_bool(&ms.stopping) &&
				!os_atomic_load_bool(&ms.events_restart);
		     i++)
			os_sleep_ms(100);
	}
	return NULL;
}

/* ------------------------------------------------------------- client thread --- */

static void *ms_thread(void *unused)
{
	UNUSED_PARAMETER(unused);
	os_set_thread_name("ctd-multistream");
	uint64_t next_profiles = 0, next_status = 0, next_overlays = 0, next_heartbeat = 0, next_pairing_poll = 0;
	uint64_t next_clock = 0, next_clock_burst = 0, next_events = 0, last_events = 0;
	uint64_t next_obsws_check = 0, next_obsws_send = 0; /* v1.6: every 30 s for changes, every 5 min anyway */
	uint64_t next_release = 0;
	bool registered = false; /* this connection's latest registration (heartbeat) went through */
	bool events_backing_off = false;
	int clock_burst_left = 0;

	while (!os_atomic_load_bool(&ms.stopping)) {
		os_event_timedwait(ms.wake, 500);
		if (os_atomic_load_bool(&ms.stopping))
			break;

		pthread_mutex_lock(&ms.mutex);
		char *base = api_base(ms.server);
		char *server = ms.server ? bstrdup(ms.server) : NULL;
		char *token = ms.token && !ms.token_refused ? bstrdup(ms.token) : NULL;
		char *profile = safe_id(ms.profile_id) ? bstrdup(ms.profile_id) : NULL;
		char *pairing_id = ms.pairing_id ? bstrdup(ms.pairing_id) : NULL;
		char *poll_secret = ms.poll_secret ? bstrdup(ms.poll_secret) : NULL;
		char *overlay_template = ms.overlay_send ? bstrdup(ms.overlay_template) : NULL;
		char *overlay_canvas = ms.overlay_send ? bstrdup(ms.overlay_canvas) : NULL;
		const bool overlay_send = ms.overlay_send;
		const bool pairing_requested = ms.pairing_requested;
		const int pairing_interval = ms.pairing_interval_s;
		const bool refresh = ms.refresh_requested || ms.reset_requested;
		const bool resync = ms.resync_needed;
		const bool profiles_dirty = ms.profiles_dirty;
		const bool overlays_dirty = ms.overlays_dirty;
		const bool ingest = ms.ingest_requested;
		const bool clock_dirty = ms.clock_dirty;
		const bool clock_reset = ms.clock_reset;
		const bool events_urgent = ms.events_urgent;
		ms.events_urgent = false;
		ms.clock_dirty = ms.clock_reset = false;
		ms.refresh_requested = ms.reset_requested = ms.resync_needed = false;
		ms.profiles_dirty = ms.overlays_dirty = ms.ingest_requested = ms.overlay_send = false;
		if (ms.overlay_request == MS_OVERLAY_WAITING && os_gettime_ns() > ms.overlay_deadline_ns)
			set_overlay_request(MS_OVERLAY_FAILED, "CTD Overlay didn't answer");
		pthread_mutex_unlock(&ms.mutex);

		const uint64_t now = os_gettime_ns();
		if (refresh) {
			next_profiles = next_status = next_overlays = next_heartbeat = 0;
			next_obsws_check = next_obsws_send =
				0; /* a new connection gets the share (or the revoke) at once */
		}
		if (refresh || clock_reset)
			next_clock = next_clock_burst = 0;

		if (base && pairing_requested) {
			start_pairing(base);
			next_pairing_poll = now + 2 * NS_PER_S;
		} else if (base && pairing_id && poll_secret && now >= next_pairing_poll) {
			poll_pairing(base, pairing_id, poll_secret);
			next_pairing_poll = now + (uint64_t)pairing_interval * NS_PER_S;
		}

		if (base && token) {
			if (profiles_dirty || now >= next_profiles) {
				fetch_profiles(base, token, server);
				next_profiles = now + PROFILES_EVERY_NS;
			}
			if (profile) {
				if (refresh || resync || now >= next_status) {
					fetch_status(base, token, profile, server);
					next_status = now + STATUS_EVERY_NS;
				}
				if (refresh || resync)
					fetch_chat(base, token, profile);
				if (overlays_dirty || now >= next_overlays) {
					fetch_overlays(base, token, profile);
					next_overlays = now + OVERLAYS_EVERY_NS;
				}
				if (ingest)
					fetch_ingest(base, token, profile);
				if (overlay_send)
					send_overlay_request(base, token, profile, overlay_template, overlay_canvas);
				if (now >= next_clock_burst) {
					/* a fresh burst: older samples go, so the clock never leans on a stale one */
					pthread_mutex_lock(&ms.mutex);
					ms.clock_count = ms.clock_next = 0;
					pthread_mutex_unlock(&ms.mutex);
					clock_burst_left = CLOCK_BURST;
					next_clock = 0;
					next_clock_burst = now + CLOCK_RESYNC_NS;
				}
				if (clock_dirty || now >= next_clock) {
					if (fetch_clock(base, token, profile)) {
						if (clock_burst_left > 0)
							clock_burst_left--;
						next_clock = now + (clock_burst_left > 0 ? CLOCK_BURST_EVERY_NS
											 : CLOCK_EVERY_NS);
					} else {
						next_clock = now + CLOCK_RETRY_NS;
					}
				}
			}
			pthread_mutex_lock(&ms.mutex);
			const bool register_first = ms.events_register;
			ms.events_register = false;
			pthread_mutex_unlock(&ms.mutex);
			if (register_first || now >= next_heartbeat) {
				registered = heartbeat(base, token);
				next_heartbeat = now + HEARTBEAT_EVERY_NS;
			}
			/* v1.6: OBS's WebSocket settings for the hub, while the user shares them; once more to revoke */
			pthread_mutex_lock(&ms.mutex);
			const bool share = ms.share_obsws;
			/* unticked, but the hub may still hold them (the revoke failed, or OBS closed before it went) */
			const bool revoke_owed = !share && ms.obsws_on_relay;
			/* after (re-)registering too: a new id, or a hub that lost track of this OBS */
			const bool obsws_dirty = ms.obsws_dirty || (share && register_first);
			const uint64_t obsws_sent = ms.obsws_sent;
			ms.obsws_dirty = false;
			pthread_mutex_unlock(&ms.mutex);
			if (!registered) {
				/* not until this connection is registered: before that the hub can't take it (a 404 would read
				 * as an older hub, and settle a revoke that isn't) */
				pthread_mutex_lock(&ms.mutex);
				ms.obsws_dirty = ms.obsws_dirty || obsws_dirty;
				pthread_mutex_unlock(&ms.mutex);
			} else if (obsws_dirty || ((share || revoke_owed) && now >= next_obsws_check)) {
				next_obsws_check = now + 30 * NS_PER_S;
				bool due = obsws_dirty || now >= next_obsws_send;
				if (!due && share) {
					struct obsws_config ws;
					obsws_config_read(&ws);
					due = ws.fingerprint != obsws_sent;
					obsws_config_free(&ws);
				}
				if (due)
					next_obsws_send =
						now + send_obs_websocket(base, token, server, share) * NS_PER_S;
			}
			/* an urgent event brings the next batch forward, unless the hub asked us to back off */
			if (events_urgent && !events_backing_off) {
				const uint64_t soonest = last_events + EVENTS_URGENT_GAP_NS;
				if (soonest < next_events)
					next_events = soonest;
			}
			if (profile && now >= next_events) {
				const uint64_t wait = send_events(base, token, profile);
				events_backing_off = wait > EVENTS_EVERY_NS;
				last_events = now;
				next_events = now + wait;
			} else if (events_urgent && profile) {
				/* too soon after the last POST: keep it urgent, and look again then */
				pthread_mutex_lock(&ms.mutex);
				ms.events_urgent = true;
				pthread_mutex_unlock(&ms.mutex);
			}
		} else if (overlay_send) {
			pthread_mutex_lock(&ms.mutex);
			set_overlay_request(MS_OVERLAY_FAILED, "Connect to CTD Relay first");
			pthread_mutex_unlock(&ms.mutex);
		}

		/* dropped connections' unregistering, after this one's work, so a dead old server can't hold it up */
		if (now >= next_release)
			next_release = release_app() ? 0 : now + RELEASE_RETRY_NS;

		bfree(base);
		bfree(server);
		bfree(token);
		bfree(profile);
		bfree(pairing_id);
		bfree(poll_secret);
		bfree(overlay_template);
		bfree(overlay_canvas);
	}
	return NULL;
}

/* ------------------------------------------------------------- API --- */

void ms_init(void)
{
	pthread_mutex_init(&ms.mutex, NULL);
	os_event_init(&ms.wake, OS_EVENT_TYPE_AUTO);
	os_atomic_set_bool(&ms.alive, true);
	load_settings();
	if (!ms.server)
		ms.server = bstrdup(MS_DEFAULT_SERVER);
	ms.link = ms.token ? MS_CONNECTED : MS_DISCONNECTED;
	set_string(&ms.message, ms.token ? "Connecting…" : "Not connected");
	ms.refresh_requested = true;
	ms.events_boot_ms = epoch_ms();
	if (pthread_create(&ms.thread, NULL, ms_thread, NULL) == 0)
		ms.thread_running = true;
	if (pthread_create(&ms.events_thread, NULL, ms_events_thread, NULL) == 0)
		ms.events_thread_running = true;
}

void ms_shutdown(void)
{
	os_atomic_set_bool(&ms.alive, false);
	os_atomic_set_bool(&ms.stopping, true);
	os_event_signal(ms.wake);
	if (ms.thread_running) {
		pthread_join(ms.thread, NULL);
		ms.thread_running = false;
	}
	if (ms.events_thread_running) {
		pthread_join(ms.events_thread, NULL);
		ms.events_thread_running = false;
	}
	os_event_destroy(ms.wake);
	pthread_mutex_destroy(&ms.mutex);
	clear_pairing();
	release_array(&ms.profiles);
	release_data(&ms.status);
	release_array(&ms.overlays);
	release_data(&ms.ingest);
	release_array(&ms.chat);
	release_array(&ms.alerts);
	release_data(&ms.overlay_created);
	release_array(&ms.events);
	set_string(&ms.last_event_id, NULL);
	set_string(&ms.overlay_template, NULL);
	set_string(&ms.app_id, NULL);
	while (ms.release_count) /* saved already: tried again next time */
		drop_release(ms.release_count - 1);
	set_string(&ms.obsws_message, NULL);
	set_string(&ms.overlay_canvas, NULL);
	set_string(&ms.overlay_message_id, NULL);
	set_string(&ms.overlay_message, NULL);
	set_string(&ms.server, NULL);
	set_string(&ms.token, NULL);
	set_string(&ms.profile_id, NULL);
	set_string(&ms.message, NULL);
}

static char *copy_or_empty(const char *value)
{
	return bstrdup(value ? value : "");
}

void ms_snapshot_get(struct ms_snapshot *out)
{
	memset(out, 0, sizeof(*out));
	if (!os_atomic_load_bool(&ms.alive))
		return;
	pthread_mutex_lock(&ms.mutex);
	out->link = ms.link;
	out->message = copy_or_empty(ms.message);
	out->server = copy_or_empty(ms.server);
	out->pairing_code = copy_or_empty(ms.pairing_code);
	out->approve_url = copy_or_empty(ms.approve_url);
	out->profile_id = copy_or_empty(ms.profile_id);
	out->profile_locked = ms.profile_locked;
	out->events_live = ms.events_live;
	out->profiles = ms.profiles;
	obs_data_array_addref(out->profiles);
	out->status = ms.status;
	obs_data_addref(out->status);
	out->overlays = ms.overlays;
	obs_data_array_addref(out->overlays);
	out->ingest = ms.ingest;
	obs_data_addref(out->ingest);
	out->ingest_seq = ms.ingest_seq;
	out->chat = ms.chat;
	obs_data_array_addref(out->chat);
	out->chat_seq = ms.chat_seq;
	out->alerts = ms.alerts;
	obs_data_array_addref(out->alerts);
	out->alerts_seq = ms.alerts_seq;
	out->overlay_request = ms.overlay_request;
	out->overlay_message = copy_or_empty(ms.overlay_message);
	out->overlay_created = ms.overlay_created;
	obs_data_addref(out->overlay_created);
	out->overlay_seq = ms.overlay_seq;
	out->events_state = ms.events_state;
	out->events_sent = ms.events_sent;
	out->share_obs_websocket = ms.share_obsws;
	out->obs_websocket_message = bstrdup(ms.obsws_message ? ms.obsws_message : "");
	out->seq = ms.seq;
	pthread_mutex_unlock(&ms.mutex);
}

void ms_snapshot_free(struct ms_snapshot *snapshot)
{
	bfree(snapshot->message);
	bfree(snapshot->server);
	bfree(snapshot->pairing_code);
	bfree(snapshot->approve_url);
	bfree(snapshot->profile_id);
	bfree(snapshot->overlay_message);
	bfree(snapshot->obs_websocket_message);
	obs_data_array_release(snapshot->profiles);
	obs_data_release(snapshot->status);
	obs_data_array_release(snapshot->overlays);
	obs_data_release(snapshot->ingest);
	obs_data_array_release(snapshot->chat);
	obs_data_array_release(snapshot->alerts);
	obs_data_release(snapshot->overlay_created);
	memset(snapshot, 0, sizeof(*snapshot));
}

void ms_set_server(const char *server)
{
	if (!os_atomic_load_bool(&ms.alive))
		return;
	pthread_mutex_lock(&ms.mutex);
	if (!server || !*server || same(ms.server, server)) {
		pthread_mutex_unlock(&ms.mutex);
		return;
	}
	/* like a Disconnect: the old server unregisters this OBS (and forgets OBS's WebSocket settings) */
	queue_release();
	set_string(&ms.server, server);
	/* a token belongs to one server, and so does the user's OK to share OBS's password: a new one asks again */
	set_string(&ms.token, NULL);
	set_string(&ms.profile_id, NULL);
	ms.profile_locked = false;
	ms.app_id_renewed = false;
	ms.token_refused = false;
	ms.obsws_on_relay = ms.obsws_on_relay || ms.share_obsws; /* the next connection makes sure it's forgotten */
	ms.share_obsws = false;
	ms.obsws_dirty = false;
	ms.obsws_sent = 0;
	set_string(&ms.obsws_message, NULL);
	clear_pairing();
	release_array(&ms.profiles);
	clear_profile_state();
	save_settings();
	set_link(MS_DISCONNECTED, "Not connected");
	ms.reset_requested = true;
	pthread_mutex_unlock(&ms.mutex);
	os_event_signal(ms.wake);
}

void ms_start_pairing(void)
{
	if (!os_atomic_load_bool(&ms.alive))
		return;
	pthread_mutex_lock(&ms.mutex);
	clear_pairing();
	ms.pairing_requested = true;
	set_link(MS_PAIRING, "Asking CTD Relay for a code…");
	pthread_mutex_unlock(&ms.mutex);
	os_event_signal(ms.wake);
}

void ms_disconnect(void)
{
	if (!os_atomic_load_bool(&ms.alive))
		return;
	pthread_mutex_lock(&ms.mutex);
	/* the worker unregisters this OBS with the old token first (not on the UI thread: it's a network call) */
	queue_release();
	set_string(&ms.token, NULL);
	ms.app_id_renewed = false;
	ms.token_refused = false;
	/* the release tells the old hub to forget it; a new connection gets it again (if shared) */
	ms.obsws_sent = 0;
	ms.obsws_dirty = ms.share_obsws;
	set_string(&ms.obsws_message, NULL);
	ms.profile_locked = false;
	clear_pairing();
	release_array(&ms.profiles);
	clear_profile_state();
	save_settings();
	set_link(MS_DISCONNECTED, "Not connected");
	pthread_mutex_unlock(&ms.mutex);
	os_event_signal(ms.wake);
}

void ms_set_share_obs_websocket(bool on)
{
	if (!os_atomic_load_bool(&ms.alive))
		return;
	pthread_mutex_lock(&ms.mutex);
	if (ms.share_obsws == on) {
		pthread_mutex_unlock(&ms.mutex);
		return;
	}
	ms.share_obsws = on;
	if (!on)
		ms.obsws_on_relay = true; /* owed until the hub answers the revoke, even if OBS closes first */
	ms.obsws_dirty = true;
	set_string(&ms.obsws_message, on ? "Sharing with CTD Relay…" : "Telling CTD Relay to forget OBS's password…");
	save_settings();
	ms.seq++;
	pthread_mutex_unlock(&ms.mutex);
	blog(LOG_INFO, "[ctd-live] CTD Relay: OBS's WebSocket settings %s", on ? "shared" : "no longer shared");
	os_event_signal(ms.wake);
}

void ms_set_profile(const char *profile_id)
{
	if (!os_atomic_load_bool(&ms.alive))
		return;
	pthread_mutex_lock(&ms.mutex);
	if (ms.profile_locked || !safe_id(profile_id) || same(ms.profile_id, profile_id)) {
		pthread_mutex_unlock(&ms.mutex);
		return;
	}
	set_string(&ms.profile_id, profile_id);
	clear_profile_state();
	save_settings();
	ms.reset_requested = true;
	pthread_mutex_unlock(&ms.mutex);
	os_event_signal(ms.wake);
}

void ms_request_ingest(void)
{
	if (!os_atomic_load_bool(&ms.alive))
		return;
	pthread_mutex_lock(&ms.mutex);
	ms.ingest_requested = true;
	pthread_mutex_unlock(&ms.mutex);
	os_event_signal(ms.wake);
}

void ms_request_overlay(const char *template_id, const char *canvas)
{
	if (!os_atomic_load_bool(&ms.alive))
		return;
	pthread_mutex_lock(&ms.mutex);
	set_string(&ms.overlay_template, template_id);
	set_string(&ms.overlay_canvas, canvas);
	set_string(&ms.overlay_message_id, NULL);
	release_data(&ms.overlay_created);
	ms.overlay_send = true;
	set_overlay_request(MS_OVERLAY_SENDING, "Looking for CTD Overlay on the hub…");
	pthread_mutex_unlock(&ms.mutex);
	os_event_signal(ms.wake);
}

void ms_clock_get(struct ms_clock *out)
{
	memset(out, 0, sizeof(*out));
	if (!os_atomic_load_bool(&ms.alive))
		return;
	pthread_mutex_lock(&ms.mutex);
	out->valid = ms.clock_valid;
	if (ms.clock_valid) {
		out->now_ms = (int64_t)(mono_ms() + ms.clock_offset_ms);
		out->run_started_ms = ms.clock_run_started_ms;
		out->fps = ms.clock_fps;
		out->utc_offset_min = ms.clock_utc_offset_min;
		out->delay_ms = ms.clock_delay_ms;
	}
	pthread_mutex_unlock(&ms.mutex);
}

void ms_post_event(const char *type, obs_data_t *data)
{
	if (!os_atomic_load_bool(&ms.alive) || !type || !data)
		return;
	pthread_mutex_lock(&ms.mutex);
	if (!ms.token || ms.token_refused || ms.events_need_scope) {
		pthread_mutex_unlock(&ms.mutex);
		return;
	}
	char id[64];
	snprintf(id, sizeof(id), "%lld-%llu", ms.events_boot_ms, ++ms.events_next_id);
	/* on the hub's clock when it's synced, so CTD Live mobile can line events up with the timecode */
	const long long ts = ms.clock_valid ? (long long)(mono_ms() + ms.clock_offset_ms) : epoch_ms();

	obs_data_t *event = obs_data_create();
	obs_data_set_string(event, "id", id);
	obs_data_set_string(event, "type", type);
	obs_data_set_int(event, "ts", ts);
	obs_data_t *copy = clone_data(data);
	obs_data_set_obj(event, "data", copy);
	obs_data_release(copy);

	if (!ms.events)
		ms.events = obs_data_array_create();
	if (obs_data_array_count(ms.events) >= EVENTS_KEEP)
		obs_data_array_erase(ms.events, 0); /* the hub has been away a while: the oldest go first */
	obs_data_array_push_back(ms.events, event);
	obs_data_release(event);
	const bool urgent = strcmp(type, "obs.scene") == 0 || strcmp(type, "obs.feed.state") == 0;
	ms.events_urgent |= urgent;
	pthread_mutex_unlock(&ms.mutex);
	if (urgent)
		os_event_signal(ms.wake);
}

void ms_timecode(int64_t ms_value, double fps, bool wrap_day, char *out, size_t size)
{
	const int64_t nominal = fps > 0 ? (int64_t)(fps + 0.5) : 30; /* non-drop-frame counts at the nominal rate */
	if (ms_value < 0)
		ms_value = 0;
	const int64_t seconds = ms_value / 1000;
	const int64_t frame = (ms_value % 1000) * nominal / 1000;
	int64_t hours = seconds / 3600;
	if (wrap_day)
		hours %= 24;
	snprintf(out, size, "%02lld:%02lld:%02lld:%02lld", (long long)hours, (long long)(seconds / 60 % 60),
		 (long long)(seconds % 60), (long long)frame);
}

void ms_refresh(void)
{
	if (!os_atomic_load_bool(&ms.alive))
		return;
	pthread_mutex_lock(&ms.mutex);
	ms.refresh_requested = true;
	ms.token_refused = false;
	pthread_mutex_unlock(&ms.mutex);
	os_event_signal(ms.wake);
	os_atomic_set_bool(&ms.events_restart, true);
}
