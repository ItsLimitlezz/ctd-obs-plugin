/*
 * The few atomics the engine needs, without libobs: MSVC intrinsics on Windows, GCC/Clang builtins elsewhere.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef _MSC_VER
#include <intrin.h>

static inline long eng_load_long(volatile long *p)
{
	return _InterlockedOr(p, 0);
}
static inline void eng_store_long(volatile long *p, long v)
{
	_InterlockedExchange(p, v);
}
static inline long eng_inc_long(volatile long *p)
{
	return _InterlockedIncrement(p);
}
static inline long eng_exchange_long(volatile long *p, long v)
{
	return _InterlockedExchange(p, v);
}
static inline long eng_add_long(volatile long *p, long v)
{
	return _InterlockedExchangeAdd(p, v) + v;
}
static inline int64_t eng_load_i64(volatile int64_t *p)
{
	return _InterlockedOr64((volatile long long *)p, 0);
}
static inline void eng_store_i64(volatile int64_t *p, int64_t v)
{
	_InterlockedExchange64((volatile long long *)p, v);
}
#else
static inline long eng_load_long(volatile long *p)
{
	return __atomic_load_n(p, __ATOMIC_SEQ_CST);
}
static inline void eng_store_long(volatile long *p, long v)
{
	__atomic_store_n(p, v, __ATOMIC_SEQ_CST);
}
static inline long eng_inc_long(volatile long *p)
{
	return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST);
}
static inline long eng_exchange_long(volatile long *p, long v)
{
	return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST);
}
static inline long eng_add_long(volatile long *p, long v)
{
	return __atomic_add_fetch(p, v, __ATOMIC_SEQ_CST);
}
static inline int64_t eng_load_i64(volatile int64_t *p)
{
	return __atomic_load_n(p, __ATOMIC_SEQ_CST);
}
static inline void eng_store_i64(volatile int64_t *p, int64_t v)
{
	__atomic_store_n(p, v, __ATOMIC_SEQ_CST);
}
#endif

static inline bool eng_load_bool(volatile long *p)
{
	return eng_load_long(p) != 0;
}
static inline void eng_store_bool(volatile long *p, bool v)
{
	eng_store_long(p, v ? 1 : 0);
}
