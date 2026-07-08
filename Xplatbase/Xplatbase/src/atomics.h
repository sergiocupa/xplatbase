//  MIT License – Modified for Mandatory Attribution
//
//  Copyright(c) 2025 Sergio Paludo
//
//  github.com/sergiocupa
//
//  Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files,
//  to use, copy, modify, merge, publish, distribute, and sublicense the software, including for commercial purposes, provided that:
//
//     01. The original author’s credit is retained in all copies of the source code;
//     02. The original author’s credit is included in any code generated, derived, or distributed from this software, including templates, libraries, or code - generating scripts.
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED.


#ifndef ATOMICS_H
#define ATOMICS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "../include/xplatbase.h"

#ifdef XPLATBASE_WIN

	typedef volatile LONG    xatomic_int;
	typedef volatile LONG    xatomic_uint32;
	typedef volatile LONG64  xatomic_int64;
	typedef volatile LONG64  xatomic_uint64;
	typedef volatile PVOID   xatomic_ptr;

	STATIC_INLINE void atomic_initialize_inline(xatomic_int* s, int val)
	{
	    *s = (LONG)val;
	}

	STATIC_INLINE void atomic_set_inline(xatomic_int* s, int val)
	{
	    InterlockedExchange(s, (LONG)val);
	}

	STATIC_INLINE int atomic_get_inline(xatomic_int* s)
	{
	    return (int)ReadAcquire(s);
	}

	STATIC_INLINE void* atomic_get_ptr_inline(xatomic_ptr* s)
	{
	    return ReadPointerAcquire((PVOID volatile const*)s);
	}

	STATIC_INLINE void atomic_set_ptr_inline(xatomic_ptr* target, void* value)
	{
	    InterlockedExchangePointer((volatile PVOID*)target, value);
	}

	STATIC_INLINE int atomic_add_inline(xatomic_int* s, int val)
	{
	    /* Com fence: em x86/x64 gera o mesmo 'lock xadd' do NoFence (custo zero),
	     * mas garante a ordenacao em ARM, onde o NoFence permitiria reordenacao
	     * (ex.: pool_api_enter incrementa api_users e logo le shutdown). */
	    return (int)InterlockedExchangeAdd(s, (LONG)val);
	}

	STATIC_INLINE int atomic_sub_inline(xatomic_int* s, int val)
	{
	    return (int)InterlockedExchangeAdd(s, -(LONG)val);
	}

	STATIC_INLINE int atomic_cas_inline(xatomic_int* s, int* expected, int desired)
	{
	    LONG old = InterlockedCompareExchange(s, (LONG)desired, (LONG)*expected);
	    if (old == (LONG)*expected)
	        return 1;
	    *expected = (int)old;
	    return 0;
	}

	STATIC_INLINE void atomic_u32_set_inline(xatomic_uint32* s, uint32_t val)
	{
	    InterlockedExchange(s, (LONG)val);
	}

	STATIC_INLINE uint32_t atomic_u32_get_inline(xatomic_uint32* s)
	{
	    return (uint32_t)ReadAcquire(s);
	}

	STATIC_INLINE uint32_t atomic_u32_add_inline(xatomic_uint32* s, uint32_t val)
	{
	    /* idem atomic_add_inline: fence sem custo em x86/x64, correto em ARM. */
	    return (uint32_t)InterlockedExchangeAdd(s, (LONG)val);
	}

	STATIC_INLINE int atomic_u32_cas_inline(xatomic_uint32* s, uint32_t* expected, uint32_t desired)
	{
	    LONG old = InterlockedCompareExchange(s, (LONG)desired, (LONG)*expected);
	    if ((uint32_t)old == *expected)
	        return 1;
	    *expected = (uint32_t)old;
	    return 0;
	}

	STATIC_INLINE void atomic_set64_inline(xatomic_int64* s, int64_t val)
	{
	    InterlockedExchange64(s, (LONG64)val);
	}

	STATIC_INLINE int64_t atomic_get64_inline(xatomic_int64* s)
	{
	    return (int64_t)ReadAcquire64(s);
	}

	STATIC_INLINE int64_t atomic_add64_inline(xatomic_int64* s, int64_t val)
	{
	    return (int64_t)InterlockedExchangeAdd64(s, (LONG64)val);
	}

	STATIC_INLINE int64_t atomic_sub64_inline(xatomic_int64* s, int64_t val)
	{
	    return (int64_t)InterlockedExchangeAdd64(s, -(LONG64)val);
	}

	STATIC_INLINE int atomic_cas64_inline(xatomic_int64* s, int64_t* expected, int64_t desired)
	{
	    LONG64 old = InterlockedCompareExchange64(s, (LONG64)desired, (LONG64)*expected);
	    if (old == (LONG64)*expected)
	        return 1;
	    *expected = (int64_t)old;
	    return 0;
	}

	STATIC_INLINE void atomic_u64_set_inline(xatomic_uint64* s, uint64_t val)
	{
	    InterlockedExchange64(s, (LONG64)val);
	}

	STATIC_INLINE uint64_t atomic_u64_get_inline(xatomic_uint64* s)
	{
	    return (uint64_t)ReadAcquire64(s);
	}

	STATIC_INLINE uint64_t atomic_u64_add_inline(xatomic_uint64* s, uint64_t val)
	{
	    return (uint64_t)InterlockedExchangeAdd64(s, (LONG64)val);
	}

	STATIC_INLINE int atomic_u64_cas_inline(xatomic_uint64* s, uint64_t* expected, uint64_t desired)
	{
	    LONG64 old = InterlockedCompareExchange64(s, (LONG64)desired, (LONG64)*expected);
	    if ((uint64_t)old == *expected)
	        return 1;
	    *expected = (uint64_t)old;
	    return 0;
	}

	STATIC_INLINE int atomic_cas_ptr_inline(xatomic_ptr* target, void** expected, void* desired)
	{
	    PVOID old = InterlockedCompareExchangePointer((volatile PVOID*)target, desired, *expected);
	    if (old == *expected)
	        return 1;
	    *expected = old;
	    return 0;
	}

#else

    #include <stdatomic.h>
    #include <stdint.h>

	typedef _Atomic int       xatomic_int;
	typedef _Atomic uint32_t  xatomic_uint32;
	typedef _Atomic int64_t   xatomic_int64;
	typedef _Atomic uint64_t  xatomic_uint64;
	typedef _Atomic(void*)    xatomic_ptr;

	STATIC_INLINE void atomic_initialize_inline(xatomic_int* s, int val)
	{
	    atomic_init(s, val);
	}

	STATIC_INLINE void atomic_set_inline(xatomic_int* s, int val)
	{
	    atomic_store_explicit(s, val, memory_order_release);
	}

	STATIC_INLINE int atomic_get_inline(xatomic_int* s)
	{
	    return atomic_load_explicit(s, memory_order_acquire);
	}

	STATIC_INLINE int atomic_sub_inline(xatomic_int* s, int val)
	{
	    return atomic_fetch_sub_explicit(s, val, memory_order_relaxed);
	}

	STATIC_INLINE int atomic_add_inline(xatomic_int* s, int val)
	{
	    return atomic_fetch_add_explicit(s, val, memory_order_relaxed);
	}

	STATIC_INLINE int atomic_cas_inline(xatomic_int* s, int* expected, int desired)
	{
	    return atomic_compare_exchange_strong_explicit(s, expected, desired, memory_order_acq_rel, memory_order_relaxed);
	}

	STATIC_INLINE void atomic_u32_set_inline(xatomic_uint32* s, uint32_t val)
	{
	    atomic_store_explicit(s, val, memory_order_release);
	}

	STATIC_INLINE uint32_t atomic_u32_get_inline(xatomic_uint32* s)
	{
	    return atomic_load_explicit(s, memory_order_acquire);
	}

	STATIC_INLINE uint32_t atomic_u32_add_inline(xatomic_uint32* s, uint32_t val)
	{
	    return atomic_fetch_add_explicit(s, val, memory_order_relaxed);
	}

	STATIC_INLINE int atomic_u32_cas_inline(xatomic_uint32* s, uint32_t* expected, uint32_t desired)
	{
	    return atomic_compare_exchange_strong_explicit(
	        s, expected, desired, memory_order_acq_rel, memory_order_relaxed);
	}

	STATIC_INLINE void atomic_set64_inline(xatomic_int64* s, int64_t val)
	{
	    atomic_store_explicit(s, val, memory_order_release);
	}

	STATIC_INLINE int64_t atomic_get64_inline(xatomic_int64* s)
	{
	    return atomic_load_explicit(s, memory_order_acquire);
	}

	STATIC_INLINE int64_t atomic_add64_inline(xatomic_int64* s, int64_t val)
	{
	    return atomic_fetch_add_explicit(s, val, memory_order_relaxed);
	}

	STATIC_INLINE int64_t atomic_sub64_inline(xatomic_int64* s, int64_t val)
	{
	    return atomic_fetch_sub_explicit(s, val, memory_order_relaxed);
	}

	STATIC_INLINE int atomic_cas64_inline(xatomic_int64* s, int64_t* expected, int64_t desired)
	{
	    return atomic_compare_exchange_strong_explicit(s, expected, desired, memory_order_acq_rel, memory_order_relaxed);
	}

	STATIC_INLINE void atomic_u64_set_inline(xatomic_uint64* s, uint64_t val)
	{
	    atomic_store_explicit(s, val, memory_order_release);
	}

	STATIC_INLINE uint64_t atomic_u64_get_inline(xatomic_uint64* s)
	{
	    return atomic_load_explicit(s, memory_order_acquire);
	}

	STATIC_INLINE uint64_t atomic_u64_add_inline(xatomic_uint64* s, uint64_t val)
	{
	    return atomic_fetch_add_explicit(s, val, memory_order_relaxed);
	}

	STATIC_INLINE int atomic_u64_cas_inline(xatomic_uint64* s, uint64_t* expected, uint64_t desired)
	{
	    return atomic_compare_exchange_strong_explicit(
	        s, expected, desired, memory_order_acq_rel, memory_order_relaxed);
	}

	STATIC_INLINE void* atomic_get_ptr_inline(xatomic_ptr* s)
	{
	    return atomic_load_explicit(s, memory_order_acquire);
	}

	STATIC_INLINE void atomic_set_ptr_inline(xatomic_ptr* target, void* value)
	{
	    atomic_store_explicit(target, value, memory_order_release);
	}

	STATIC_INLINE int atomic_cas_ptr_inline(xatomic_ptr* target, void** expected, void* desired)
	{
	    return atomic_compare_exchange_strong_explicit(target, expected, desired, memory_order_acq_rel, memory_order_relaxed);
	}

#endif



/* Versoes com linkage externa (extern + dllexport em Release), para consumo
 * fora da lib (DLL). Apenas encaminham para a variante _inline (definicao
 * unica em atomics.c) -- uso interno da lib deve preferir sempre os _inline. */
XPLATBASE_API void     atomic_initialize(xatomic_int* s, int val);
XPLATBASE_API void     atomic_set(xatomic_int* s, int val);
XPLATBASE_API int      atomic_get(xatomic_int* s);
XPLATBASE_API void*    atomic_get_ptr(xatomic_ptr* s);
XPLATBASE_API void     atomic_set_ptr(xatomic_ptr* target, void* value);
XPLATBASE_API int      atomic_add(xatomic_int* s, int val);
XPLATBASE_API int      atomic_sub(xatomic_int* s, int val);
XPLATBASE_API int      atomic_cas(xatomic_int* s, int* expected, int desired);

XPLATBASE_API void     atomic_u32_set(xatomic_uint32* s, uint32_t val);
XPLATBASE_API uint32_t atomic_u32_get(xatomic_uint32* s);
XPLATBASE_API uint32_t atomic_u32_add(xatomic_uint32* s, uint32_t val);
XPLATBASE_API int      atomic_u32_cas(xatomic_uint32* s, uint32_t* expected, uint32_t desired);

XPLATBASE_API void     atomic_set64(xatomic_int64* s, int64_t val);
XPLATBASE_API int64_t  atomic_get64(xatomic_int64* s);
XPLATBASE_API int64_t  atomic_add64(xatomic_int64* s, int64_t val);
XPLATBASE_API int64_t  atomic_sub64(xatomic_int64* s, int64_t val);
XPLATBASE_API int      atomic_cas64(xatomic_int64* s, int64_t* expected, int64_t desired);

XPLATBASE_API void     atomic_u64_set(xatomic_uint64* s, uint64_t val);
XPLATBASE_API uint64_t atomic_u64_get(xatomic_uint64* s);
XPLATBASE_API uint64_t atomic_u64_add(xatomic_uint64* s, uint64_t val);
XPLATBASE_API int      atomic_u64_cas(xatomic_uint64* s, uint64_t* expected, uint64_t desired);

XPLATBASE_API int      atomic_cas_ptr(xatomic_ptr* target, void** expected, void* desired);



#ifdef __cplusplus
}
#endif

#endif /* ATOMICS */
