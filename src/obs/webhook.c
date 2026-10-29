/*
 * The webhook. See webhook.h.
 */

#include "webhook.h"
#include "servers.h"
#include "../http-client.h"

#include <obs-module.h>
#include <util/bmem.h>
#include <util/darray.h>
#include <util/platform.h>
#include <util/threading.h>

#include <string.h>
#include <time.h>

#define MAX_WAITING 200
#define TIMEOUT_NS 1000000000ull /* I4: 1 s */
#define LOG_EVERY_NS (60ull * 1000000000ull)

struct delivery {
	char *url;
	char *profile; /* its name, for the log */
	char *body;
};

static struct {
	pthread_t thread;
	bool running;
	pthread_mutex_t mutex;
	os_sem_t *wake;
	volatile bool stopping;
	DARRAY(struct delivery) queue;
	uint64_t logged_ns; /* failures are logged once a minute */
	long dropped;
} hook;

static void free_delivery(struct delivery *d)
{
	if (d->url)
		memset(d->url, 0, strlen(d->url));
	bfree(d->url);
	bfree(d->profile);
	bfree(d->body);
}

static int past_deadline(void *opaque)
{
	const uint64_t deadline = *(const uint64_t *)opaque;
	return os_gettime_ns() > deadline || hook.stopping;
}

static void deliver(struct delivery *d)
{
	uint64_t deadline = os_gettime_ns() + TIMEOUT_NS;
	const struct http_call call = {
		.method = "POST",
		.url = d->url,
		.body = d->body,
		.follow_redirects = true, /* nothing secret in it */
		.interrupt = past_deadline,
		.opaque = &deadline,
	};
	const enum http_status status = http_call_json(&call, NULL);
	if (status == HTTP_OK || status == HTTP_BAD_RESPONSE) /* a 2xx with a body that isn't JSON is still delivered */
		return;
	const uint64_t now = os_gettime_ns();
	if (now - hook.logged_ns >= LOG_EVERY_NS) {
		hook.logged_ns = now;
		blog(LOG_WARNING,
		     "[ctd-live] the webhook of profile \"%s\" failed (%s); events are dropped, never retried",
		     d->profile, http_status_text(status));
	}
}

static void *webhook_thread(void *unused)
{
	UNUSED_PARAMETER(unused);
	os_set_thread_name("ctd-webhook");
	while (os_sem_wait(hook.wake) == 0 && !hook.stopping) {
		struct delivery d = {0};
		pthread_mutex_lock(&hook.mutex);
		if (hook.queue.num) {
			d = hook.queue.array[0];
			da_erase(hook.queue, 0);
		}
		pthread_mutex_unlock(&hook.mutex);
		if (d.url) {
			deliver(&d);
			free_delivery(&d);
		}
	}
	return NULL;
}

void webhook_start(void)
{
	if (hook.running)
		return;
	pthread_mutex_init(&hook.mutex, NULL);
	da_init(hook.queue);
	if (os_sem_init(&hook.wake, 0) != 0)
		return;
	hook.stopping = false;
	hook.running = pthread_create(&hook.thread, NULL, webhook_thread, NULL) == 0;
}

void webhook_stop(void)
{
	if (!hook.running)
		return;
	hook.stopping = true;
	os_sem_post(hook.wake);
	pthread_join(hook.thread, NULL);
	os_sem_destroy(hook.wake);
	for (size_t i = 0; i < hook.queue.num; i++)
		free_delivery(&hook.queue.array[i]);
	da_free(hook.queue);
	pthread_mutex_destroy(&hook.mutex);
	hook.running = false;
}

static void enqueue(const struct ctd_server *s, const char *body)
{
	struct delivery d = {bstrdup(s->webhook_url), bstrdup(s->name), bstrdup(body)};
	pthread_mutex_lock(&hook.mutex);
	if (hook.queue.num >= MAX_WAITING) {
		free_delivery(&hook.queue.array[0]);
		da_erase(hook.queue, 0);
		hook.dropped++;
	}
	da_push_back(hook.queue, &d);
	pthread_mutex_unlock(&hook.mutex);
	os_sem_post(hook.wake);
}

void webhook_post(const char *type, obs_data_t *data)
{
	if (!hook.running || !type || !data)
		return;
	const char *server_id = obs_data_get_string(data, "serverId");
	char *body = NULL;
	for (size_t i = 0; i < servers_count(); i++) {
		struct ctd_server s;
		if (!servers_get_index(i, &s))
			continue;
		const bool wanted = s.webhook_url && *s.webhook_url && (!*server_id || strcmp(server_id, s.id) == 0);
		if (wanted) {
			if (!body) {
				struct timespec ts;
				timespec_get(&ts, TIME_UTC);
				obs_data_t *envelope = obs_data_create();
				obs_data_set_string(envelope, "source", "ctd-obs-plugin");
				obs_data_set_string(envelope, "type", type);
				obs_data_set_int(envelope, "timeMs",
						 (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
				obs_data_set_obj(envelope, "data", data);
				body = bstrdup(obs_data_get_json(envelope));
				obs_data_release(envelope);
			}
			enqueue(&s, body);
		}
		ctd_server_free(&s);
	}
	bfree(body);
}
