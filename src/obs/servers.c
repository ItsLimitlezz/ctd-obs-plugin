/*
 * Server profiles. See servers.h.
 */

#include "servers.h"

#include <obs-module.h>
#include <util/bmem.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct watcher {
	servers_changed_fn fn;
	void *param;
};

static struct {
	pthread_mutex_t mutex;
	/* held while watchers are called, so servers_unwatch (a source being destroyed) waits for a call in flight */
	pthread_mutex_t notify_mutex;
	bool ready;
	DARRAY(struct ctd_server) list;
	DARRAY(struct watcher) watchers;
	unsigned long long next_id;
} servers;

/* --------------------------------------------------------------- helpers -- */

static char *dup_or_empty(const char *value)
{
	return bstrdup(value ? value : "");
}

static void copy_server(struct ctd_server *to, const struct ctd_server *from)
{
	memcpy(to->id, from->id, sizeof(to->id));
	to->name = dup_or_empty(from->name);
	to->host = dup_or_empty(from->host);
	to->srt_port = from->srt_port;
	to->api_url = dup_or_empty(from->api_url);
	to->api_user = dup_or_empty(from->api_user);
	to->api_pass = dup_or_empty(from->api_pass);
	to->api_token = dup_or_empty(from->api_token);
	to->read_user = dup_or_empty(from->read_user);
	to->read_pass = dup_or_empty(from->read_pass);
	to->passphrase = dup_or_empty(from->passphrase);
	to->webhook_url = dup_or_empty(from->webhook_url);
	to->is_default = from->is_default;
	to->allow_writes = from->allow_writes;
}

/* secrets are wiped, not just freed */
static void free_secret(char **value)
{
	if (*value)
		memset(*value, 0, strlen(*value));
	bfree(*value);
	*value = NULL;
}

void ctd_server_free(struct ctd_server *s)
{
	bfree(s->name);
	bfree(s->host);
	bfree(s->api_url);
	bfree(s->api_user);
	free_secret(&s->api_pass);
	free_secret(&s->api_token);
	bfree(s->read_user);
	free_secret(&s->read_pass);
	free_secret(&s->passphrase);
	free_secret(&s->webhook_url);
	memset(s, 0, sizeof(*s));
}

static struct ctd_server *find(const char *id)
{
	for (size_t i = 0; i < servers.list.num; i++) {
		if (id && *id ? strcmp(servers.list.array[i].id, id) == 0 : servers.list.array[i].is_default)
			return &servers.list.array[i];
	}
	/* no default marked: the first one */
	return (!id || !*id) && servers.list.num ? &servers.list.array[0] : NULL;
}

/* ---------------------------------------------------------------- storage -- */

static char *config_file(void)
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	return obs_module_config_path("servers.json");
}

/* call with the mutex held */
static void save(void)
{
	obs_data_t *root = obs_data_create();
	obs_data_array_t *items = obs_data_array_create();
	for (size_t i = 0; i < servers.list.num; i++) {
		const struct ctd_server *s = &servers.list.array[i];
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "id", s->id);
		obs_data_set_string(item, "name", s->name);
		obs_data_set_string(item, "host", s->host);
		obs_data_set_int(item, "srt_port", s->srt_port);
		obs_data_set_string(item, "api_url", s->api_url);
		obs_data_set_string(item, "api_user", s->api_user);
		obs_data_set_string(item, "api_pass", s->api_pass);
		obs_data_set_string(item, "api_token", s->api_token);
		obs_data_set_string(item, "read_user", s->read_user);
		obs_data_set_string(item, "read_pass", s->read_pass);
		obs_data_set_string(item, "passphrase", s->passphrase);
		obs_data_set_string(item, "webhook_url", s->webhook_url);
		obs_data_set_bool(item, "default", s->is_default);
		obs_data_set_bool(item, "allow_writes", s->allow_writes);
		obs_data_array_push_back(items, item);
		obs_data_release(item);
	}
	obs_data_set_array(root, "servers", items);
	obs_data_array_release(items);
	char *file = config_file();
	if (file && !obs_data_save_json_safe(root, file, "tmp", "bak"))
		blog(LOG_WARNING, "[ctd-live] couldn't save the server profiles");
	bfree(file);
	obs_data_release(root);
}

static void new_id(char *out, size_t size)
{
	snprintf(out, size, "srv-%llx-%llu", (unsigned long long)os_gettime_ns(), ++servers.next_id);
}

static void load(void)
{
	char *file = config_file();
	obs_data_t *root = file ? obs_data_create_from_json_file_safe(file, "bak") : NULL;
	bfree(file);
	obs_data_array_t *items = root ? obs_data_get_array(root, "servers") : NULL;
	for (size_t i = 0; items && i < obs_data_array_count(items); i++) {
		obs_data_t *item = obs_data_array_item(items, i);
		struct ctd_server s = {0};
		snprintf(s.id, sizeof(s.id), "%s", obs_data_get_string(item, "id"));
		if (!*s.id)
			new_id(s.id, sizeof(s.id));
		s.name = dup_or_empty(obs_data_get_string(item, "name"));
		s.host = dup_or_empty(obs_data_get_string(item, "host"));
		s.srt_port = (int)obs_data_get_int(item, "srt_port");
		if (s.srt_port <= 0)
			s.srt_port = 8890;
		s.api_url = dup_or_empty(obs_data_get_string(item, "api_url"));
		s.api_user = dup_or_empty(obs_data_get_string(item, "api_user"));
		s.api_pass = dup_or_empty(obs_data_get_string(item, "api_pass"));
		s.api_token = dup_or_empty(obs_data_get_string(item, "api_token"));
		s.read_user = dup_or_empty(obs_data_get_string(item, "read_user"));
		s.read_pass = dup_or_empty(obs_data_get_string(item, "read_pass"));
		s.passphrase = dup_or_empty(obs_data_get_string(item, "passphrase"));
		s.webhook_url = dup_or_empty(obs_data_get_string(item, "webhook_url"));
		s.is_default = obs_data_get_bool(item, "default");
		s.allow_writes = obs_data_get_bool(item, "allow_writes");
		da_push_back(servers.list, &s);
		obs_data_release(item);
	}
	obs_data_array_release(items);
	obs_data_release(root);

	/* H3: a first run starts with this machine; the dialog's Test button checks it */
	if (!servers.list.num) {
		struct ctd_server s = {0};
		new_id(s.id, sizeof(s.id));
		s.name = bstrdup("This computer");
		s.host = bstrdup("127.0.0.1");
		s.srt_port = 8890;
		s.api_url = bstrdup("http://127.0.0.1:9997");
		s.api_user = bstrdup("");
		s.api_pass = bstrdup("");
		s.api_token = bstrdup("");
		s.read_user = bstrdup("");
		s.read_pass = bstrdup("");
		s.passphrase = bstrdup("");
		s.webhook_url = bstrdup("");
		s.is_default = true;
		da_push_back(servers.list, &s);
		save();
	}
}

/* -------------------------------------------------------------------- API -- */

void servers_init(void)
{
	pthread_mutex_init(&servers.mutex, NULL);
	pthread_mutex_init(&servers.notify_mutex, NULL);
	da_init(servers.list);
	da_init(servers.watchers);
	load();
	servers.ready = true;
}

void servers_shutdown(void)
{
	if (!servers.ready)
		return;
	servers.ready = false;
	for (size_t i = 0; i < servers.list.num; i++)
		ctd_server_free(&servers.list.array[i]);
	da_free(servers.list);
	da_free(servers.watchers);
	pthread_mutex_destroy(&servers.mutex);
	pthread_mutex_destroy(&servers.notify_mutex);
}

size_t servers_count(void)
{
	if (!servers.ready)
		return 0;
	pthread_mutex_lock(&servers.mutex);
	const size_t count = servers.list.num;
	pthread_mutex_unlock(&servers.mutex);
	return count;
}

bool servers_get_index(size_t index, struct ctd_server *out)
{
	memset(out, 0, sizeof(*out));
	if (!servers.ready)
		return false;
	pthread_mutex_lock(&servers.mutex);
	const bool found = index < servers.list.num;
	if (found)
		copy_server(out, &servers.list.array[index]);
	pthread_mutex_unlock(&servers.mutex);
	return found;
}

bool servers_get(const char *id, struct ctd_server *out)
{
	memset(out, 0, sizeof(*out));
	if (!servers.ready)
		return false;
	pthread_mutex_lock(&servers.mutex);
	const struct ctd_server *s = find(id);
	if (s)
		copy_server(out, s);
	pthread_mutex_unlock(&servers.mutex);
	return s != NULL;
}

static void notify(const char *id)
{
	pthread_mutex_lock(&servers.notify_mutex);
	pthread_mutex_lock(&servers.mutex);
	struct watcher *copy = NULL;
	const size_t count = servers.watchers.num;
	if (count) {
		copy = bmalloc(count * sizeof(*copy));
		memcpy(copy, servers.watchers.array, count * sizeof(*copy));
	}
	pthread_mutex_unlock(&servers.mutex);
	for (size_t i = 0; i < count; i++)
		copy[i].fn(copy[i].param, id);
	bfree(copy);
	pthread_mutex_unlock(&servers.notify_mutex);
}

void servers_put(const struct ctd_server *server, char *id_out, size_t size)
{
	if (!servers.ready)
		return;
	char id[48];
	pthread_mutex_lock(&servers.mutex);
	struct ctd_server *existing = *server->id ? find(server->id) : NULL;
	if (existing) {
		ctd_server_free(existing);
		copy_server(existing, server);
	} else {
		struct ctd_server s = {0};
		copy_server(&s, server);
		new_id(s.id, sizeof(s.id));
		da_push_back(servers.list, &s);
		existing = &servers.list.array[servers.list.num - 1];
	}
	/* one default */
	if (existing->is_default) {
		for (size_t i = 0; i < servers.list.num; i++)
			servers.list.array[i].is_default = &servers.list.array[i] == existing;
	}
	snprintf(id, sizeof(id), "%s", existing->id);
	save();
	pthread_mutex_unlock(&servers.mutex);
	blog(LOG_INFO, "[ctd-live] server profile '%s' saved", server->name ? server->name : "");
	if (id_out)
		snprintf(id_out, size, "%s", id);
	notify(id); /* H4: sources on it reconnect */
}

void servers_remove(const char *id)
{
	if (!servers.ready || !id || !*id)
		return;
	pthread_mutex_lock(&servers.mutex);
	for (size_t i = 0; i < servers.list.num; i++) {
		if (strcmp(servers.list.array[i].id, id) == 0) {
			const bool was_default = servers.list.array[i].is_default;
			ctd_server_free(&servers.list.array[i]);
			da_erase(servers.list, i);
			if (was_default && servers.list.num)
				servers.list.array[0].is_default = true;
			break;
		}
	}
	save();
	pthread_mutex_unlock(&servers.mutex);
	notify(id); /* its sources fall back to the default profile */
}

bool ctd_server_api_address(const struct ctd_server *server, char *host, size_t size, int *port)
{
	const char *url = server->api_url ? server->api_url : "";
	while (*url == ' ')
		url++;
	if (!*url)
		return false;
	const char *scheme = strstr(url, "://");
	const char *start = scheme ? scheme + 3 : url;
	*port = 9997;
	size_t length = strcspn(start, "/");
	const char *end = start + length;
	const char *colon = NULL;
	if (*start == '[') { /* [ipv6]:port */
		const char *close = memchr(start, ']', length);
		if (!close)
			return false;
		snprintf(host, size, "%.*s", (int)(close - start - 1), start + 1);
		colon = close + 1 < end && close[1] == ':' ? close + 1 : NULL;
	} else {
		colon = memchr(start, ':', length);
		snprintf(host, size, "%.*s", (int)((colon ? colon : end) - start), start);
	}
	if (colon)
		*port = atoi(colon + 1);
	return *host && *port > 0 && *port < 65536;
}

void servers_watch(servers_changed_fn fn, void *param)
{
	if (!servers.ready)
		return;
	struct watcher w = {fn, param};
	pthread_mutex_lock(&servers.mutex);
	da_push_back(servers.watchers, &w);
	pthread_mutex_unlock(&servers.mutex);
}

void servers_unwatch(servers_changed_fn fn, void *param)
{
	if (!servers.ready)
		return;
	pthread_mutex_lock(&servers.notify_mutex);
	pthread_mutex_lock(&servers.mutex);
	for (size_t i = 0; i < servers.watchers.num; i++) {
		if (servers.watchers.array[i].fn == fn && servers.watchers.array[i].param == param) {
			da_erase(servers.watchers, i);
			break;
		}
	}
	pthread_mutex_unlock(&servers.mutex);
	pthread_mutex_unlock(&servers.notify_mutex);
}
