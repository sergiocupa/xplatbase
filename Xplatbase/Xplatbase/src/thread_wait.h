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
    XPLATBASE_API void    thread_wait_destroy(xwait_t* w);
    XPLATBASE_API void    thread_wait_prepare(xwait_t* w);
    XPLATBASE_API void    thread_wait_sleep(xwait_t* w);
    XPLATBASE_API boolean thread_wait_sleep_for(xwait_t* w, long long timeout_us);
    XPLATBASE_API void    thread_wait_wake(xwait_t* w);




    XTHREAD_INLINE void thread_cond_init(xcond_t* cond)
    {
#ifdef XPLATBASE_WIN
        InitializeConditionVariable(cond);
#else
        pthread_cond_init(cond, NULL);
#endif
    }



    XTHREAD_INLINE void thread_cond_signal(xcond_t* cond)
    {
#ifdef XPLATBASE_WIN
        WakeConditionVariable(cond);
#else
        pthread_cond_signal(cond);
#endif
    }



    XTHREAD_INLINE void thread_cond_wait(xcond_t* cond, xmutex_t* mutex)
    {
#ifdef XPLATBASE_WIN
        SleepConditionVariableCS(cond, mutex, INFINITE);
#else
        pthread_cond_wait(cond, mutex);
#endif
    }



    XTHREAD_INLINE void thread_cond_destroy(xcond_t* cond)
    {
#ifdef XPLATBASE_WIN
        (void)cond;
#else
        pthread_cond_destroy(cond);
#endif
    }


#ifdef __cplusplus
}
#endif

#endif /* THREAD_WAIT */








