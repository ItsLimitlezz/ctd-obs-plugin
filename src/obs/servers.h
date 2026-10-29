/*
 * Server profiles (SPEC 5.1, H1 to H4): the MediaMTX servers the sources read from, kept in the plugin's own config
 * (servers.json), so every scene collection shares them. A source stores a profile id and a path; changing a
 * profile's address or credentials reconnects every source that uses it.
 *
 * Secrets (API password and token, SRT read password, SRT passphrase) live only here and in the engine's URL; they are
 * never logged or shown in a stats text.
 *
 * All calls are thread-safe. Watchers are called on the thread that made the change (the UI's, from the dialog),
 * with no lock held.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ctd_server {
	char id[48];
	char *name;
	char *host;     /* the machine running MediaMTX */
	int srt_port;   /* 8890 */
	char *api_url;  /* "http://<host>:9997"; empty: no API */
	char *api_user; /* API login, or */
	char *api_pass;
	char *api_token; /* a bearer token (MediaMTX JWT auth) */
	char *read_user; /* SRT read credentials: streamid read:<path>:<user>:<pass> */
	char *read_pass;
	char *passphrase; /* SRT encryption passphrase, 10 to 79 characters, or empty */
	char *webhook_url; /* I4: events about this server's feeds are POSTed here; empty for none. May hold a token: secret */
	bool is_default;
	bool allow_writes; /* H8: the CTD Hub may create and remove paths and kick publishers on this server */
};

void servers_init(void);
void servers_shutdown(void);

size_t servers_count(void);
/* Copies of the profiles; free each with ctd_server_free. */
bool servers_get_index(size_t index, struct ctd_server *out);
/* The profile with this id; NULL or "" gives the default one. False if there is none. */
bool servers_get(const char *id, struct ctd_server *out);
void ctd_server_free(struct ctd_server *server);

/* Adds (empty id) or replaces a profile and saves. Returns its id in id_out (may be NULL). */
void servers_put(const struct ctd_server *server, char *id_out, size_t size);
void servers_remove(const char *id);

/* The API's host and port from api_url ("http://host:9997"). False if it is blank or can't be read. */
bool ctd_server_api_address(const struct ctd_server *server, char *host, size_t size, int *port);

typedef void (*servers_changed_fn)(void *param, const char *id);
void servers_watch(servers_changed_fn fn, void *param);
void servers_unwatch(servers_changed_fn fn, void *param);

/* src/ui/servers-dialog.cpp: the Tools ▸ CTD Live servers window (UI thread only). */
void ctd_servers_dialog_register(void);
void ctd_servers_dialog_open(void);

#ifdef __cplusplus
}
#endif
