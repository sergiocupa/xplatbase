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


#ifndef THREAD_WAIT_H
#define THREAD_WAIT_H

#ifdef __cplusplus
extern "C" {
#endif

    #include "../include/xplatbase.h"


    #ifdef XPLATBASE_WIN

        typedef struct {
            DWORD thread_id;
        } xwait_t;

    #else 

        typedef struct {
            atomic_int futex_val;   /* 0 = sleeping, 1 = signaled */
        } xwait_t;

    #endif


    inline bool thread_wait_init(void);
    inline void thread_wait_prepare(xwait_t* w);
    inline void thread_wait_sleep(xwait_t* w);
    inline bool thread_wait_sleep_for(xwait_t* w, long long timeout_us);
    inline void thread_wait_wake(xwait_t* w);


#ifdef __cplusplus
}
#endif

#endif /* THREAD_WAIT */








