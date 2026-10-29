/*
 * OBS's own WebSocket server settings (obs-websocket 5, OBS 28 and later), read from its config file
 * (<OBS config>/plugin_config/obs-websocket/config.json), and this machine's address on the LAN. CTD Relay uses them to
 * reach OBS for CTD Live mobile (PUT /apps/{appId}/obs-websocket, API v1.6), when the user lets it.
 *
 * The password is a secret: it is never logged, and obsws_config_free wipes it.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct obsws_config {
	bool found; /* the config file was there and readable */
	bool enabled;
	int port;
	bool auth_required;
	char *password;       /* bmalloc'd; may be empty */
	uint64_t fingerprint; /* changes whenever any of the above does */
};

/* Reads the settings, with obs-websocket's launch overrides (--websocket_port, --websocket_password) on top; found is
 * false if there is no config file (obs-websocket missing, or older than OBS 28). */
void obsws_config_read(struct obsws_config *out);
void obsws_config_free(struct obsws_config *config);

/* The address this machine uses to reach host (the local end of a route to it), e.g. "192.168.1.42"; false if none.
 * host may carry a scheme and a port ("http://192.168.1.50:30080"). Nothing is sent. */
bool obsws_local_address(const char *host, char *out, size_t size);

#ifdef __cplusplus
}
#endif
