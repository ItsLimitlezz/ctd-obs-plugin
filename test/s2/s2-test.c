/*
 * Spike S2 harness: drives http_get_file / http_put_file (src/http-client.c, compiled unmodified) against
 * test/s2/server.mjs or, later, a dev CTD Relay. Links only libcurl and libavutil (plus test/s2/shim for libobs).
 *
 *   s2-test --get URL --put URL --token TOKEN --dir DIR [--debug URL]
 *
 *   --get    an existing large file on the server (server.mjs: http://127.0.0.1:8787/files/big.bin)
 *   --put    where to upload a copy; must be GET-able at the same URL afterwards
 *   --dir    a scratch directory on this machine (needs ~2x the file's size free)
 *   --debug  server.mjs only: its /debug/partials URL; also turns on the fault-injection tests (T6, T7, T10)
 *
 * Exit status 0 when every test passed.
 */

#include "http-client.h"

#include <curl/curl.h>
#include <libavutil/mem.h>
#include <libavutil/sha.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <dirent.h>
#include <sys/resource.h>
#include <sys/stat.h>
#endif

static int failures;

#define CHECK(cond, ...)                                     \
	do {                                                 \
		if (cond) {                                  \
			printf("    ok    " __VA_ARGS__);    \
		} else {                                     \
			printf("    FAIL  " __VA_ARGS__);    \
			failures++;                          \
		}                                            \
		printf("\n");                                \
	} while (0)

struct watch {
	unsigned calls;
	uint64_t last_done, last_total, max_done;
	bool monotonic;
	double abort_at; /* fraction of the total at which interrupt() says stop; 0 = never */
	uint64_t aborted_at;
};

static void on_progress(void *opaque, uint64_t done, uint64_t total)
{
	struct watch *w = opaque;
	if (done < w->last_done)
		w->monotonic = false;
	w->calls++;
	w->last_done = done;
	w->last_total = total;
	if (done > w->max_done)
		w->max_done = done;
}

static int on_interrupt(void *opaque)
{
	struct watch *w = opaque;
	if (w->abort_at > 0 && w->last_total > 0 && (double)w->last_done >= w->abort_at * (double)w->last_total) {
		if (!w->aborted_at)
			w->aborted_at = w->last_done;
		return 1;
	}
	return 0;
}

static void watch_reset(struct watch *w, double abort_at)
{
	memset(w, 0, sizeof(*w));
	w->monotonic = true;
	w->abort_at = abort_at;
}

static double now_s(void)
{
	struct timespec ts;
	timespec_get(&ts, TIME_UTC);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static bool file_exists(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (f)
		fclose(f);
	return f != NULL;
}

/* SHA-256 of a file on disk, independently of the transfer */
static bool hash_file(const char *path, char out[65], uint64_t *size)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return false;
	struct AVSHA *sha = av_sha_alloc();
	av_sha_init(sha, 256);
	static unsigned char buf[1 << 20];
	size_t got;
	*size = 0;
	while ((got = fread(buf, 1, sizeof(buf), f)) > 0) {
		av_sha_update(sha, buf, got);
		*size += got;
	}
	fclose(f);
	uint8_t digest[32];
	av_sha_final(sha, digest);
	av_freep(&sha);
	for (int i = 0; i < 32; i++)
		sprintf(out + i * 2, "%02x", digest[i]);
	return true;
}

/* how many "*.part" files are in dir */
static int count_parts(const char *dir)
{
	int n = 0;
#ifdef _WIN32
	char pattern[1024];
	snprintf(pattern, sizeof(pattern), "%s\\*.part", dir);
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA(pattern, &fd);
	if (h != INVALID_HANDLE_VALUE) {
		do
			n++;
		while (FindNextFileA(h, &fd));
		FindClose(h);
	}
#else
	DIR *d = opendir(dir);
	struct dirent *e;
	while (d && (e = readdir(d))) {
		const size_t len = strlen(e->d_name);
		if (len > 5 && strcmp(e->d_name + len - 5, ".part") == 0)
			n++;
	}
	if (d)
		closedir(d);
#endif
	return n;
}

static double peak_rss_mb(void)
{
#ifdef _WIN32
	PROCESS_MEMORY_COUNTERS pmc;
	GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
	return (double)pmc.PeakWorkingSetSize / (1024.0 * 1024.0);
#else
	struct rusage ru;
	getrusage(RUSAGE_SELF, &ru);
	return (double)ru.ru_maxrss / 1024.0; /* KiB on Linux */
#endif
}

struct small_body {
	char text[4096];
	size_t len;
};

static size_t small_write(char *data, size_t size, size_t count, void *opaque)
{
	struct small_body *b = opaque;
	const size_t len = size * count;
	const size_t room = sizeof(b->text) - 1 - b->len;
	const size_t n = len < room ? len : room;
	memcpy(b->text + b->len, data, n);
	b->len += n;
	b->text[b->len] = 0;
	return len;
}

/* server.mjs's /debug/partials: true when no ".upload" temp file is left */
static bool server_has_no_partials(const char *debug_url, char *seen, size_t seen_len)
{
	for (int attempt = 0; attempt < 20; attempt++) { /* the server deletes asynchronously */
		struct small_body b = {0};
		CURL *c = curl_easy_init();
		curl_easy_setopt(c, CURLOPT_URL, debug_url);
		curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, small_write);
		curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
		const CURLcode res = curl_easy_perform(c);
		curl_easy_cleanup(c);
		snprintf(seen, seen_len, "%s", res == CURLE_OK ? b.text : curl_easy_strerror(res));
		if (res == CURLE_OK && strstr(b.text, "\"partials\":[]"))
			return true;
#ifdef _WIN32
		Sleep(100);
#else
		nanosleep(&(struct timespec){0, 100 * 1000 * 1000}, NULL);
#endif
	}
	return false;
}

static void write_text(const char *path, const char *text)
{
	FILE *f = fopen(path, "wb");
	if (f) {
		fputs(text, f);
		fclose(f);
	}
}

static bool file_is(const char *path, const char *text)
{
	char buf[256] = {0};
	FILE *f = fopen(path, "rb");
	if (!f)
		return false;
	const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	return n == strlen(text) && memcmp(buf, text, n) == 0;
}

static void progress_checks(const struct watch *w, uint64_t size)
{
	CHECK(w->calls > 1, "progress callback called %u times", w->calls);
	CHECK(w->monotonic, "progress never went backwards");
	CHECK(w->last_done == size && w->last_total == size, "last progress = %llu / %llu (file %llu)",
	      (unsigned long long)w->last_done, (unsigned long long)w->last_total, (unsigned long long)size);
}

int main(int argc, char **argv)
{
	const char *get_url = NULL, *put_url = NULL, *token = NULL, *dir = NULL, *debug_url = NULL;
	for (int i = 1; i + 1 < argc; i += 2) {
		if (!strcmp(argv[i], "--get"))
			get_url = argv[i + 1];
		else if (!strcmp(argv[i], "--put"))
			put_url = argv[i + 1];
		else if (!strcmp(argv[i], "--token"))
			token = argv[i + 1];
		else if (!strcmp(argv[i], "--dir"))
			dir = argv[i + 1];
		else if (!strcmp(argv[i], "--debug"))
			debug_url = argv[i + 1];
	}
	if (!get_url || !put_url || !token || !dir) {
		fprintf(stderr, "usage: s2-test --get URL --put URL --token TOKEN --dir DIR [--debug URL]\n");
		return 2;
	}
	curl_global_init(CURL_GLOBAL_DEFAULT);
	printf("libcurl %s\n", curl_version());

	char down[1024], back[1024], abort_path[1024], url[4096], put_abort_url[2048], hex[65], hex2[65], disk[65];
	snprintf(down, sizeof(down), "%s/down.bin", dir);
	snprintf(back, sizeof(back), "%s/back.bin", dir);
	snprintf(abort_path, sizeof(abort_path), "%s/abort.bin", dir);
	snprintf(put_abort_url, sizeof(put_abort_url), "%s.aborted", put_url);
	remove(down);
	remove(back);
	remove(abort_path);
	struct watch w;
	uint64_t size = 0;
	enum http_status st;
	double t0, secs;

	printf("T1 download (GET, streamed to a temp file, renamed)\n");
	watch_reset(&w, 0);
	t0 = now_s();
	st = http_get_file(get_url, token, down, hex, on_interrupt, on_progress, &w);
	secs = now_s() - t0;
	CHECK(st == HTTP_OK, "status %s", http_status_text(st));
	CHECK(hash_file(down, disk, &size) && strcmp(disk, hex) == 0, "streamed sha256 %s = sha256 of the file on disk",
	      hex);
	printf("          %.0f MiB in %.1f s (%.0f MiB/s)\n", (double)size / 1048576.0, secs,
	       (double)size / 1048576.0 / secs);
	progress_checks(&w, size);
	CHECK(count_parts(dir) == 0, "no .part file left");
	const uint64_t big_size = size;
	char big_sha[65];
	memcpy(big_sha, hex, sizeof(big_sha));

	printf("T2 upload (PUT with Content-Length, streamed from disk)\n");
	watch_reset(&w, 0);
	t0 = now_s();
	st = http_put_file(put_url, token, down, hex, on_interrupt, on_progress, &w);
	secs = now_s() - t0;
	CHECK(st == HTTP_OK, "status %s (any X-Content-SHA256 from the server matched)", http_status_text(st));
	CHECK(strcmp(hex, big_sha) == 0, "streamed sha256 %s = T1's", hex);
	printf("          %.0f MiB in %.1f s (%.0f MiB/s)\n", (double)big_size / 1048576.0, secs,
	       (double)big_size / 1048576.0 / secs);
	progress_checks(&w, big_size);

	printf("T3 round trip (GET the uploaded copy back)\n");
	watch_reset(&w, 0);
	st = http_get_file(put_url, token, back, hex2, on_interrupt, on_progress, &w);
	CHECK(st == HTTP_OK, "status %s", http_status_text(st));
	CHECK(strcmp(hex2, big_sha) == 0, "sha256 after GET -> PUT -> GET %s = original", hex2);
	CHECK(hash_file(back, disk, &size) && strcmp(disk, big_sha) == 0 && size == big_size,
	      "file on disk matches the original byte for byte (sha256 + size)");
	remove(back);

	printf("T4 abort a GET halfway (target already exists)\n");
	write_text(abort_path, "old content");
	watch_reset(&w, 0.5);
	st = http_get_file(get_url, token, abort_path, hex, on_interrupt, on_progress, &w);
	CHECK(st == HTTP_ABORTED, "status %s, interrupted at %llu of %llu bytes", http_status_text(st),
	      (unsigned long long)w.aborted_at, (unsigned long long)w.last_total);
	CHECK(file_is(abort_path, "old content"), "existing file untouched");
	CHECK(count_parts(dir) == 0, "no .part file left");
	CHECK(hex[0] == 0, "no sha256 reported");
	remove(abort_path);

	printf("T5 abort a PUT halfway\n");
	watch_reset(&w, 0.5);
	st = http_put_file(put_abort_url, token, down, hex, on_interrupt, on_progress, &w);
	CHECK(st == HTTP_ABORTED, "status %s, interrupted at %llu of %llu bytes", http_status_text(st),
	      (unsigned long long)w.aborted_at, (unsigned long long)w.last_total);
	watch_reset(&w, 0);
	st = http_get_file(put_abort_url, token, abort_path, hex, on_interrupt, on_progress, &w);
	CHECK(st == HTTP_NOT_FOUND, "server didn't publish the partial upload (GET it: %s)", http_status_text(st));
	CHECK(!file_exists(abort_path) && count_parts(dir) == 0, "and that failed GET left no file here");
	if (debug_url) {
		char seen[4096];
		CHECK(server_has_no_partials(debug_url, seen, sizeof(seen)), "server deleted its temp file (%s)", seen);
	}

	if (debug_url) {
		printf("T6 server drops the connection halfway through a GET\n");
		snprintf(url, sizeof(url), "%s?fail=midway", get_url);
		watch_reset(&w, 0);
		st = http_get_file(url, token, abort_path, hex, on_interrupt, on_progress, &w);
		CHECK(st == HTTP_UNREACHABLE, "status %s after %llu bytes", http_status_text(st),
		      (unsigned long long)w.max_done);
		CHECK(!file_exists(abort_path) && count_parts(dir) == 0, "no file and no .part left");

		printf("T7 server's X-Content-SHA256 doesn't match the bytes\n");
		snprintf(url, sizeof(url), "%s?badsha=1", get_url);
		watch_reset(&w, 0);
		st = http_get_file(url, token, abort_path, hex, on_interrupt, on_progress, &w);
		CHECK(st == HTTP_BAD_RESPONSE, "status %s", http_status_text(st));
		CHECK(!file_exists(abort_path) && count_parts(dir) == 0, "no file and no .part left");

		printf("T10 server takes the whole PUT body, then answers 500\n");
		snprintf(url, sizeof(url), "%s?fail=end", put_abort_url);
		watch_reset(&w, 0);
		st = http_put_file(url, token, down, hex, on_interrupt, on_progress, &w);
		CHECK(st == HTTP_SERVER_ERROR, "status %s after %llu bytes", http_status_text(st),
		      (unsigned long long)w.max_done);
		CHECK(hex[0] == 0, "no sha256 reported");
	}

	printf("T8 wrong token on a PUT (refused before the body: Expect: 100-continue)\n");
	watch_reset(&w, 0);
	st = http_put_file(put_abort_url, "wrong-token", down, hex, on_interrupt, on_progress, &w);
	CHECK(st == HTTP_UNAUTHORIZED, "status %s", http_status_text(st));
	CHECK(w.max_done < 1024 * 1024, "%llu body bytes read before the refusal", (unsigned long long)w.max_done);

	printf("T9 wrong token on a GET\n");
	watch_reset(&w, 0);
	st = http_get_file(get_url, "wrong-token", abort_path, hex, on_interrupt, on_progress, &w);
	CHECK(st == HTTP_UNAUTHORIZED, "status %s", http_status_text(st));
	CHECK(!file_exists(abort_path) && count_parts(dir) == 0, "no file and no .part left");

	const double rss = peak_rss_mb();
	printf("Memory\n");
	CHECK(rss < 64.0, "peak RSS %.1f MiB for a %.0f MiB file (nothing buffered whole)", rss,
	      (double)big_size / 1048576.0);

	remove(down);
	curl_global_cleanup();
	printf("\n%s (%d failed)\n", failures ? "FAILED" : "ALL PASSED", failures);
	return failures ? 1 : 0;
}
