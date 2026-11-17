/* Test shim for libobs util/threading.h */
#pragma once
#ifdef _MSC_VER
#include <intrin.h>
static inline long os_atomic_inc_long(volatile long *val)
{
	return _InterlockedIncrement(val);
}
#else
static inline long os_atomic_inc_long(volatile long *val)
{
	return __atomic_add_fetch(val, 1, __ATOMIC_SEQ_CST);
}
#endif
