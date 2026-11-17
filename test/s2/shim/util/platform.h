/* Test shim for libobs util/platform.h (only what src/http-client.c uses) */
#pragma once
#include <stdint.h>
#include <stdio.h>
FILE *os_fopen(const char *path, const char *mode);
int os_rename(const char *old_path, const char *new_path);
int os_unlink(const char *path);
int os_fseeki64(FILE *file, int64_t offset, int origin);
int64_t os_ftelli64(FILE *file);
uint64_t os_gettime_ns(void);
