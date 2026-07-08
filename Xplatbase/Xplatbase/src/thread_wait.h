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


#ifndef THREAD_WAIT_H
#define THREAD_WAIT_H

#ifdef __cplusplus
extern "C" {
#endif

    #include "../include/xplatbase.h"
    #include "atomics.h"
    #include "thread_handler.h"


    #ifdef XPLATBASE_WIN

        typedef CONDITION_VARIABLE xcond_t;

        typedef struct {
            xatomic_int signal;   /* 0 = dormindo, 1 = sinalizado (WaitOnAddress/WakeByAddress) */
        } xwait_t;

    #else

        typedef pthread_cond_t xcond_t;

        typedef struct {
            atomic_int futex_val;   /* 0 = sleeping, 1 = signaled */
            xatomic_int signal;
        } xwait_t;

    #endif


    XPLATBASE_API boolean thread_wait_init(boolean fast_mode);
    XPLATBASE_API void    thread_wait_shutdown(void);

    /* Sem estado global (so operam sobre o 'w' do chamador) -> seguras p/ inline. */

#ifdef XPLATBASE_WIN

    /* No-op: sem handle para fechar. */
    STATIC_INLINE void thread_wait_destroy_inline(xwait_t* w)
    {
        (void)w;
    }

    /* Reseta o sinal para 0 antes de entrar na fase de espera. */
    STATIC_INLINE void thread_wait_prepare_inline(xwait_t* w)
    {
        atomic_set_inline(&w->signal, 0);
    }

    /* Dorme ate ser acordada (sem timeout). */
    STATIC_INLINE void thread_wait_sleep_inline(xwait_t* w)
    {
        LONG expected = 0;
        while (atomic_get_inline(&w->signal) == 0)
            WaitOnAddress(&w->signal, &expected, sizeof(LONG), INFINITE);
        atomic_set_inline(&w->signal, 0);
    }

    /* Dorme com timeout (microsegundos). true = acordou por wake, false = timeout.
     * WaitOnAddress: dorme apenas se *address == *compare no momento da chamada,
     * portanto nao ha race entre wake-antes-de-sleep. */
    STATIC_INLINE boolean thread_wait_sleep_for_inline(xwait_t* w, long long timeout_us)
    {
        LONG  expected = 0;
        DWORD ms       = (DWORD)(timeout_us / 1000);
        if (ms == 0) ms = 1;

        BOOL woken = WaitOnAddress(&w->signal, &expected, sizeof(LONG), ms);
        atomic_set_inline(&w->signal, 0);
        return woken != FALSE;
    }

    /* Acorda a thread imediatamente. */
    STATIC_INLINE void thread_wait_wake_inline(xwait_t* w)
    {
        atomic_set_inline(&w->signal, 1);
        WakeByAddressSingle((PVOID)&w->signal);
    }

    #pragma comment(lib, "Synchronization.lib")

#elif defined(__linux__)

    #include <linux/futex.h>
    #include <sys/syscall.h>
    #include <unistd.h>
    #include <time.h>
    #include <errno.h>

    /* Libera recursos da instancia (no-op no Linux) */
    STATIC_INLINE void thread_wait_destroy_inline(xwait_t* w)
    {
        (void)w;
    }

    /* Registrar thread antes de usar */
    STATIC_INLINE void thread_wait_prepare_inline(xwait_t* w)
    {
        atomic_store_explicit(&w->futex_val, 0, memory_order_relaxed);
    }

    /* Dorme ate ser acordada - loop por causa de spurious wakeup (EINTR, etc) */
    STATIC_INLINE void thread_wait_sleep_inline(xwait_t* w)
    {
        while (atomic_load_explicit(&w->futex_val, memory_order_acquire) == 0)
            syscall(SYS_futex, &w->futex_val, FUTEX_WAIT | FUTEX_PRIVATE_FLAG,
                0, NULL, NULL, 0);

        atomic_store_explicit(&w->futex_val, 0, memory_order_relaxed);
    }

    /* Dorme com timeout (microsegundos). true = acordou por wake, false = timeout */
    STATIC_INLINE boolean thread_wait_sleep_for_inline(xwait_t* w, long long timeout_us)
    {
        struct timespec ts;
        ts.tv_sec = timeout_us / 1000000LL;
        ts.tv_nsec = (timeout_us % 1000000LL) * 1000LL;

        while (atomic_load_explicit(&w->futex_val, memory_order_acquire) == 0) {
            long ret = syscall(SYS_futex, &w->futex_val, FUTEX_WAIT | FUTEX_PRIVATE_FLAG,
                0, &ts, NULL, 0);

            if (ret == -1 && errno == ETIMEDOUT)
                return false;
        }

        atomic_store_explicit(&w->futex_val, 0, memory_order_relaxed);
        return true;
    }

    /* Acorda a thread */
    STATIC_INLINE void thread_wait_wake_inline(xwait_t* w)
    {
        atomic_store_explicit(&w->futex_val, 1, memory_order_release);
        syscall(SYS_futex, &w->futex_val, FUTEX_WAKE | FUTEX_PRIVATE_FLAG,
            1, NULL, NULL, 0);
    }

#else
    #error "Plataforma nao suportada. Requer Windows ou Linux."
#endif




    STATIC_INLINE void thread_cond_init_inline(xcond_t* cond)
    {
#ifdef XPLATBASE_WIN
        InitializeConditionVariable(cond);
#else
        pthread_cond_init(cond, NULL);
#endif
    }



    STATIC_INLINE void thread_cond_signal_inline(xcond_t* cond)
    {
#ifdef XPLATBASE_WIN
        WakeConditionVariable(cond);
#else
        pthread_cond_signal(cond);
#endif
    }



    STATIC_INLINE void thread_cond_wait_inline(xcond_t* cond, xmutex_t* mutex)
    {
#ifdef XPLATBASE_WIN
        SleepConditionVariableCS(cond, mutex, INFINITE);
#else
        pthread_cond_wait(cond, mutex);
#endif
    }



    STATIC_INLINE void thread_cond_destroy_inline(xcond_t* cond)
    {
#ifdef XPLATBASE_WIN
        (void)cond;
#else
        pthread_cond_destroy(cond);
#endif
    }


    /* Versoes com linkage externa (extern + dllexport em Release), para consumo
     * fora da lib (DLL). Apenas encaminham para a variante _inline (definicao
     * unica em thread_wait.c) -- uso interno da lib deve preferir os _inline. */
    XPLATBASE_API void    thread_wait_destroy(xwait_t* w);
    XPLATBASE_API void    thread_wait_prepare(xwait_t* w);
    XPLATBASE_API void    thread_wait_sleep(xwait_t* w);
    XPLATBASE_API boolean thread_wait_sleep_for(xwait_t* w, long long timeout_us);
    XPLATBASE_API void    thread_wait_wake(xwait_t* w);

    XPLATBASE_API void    thread_cond_init(xcond_t* cond);
    XPLATBASE_API void    thread_cond_signal(xcond_t* cond);
    XPLATBASE_API void    thread_cond_wait(xcond_t* cond, xmutex_t* mutex);
    XPLATBASE_API void    thread_cond_destroy(xcond_t* cond);


#ifdef __cplusplus
}
#endif

#endif /* THREAD_WAIT */








