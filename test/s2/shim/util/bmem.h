/* Test shim for libobs util/bmem.h */
#pragma once
#include <stddef.h>
void *bmalloc(size_t size);
void bfree(void *ptr);
