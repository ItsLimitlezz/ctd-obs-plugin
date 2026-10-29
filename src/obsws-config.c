/*
 * OBS's WebSocket settings and the LAN address. See obsws-config.h.
 */

#include "obsws-config.h"

#include <obs-module.h>
#include <util/bmem.h>
#include <util/dstr.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

/* <OBS config>/plugin_config/obs-websocket/config.json, from this plugin's own config folder (its sibling) */
static char *config_path(void)
{
	char *own = obs_module_config_path("");
	if (!own)
		return NULL;
	struct dstr path = {0};
	dstr_copy(&path, own);
	bfree(own);
	/* ".../plugin_config/ctd-live-source/" -> ".../plugin_config/" */
	while (path.len && (path.array[path.len - 1] == '/' || path.array[path.len - 1] == '\\'))
		dstr_resize(&path, path.len - 1);
	const char *slash = strrchr(path.array, '/');
	const char *backslash = strrchr(path.array, '\\');
	const char *cut = slash > backslash ? slash : backslash;
	if (!cut) {
		dstr_free(&path);
		return NULL;
	}
	dstr_resize(&path, (size_t)(cut - path.array) + 1);
	dstr_cat(&path, "obs-websocket/config.json");
	return path.array;
}

/* obs-websocket's launch overrides ("--websocket_port 4456", "--websocket_password=..."), which win over its file */
static const char *cmdline_value(const char *name)
{
	const struct obs_cmdline_args args = obs_get_cmdline_args();
	const size_t len = strlen(name);
	for (int i = 1; i < args.argc; i++) {
		const char *arg = args.argv[i];
		if (!arg || strncmp(arg, "--", 2) != 0 || strncmp(arg + 2, name, len) != 0)
			continue;
		if (arg[2 + len] == '=')
			return arg + 3 + len;
		if (arg[2 + len] == '\0' && i + 1 < args.argc)
			return args.argv[i + 1];
	}
	return NULL;
}

static uint64_t fnv(uint64_t hash, const void *data, size_t size)
{
	const unsigned char *p = data;
	for (size_t i = 0; i < size; i++)
		hash = (hash ^ p[i]) * 1099511628211ull;
	return hash;
}

void obsws_config_read(struct obsws_config *out)
{
	memset(out, 0, sizeof(*out));
	out->port = 4455;
	char *path = config_path();
	obs_data_t *json = path ? obs_data_create_from_json_file(path) : NULL;
	bfree(path);
	if (!json)
		return;
	out->found = true;
	out->enabled = obs_data_get_bool(json, "server_enabled");
	out->port = (int)obs_data_get_int(json, "server_port");
	if (out->port <= 0 || out->port > 65535)
		out->port = 4455;
	out->auth_required = obs_data_get_bool(json, "auth_required");
	const char *port = cmdline_value("websocket_port");
	const char *password = cmdline_value("websocket_password");
	if (port && atoi(port) > 0 && atoi(port) <= 65535)
		out->port = atoi(port);
	if (password && *password)
		out->auth_required = true; /* as obs-websocket does */
	out->password = bstrdup(password && *password ? password : obs_data_get_string(json, "server_password"));
	obs_data_release(json);

	uint64_t hash = 14695981039346656037ull;
	hash = fnv(hash, &out->enabled, sizeof(out->enabled));
	hash = fnv(hash, &out->port, sizeof(out->port));
	hash = fnv(hash, &out->auth_required, sizeof(out->auth_required));
	hash = fnv(hash, out->password, strlen(out->password));
	out->fingerprint = hash;
}

void obsws_config_free(struct obsws_config *config)
{
	if (config->password) {
		memset(config->password, 0, strlen(config->password));
		bfree(config->password);
	}
	memset(config, 0, sizeof(*config));
}

bool obsws_local_address(const char *host, char *out, size_t size)
{
	if (!host || !*host || !size)
		return false;
	/* the bare host: no scheme, port or path */
	const char *start = strstr(host, "://");
	start = start ? start + 3 : host;
	char name[256];
	size_t n = 0;
	while (start[n] && start[n] != ':' && start[n] != '/' && n + 1 < sizeof(name)) {
		name[n] = start[n];
		n++;
	}
	name[n] = '\0';
	if (!n)
		return false;

#ifdef _WIN32
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
		return false;
#endif
	bool ok = false;
	struct addrinfo hints = {0}, *found = NULL;
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	if (getaddrinfo(name, "9", &hints, &found) == 0 && found) {
		/* a UDP "connect" picks the route and the local address without sending anything */
#ifdef _WIN32
		SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		const bool opened = s != INVALID_SOCKET;
#else
		int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		const bool opened = s >= 0;
#endif
		if (opened) {
			struct sockaddr_in local;
			socklen_t length = sizeof(local);
			if (connect(s, found->ai_addr, (int)found->ai_addrlen) == 0 &&
			    getsockname(s, (struct sockaddr *)&local, &length) == 0 &&
			    inet_ntop(AF_INET, &local.sin_addr, out, (socklen_t)size))
				ok = local.sin_addr.s_addr != 0;
#ifdef _WIN32
			closesocket(s);
#else
			close(s);
#endif
		}
		freeaddrinfo(found);
	}
#ifdef _WIN32
	WSACleanup();
#endif
	return ok;
}
