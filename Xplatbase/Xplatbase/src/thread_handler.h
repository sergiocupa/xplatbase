//  MIT License � Modified for Mandatory Attribution
//  
//  Copyright(c) 2025 Sergio Paludo
//
//  github.com/sergiocupa
//  
//  Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files, 
//  to use, copy, modify, merge, publish, distribute, and sublicense the software, including for commercial purposes, provided that:
//  
//     01. The original author�s credit is retained in all copies of the source code;
//     02. The original author�s credit is included in any code generated, derived, or distributed from this software, including templates, libraries, or code - generating scripts.
//  
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED.


#ifndef THREAD_HAND_H
#define THREAD_HAND_H

#include "../include/xplatbase.h"

#ifndef XPLATBASE_WIN
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <stdatomic.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifdef XPLATBASE_WIN
    typedef HANDLE xthread_handle_t;
    typedef DWORD xthread_result_t;
    typedef DWORD (WINAPI xthread_func_t)(void* arg);
    typedef volatile LONG64 xthread_atomic64;
    typedef CRITICAL_SECTION xmutex_t;
#else
    typedef pthread_t xthread_handle_t;
    typedef void* xthread_result_t;
    typedef void* xthread_func_t(void* arg);
    typedef _Atomic long long xthread_atomic64;
    typedef pthread_mutex_t xmutex_t;
#endif

	typedef struct
	{
		void* arg;
		xthread_func_t* func;
	}
	ThreadFunc;

	typedef struct
	{
		ThreadFunc Func;
		xthread_handle_t Thr;
		int Joined;
	}
	Thread;


	typedef void (*CreatedThread)(const Thread* thr);



	Thread* thread_create(xthread_func_t* func, void* arg, int* status);
	void thread_join(Thread** t);
	void thread_init(CreatedThread created_thread, CreatedThread ended_thread);







    STATIC_INLINE void thread_mutex_init_inline(xmutex_t* mutex)
    {
#ifdef XPLATBASE_WIN
        InitializeCriticalSection(mutex);
#else
        pthread_mutex_init(mutex, NULL);
#endif
    }



    STATIC_INLINE void thread_mutex_lock_inline(xmutex_t* mutex)
    {
#ifdef XPLATBASE_WIN
        EnterCriticalSection(mutex);
#else
        pthread_mutex_lock(mutex);
#endif
    }



    STATIC_INLINE void thread_mutex_unlock_inline(xmutex_t* mutex)
    {
#ifdef XPLATBASE_WIN
        LeaveCriticalSection(mutex);
#else
        pthread_mutex_unlock(mutex);
#endif
    }



    STATIC_INLINE void thread_mutex_destroy_inline(xmutex_t* mutex)
    {
#ifdef XPLATBASE_WIN
        DeleteCriticalSection(mutex);
#else
        pthread_mutex_destroy(mutex);
#endif
    }



    STATIC_INLINE void thread_yield_inline(void)
    {
#ifdef XPLATBASE_WIN
        SwitchToThread();
#else
        sched_yield();
#endif
    }



    STATIC_INLINE void thread_sleep0_inline(void)
    {
#ifdef XPLATBASE_WIN
        Sleep(0);
#else
        struct timespec z = { 0, 0 };
        nanosleep(&z, NULL);
#endif
    }



    STATIC_INLINE long long thread_atomic64_load_inline(xthread_atomic64* p)
    {
#ifdef XPLATBASE_WIN
        return (long long)(*p);
#else
        return atomic_load_explicit(p, memory_order_relaxed);
#endif
    }



    STATIC_INLINE void thread_atomic64_store_inline(xthread_atomic64* p, long long v)
    {
#ifdef XPLATBASE_WIN
        *p = (LONG64)v;
#else
        atomic_store_explicit(p, v, memory_order_relaxed);
#endif
    }



    STATIC_INLINE void thread_fence_inline(void)
    {
#ifdef XPLATBASE_WIN
        MemoryBarrier();
#else
        atomic_thread_fence(memory_order_seq_cst);
#endif
    }



    STATIC_INLINE int thread_atomic64_cas_inline(xthread_atomic64* p, long long expected, long long desired)
    {
#ifdef XPLATBASE_WIN
        return InterlockedCompareExchange64(p, (LONG64)desired, (LONG64)expected) == (LONG64)expected;
#else
        long long e = expected;
        return atomic_compare_exchange_strong_explicit(p, &e, desired, memory_order_seq_cst, memory_order_relaxed);
#endif
    }



    /* Versoes com linkage externa (extern + dllexport em Release), para consumo
     * fora da lib (DLL). Apenas encaminham para a variante _inline (definicao
     * unica em thread_handler.c) -- uso interno da lib deve preferir os _inline. */
    XPLATBASE_API void      thread_mutex_init(xmutex_t* mutex);
    XPLATBASE_API void      thread_mutex_lock(xmutex_t* mutex);
    XPLATBASE_API void      thread_mutex_unlock(xmutex_t* mutex);
    XPLATBASE_API void      thread_mutex_destroy(xmutex_t* mutex);
    XPLATBASE_API void      thread_yield(void);
    XPLATBASE_API void      thread_sleep0(void);
    XPLATBASE_API long long thread_atomic64_load(xthread_atomic64* p);
    XPLATBASE_API void      thread_atomic64_store(xthread_atomic64* p, long long v);
    XPLATBASE_API void      thread_fence(void);
    XPLATBASE_API int       thread_atomic64_cas(xthread_atomic64* p, long long expected, long long desired);


#ifdef __cplusplus
}
#endif

#endif /* THREAD_HAND */