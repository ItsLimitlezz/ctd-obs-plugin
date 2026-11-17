/* Test shim: libc versions of the libobs functions src/http-client.c calls. */
#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include "obs-data.h"
#include "util/bmem.h"
#include "util/dstr.h"
#include "util/platform.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

obs_data_t *obs_data_create_from_json(const char *json)
{
	(void)json;
	return NULL; /* the JSON path isn't exercised by the S2 harness */
}

void *bmalloc(size_t size)
{
	void *p = malloc(size ? size : 1);
	if (!p)
		abort();
	return p;
}

void bfree(void *ptr)
{
	free(ptr);
}

static void dstr_reserve(struct dstr *dst, size_t len)
{
	if (len + 1 <= dst->capacity)
		return;
	dst->capacity = (len + 1) * 2;
	dst->array = realloc(dst->array, dst->capacity);
	if (!dst->array)
		abort();
}

void dstr_ncat(struct dstr *dst, const char *array, size_t len)
{
	dstr_reserve(dst, dst->len + len);
	memcpy(dst->array + dst->len, array, len);
	dst->len += len;
	dst->array[dst->len] = 0;
}

void dstr_copy(struct dstr *dst, const char *array)
{
	dst->len = 0;
	dstr_ncat(dst, array, strlen(array));
}

static void dstr_vcatf(struct dstr *dst, const char *format, va_list args)
{
	va_list copy;
	va_copy(copy, args);
	const int n = vsnprintf(NULL, 0, format, copy);
	va_end(copy);
	if (n < 0)
		abort();
	dstr_reserve(dst, dst->len + (size_t)n);
	vsnprintf(dst->array + dst->len, (size_t)n + 1, format, args);
	dst->len += (size_t)n;
}

void dstr_catf(struct dstr *dst, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	dstr_vcatf(dst, format, args);
	va_end(args);
}

void dstr_printf(struct dstr *dst, const char *format, ...)
{
	dst->len = 0;
	dstr_reserve(dst, 0);
	dst->array[0] = 0;
	va_list args;
	va_start(args, format);
	dstr_vcatf(dst, format, args);
	va_end(args);
}

void dstr_free(struct dstr *dst)
{
	free(dst->array);
	dst->array = NULL;
	dst->len = dst->capacity = 0;
}

FILE *os_fopen(const char *path, const char *mode)
{
	return fopen(path, mode);
}

#ifdef _WIN32
/* as libobs does it: replace the target in one step (rename() refuses an existing target on Windows) */
int os_rename(const char *old_path, const char *new_path)
{
	return MoveFileExA(old_path, new_path, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
}

int os_unlink(const char *path)
{
	return _unlink(path);
}

int os_fseeki64(FILE *file, int64_t offset, int origin)
{
	return _fseeki64(file, offset, origin);
}

int64_t os_ftelli64(FILE *file)
{
	return _ftelli64(file);
}

uint64_t os_gettime_ns(void)
{
	LARGE_INTEGER freq, now;
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&now);
	return (uint64_t)((double)now.QuadPart * 1e9 / (double)freq.QuadPart);
}
#else
int os_rename(const char *old_path, const char *new_path)
{
	return rename(old_path, new_path);
}

int os_unlink(const char *path)
{
	return unlink(path);
}

int os_fseeki64(FILE *file, int64_t offset, int origin)
{
	return fseeko(file, (off_t)offset, origin);
}

int64_t os_ftelli64(FILE *file)
{
	return (int64_t)ftello(file);
}

uint64_t os_gettime_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#endif
