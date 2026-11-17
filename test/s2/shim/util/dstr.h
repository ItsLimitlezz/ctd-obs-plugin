/* Test shim for libobs util/dstr.h (only what src/http-client.c uses) */
#pragma once
#include <stddef.h>
struct dstr {
	char *array;
	size_t len;
	size_t capacity;
};
void dstr_copy(struct dstr *dst, const char *array);
void dstr_ncat(struct dstr *dst, const char *array, size_t len);
void dstr_catf(struct dstr *dst, const char *format, ...);
void dstr_printf(struct dstr *dst, const char *format, ...);
void dstr_free(struct dstr *dst);
