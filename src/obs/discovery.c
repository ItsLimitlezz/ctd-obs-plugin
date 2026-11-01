/*
 * Network discovery. See discovery.h.
 */

#include "discovery.h"
#include "../obsws-config.h"

#include <obs-module.h>
#include <plugin-support.h>
#include <util/bmem.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include <stdio.h>
#include <string.h>

#define SERVICE_TYPE "_ctd-obs._tcp"
#define CHECK_MS 5000 /* how often the WebSocket settings and the address are looked at again */

/* What is announced; a change to any of it re-announces. */
struct announcement {
	int port;
	bool auth;
	char address[64];
};

#if defined(_WIN32)

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windns.h>

/* dnsapi's DNS-SD functions (Windows 10 1809+), looked up at run time so an older Windows still loads the plugin */
typedef PDNS_SERVICE_INSTANCE(WINAPI *construct_fn)(PCWSTR, PCWSTR, PIP4_ADDRESS, PIP6_ADDRESS, WORD, WORD, WORD, DWORD,
						    PCWSTR *, PCWSTR *);
typedef DWORD(WINAPI *register_fn)(PDNS_SERVICE_REGISTER_REQUEST, PDNS_SERVICE_CANCEL);
typedef VOID(WINAPI *free_fn)(PDNS_SERVICE_INSTANCE);

static struct {
	HMODULE dll;
	construct_fn construct;
	register_fn reg;
	register_fn dereg;
	free_fn free_instance;
} api;

struct registration {
	DNS_SERVICE_REGISTER_REQUEST request;
	PDNS_SERVICE_INSTANCE instance;
	HANDLE done;
	volatile DWORD status;
};

static VOID WINAPI on_complete(DWORD status, PVOID context, PDNS_SERVICE_INSTANCE instance)
{
	struct registration *r = context;
	r->status = status;
	if (instance)
		api.free_instance(instance);
	SetEvent(r->done);
}

static bool platform_init(char *why, size_t size)
{
	if (api.reg)
		return true;
	api.dll = LoadLibraryExW(L"dnsapi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (api.dll) {
		api.construct = (construct_fn)GetProcAddress(api.dll, "DnsServiceConstructInstance");
		api.reg = (register_fn)GetProcAddress(api.dll, "DnsServiceRegister");
		api.dereg = (register_fn)GetProcAddress(api.dll, "DnsServiceDeRegister");
		api.free_instance = (free_fn)GetProcAddress(api.dll, "DnsServiceFreeInstance");
	}
	if (!api.construct || !api.reg || !api.dereg || !api.free_instance) {
		api.reg = NULL;
		snprintf(why, size, "this Windows has no DNS-SD (it needs Windows 10 1809 or later)");
		return false;
	}
	return true;
}

static void platform_free(void)
{
	if (api.dll)
		FreeLibrary(api.dll);
	memset(&api, 0, sizeof(api));
}

static void *announce(const struct announcement *a, char *name, size_t name_size, char *why, size_t why_size)
{
	wchar_t host[256];
	DWORD length = sizeof(host) / sizeof(*host);
	if (!GetComputerNameExW(ComputerNamePhysicalDnsHostname, host, &length) || !length) {
		snprintf(why, why_size, "couldn't read this computer's name");
		return NULL;
	}
	struct in_addr ip4;
	if (inet_pton(AF_INET, a->address, &ip4) != 1) {
		snprintf(why, why_size, "no LAN address");
		return NULL;
	}

	wchar_t instance[400], host_local[300];
	swprintf(instance, sizeof(instance) / sizeof(*instance), L"%ls._ctd-obs._tcp.local", host);
	swprintf(host_local, sizeof(host_local) / sizeof(*host_local), L"%ls.local", host);
	wchar_t plugin[32], obs[32];
	swprintf(plugin, sizeof(plugin) / sizeof(*plugin), L"%hs", PLUGIN_VERSION);
	swprintf(obs, sizeof(obs) / sizeof(*obs), L"%hs", obs_get_version_string());
	PCWSTR keys[] = {L"v", L"auth", L"plugin", L"obs"};
	PCWSTR values[] = {L"1", a->auth ? L"1" : L"0", plugin, obs};

	struct registration *r = bzalloc(sizeof(*r));
	r->done = CreateEventW(NULL, FALSE, FALSE, NULL);
	IP4_ADDRESS address = ip4.s_addr;
	r->instance = api.construct(instance, host_local, &address, NULL, (WORD)a->port, 0, 0, 4, keys, values);
	if (!r->done || !r->instance) {
		snprintf(why, why_size, "out of memory");
		goto fail;
	}
	r->request.Version = DNS_QUERY_REQUEST_VERSION1;
	r->request.InterfaceIndex = 0; /* every network */
	r->request.pServiceInstance = r->instance;
	r->request.pRegisterCompletionCallback = on_complete;
	r->request.pQueryContext = r;
	const DWORD status = api.reg(&r->request, NULL);
	if (status != DNS_REQUEST_PENDING) {
		snprintf(why, why_size, "Windows refused the announcement (error %lu)", (unsigned long)status);
		goto fail;
	}
	if (WaitForSingleObject(r->done, 3000) == WAIT_OBJECT_0 && r->status != ERROR_SUCCESS) {
		snprintf(why, why_size, "Windows refused the announcement (error %lu)", (unsigned long)r->status);
		goto fail;
	}
	char *utf8 = NULL;
	os_wcs_to_utf8_ptr(host, 0, &utf8);
	snprintf(name, name_size, "%s", utf8 ? utf8 : "");
	bfree(utf8);
	return r;

fail:
	if (r->instance)
		api.free_instance(r->instance);
	if (r->done)
		CloseHandle(r->done);
	bfree(r);
	return NULL;
}

static void withdraw(void *handle)
{
	struct registration *r = handle;
	ResetEvent(r->done);
	if (api.dereg(&r->request, NULL) == DNS_REQUEST_PENDING)
		WaitForSingleObject(r->done, 3000);
	api.free_instance(r->instance);
	CloseHandle(r->done);
	bfree(r);
}

#elif defined(__APPLE__)

#include <arpa/inet.h>
#include <dns_sd.h>

static bool platform_init(char *why, size_t size)
{
	(void)why;
	(void)size;
	return true;
}

static void platform_free(void) {}

static void *announce(const struct announcement *a, char *name, size_t name_size, char *why, size_t why_size)
{
	TXTRecordRef txt;
	TXTRecordCreate(&txt, 0, NULL);
	const char *plugin = PLUGIN_VERSION;
	const char *obs = obs_get_version_string();
	TXTRecordSetValue(&txt, "v", 1, "1");
	TXTRecordSetValue(&txt, "auth", 1, a->auth ? "1" : "0");
	TXTRecordSetValue(&txt, "plugin", (uint8_t)strlen(plugin), plugin);
	TXTRecordSetValue(&txt, "obs", (uint8_t)strlen(obs), obs);

	/* no name: Bonjour uses the computer's name ("Studio MacBook Pro") and renames on a clash; all networks */
	DNSServiceRef ref = NULL;
	const DNSServiceErrorType err = DNSServiceRegister(&ref, 0, 0, NULL, SERVICE_TYPE, NULL, NULL,
							   htons((uint16_t)a->port), TXTRecordGetLength(&txt),
							   TXTRecordGetBytesPtr(&txt), NULL, NULL);
	TXTRecordDeallocate(&txt);
	if (err != kDNSServiceErr_NoError) {
		snprintf(why, why_size, "Bonjour refused the announcement (error %d)", (int)err);
		return NULL;
	}
	snprintf(name, name_size, "this Mac's name, as in System Settings ▸ General ▸ Sharing");
	return ref;
}

static void withdraw(void *handle)
{
	DNSServiceRefDeallocate((DNSServiceRef)handle);
}

#else

static bool platform_init(char *why, size_t size)
{
	snprintf(why, size, "not available on Linux yet");
	return false;
}

static void platform_free(void) {}

static void *announce(const struct announcement *a, char *name, size_t name_size, char *why, size_t why_size)
{
	(void)a;
	(void)name;
	(void)name_size;
	snprintf(why, why_size, "not available on Linux yet");
	return NULL;
}

static void withdraw(void *handle)
{
	(void)handle;
}

#endif

static struct {
	pthread_t thread;
	bool running;
	os_event_t *wake;
	volatile bool stopping;
	volatile long enabled;
} disc = {.enabled = 1};

/* What should be announced now; false if nothing (switched off, or obs-websocket's server is off or missing). */
static bool wanted(struct announcement *a, const char **why)
{
	if (!os_atomic_load_long(&disc.enabled)) {
		*why = "switched off in the Tools menu";
		return false;
	}
	struct obsws_config ws;
	obsws_config_read(&ws);
	const bool on = ws.found && ws.enabled;
	memset(a, 0, sizeof(*a));
	a->port = ws.port;
	a->auth = ws.auth_required;
	obsws_config_free(&ws);
	if (!on) {
		*why = "OBS's WebSocket server is off (Tools ▸ WebSocket Server Settings)";
		return false;
	}
	/* the address the default route leaves from: the LAN one on a normal home or studio network (nothing is sent) */
	if (!obsws_local_address("192.0.2.1", a->address, sizeof(a->address))) {
		*why = "this computer isn't on a network";
		return false;
	}
	return true;
}

static void *discovery_thread(void *unused)
{
	(void)unused;
	os_set_thread_name("ctd-discovery");
	char why[160] = "";
	const bool available = platform_init(why, sizeof(why));
	if (!available)
		blog(LOG_INFO, "[ctd-live] network discovery: %s", why);

	void *handle = NULL;
	struct announcement current = {0};
	char logged[160] = ""; /* the last reason logged, so each is logged once */
	while (available && !disc.stopping) {
		struct announcement next;
		const char *reason = NULL;
		const bool want = wanted(&next, &reason);
		if (handle && (!want || memcmp(&next, &current, sizeof(next)) != 0)) {
			withdraw(handle);
			handle = NULL;
		}
		if (want && !handle) {
			char name[256] = "";
			handle = announce(&next, name, sizeof(name), why, sizeof(why));
			if (handle) {
				current = next;
				blog(LOG_INFO,
				     "[ctd-live] network discovery: CTD apps on the network can find this OBS as \"%s\" "
				     "(%s, WebSocket port %d%s)",
				     name, next.address, next.port, next.auth ? ", password needed" : "");
				logged[0] = '\0';
			} else if (strcmp(logged, why) != 0) {
				blog(LOG_WARNING, "[ctd-live] network discovery: %s", why);
				snprintf(logged, sizeof(logged), "%s", why);
			}
		} else if (!want && strcmp(logged, reason) != 0) {
			blog(LOG_INFO, "[ctd-live] network discovery: not announcing this OBS: %s", reason);
			snprintf(logged, sizeof(logged), "%s", reason);
		}
		os_event_timedwait(disc.wake, CHECK_MS);
	}
	if (handle)
		withdraw(handle);
	platform_free();
	return NULL;
}

void discovery_start(void)
{
	if (disc.running || os_event_init(&disc.wake, OS_EVENT_TYPE_AUTO) != 0)
		return;
	disc.stopping = false;
	disc.running = pthread_create(&disc.thread, NULL, discovery_thread, NULL) == 0;
	if (!disc.running) {
		os_event_destroy(disc.wake);
		disc.wake = NULL;
	}
}

void discovery_stop(void)
{
	if (!disc.running)
		return;
	disc.stopping = true;
	os_event_signal(disc.wake);
	pthread_join(disc.thread, NULL);
	os_event_destroy(disc.wake);
	disc.wake = NULL;
	disc.running = false;
}

void discovery_set_enabled(bool enabled)
{
	os_atomic_set_long(&disc.enabled, enabled ? 1 : 0);
	if (disc.wake)
		os_event_signal(disc.wake);
}

bool discovery_enabled(void)
{
	return os_atomic_load_long(&disc.enabled) != 0;
}
