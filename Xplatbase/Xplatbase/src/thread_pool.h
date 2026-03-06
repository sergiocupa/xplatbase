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


#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#ifdef __cplusplus
extern "C" {
#endif

    #include "../include/xplatbase.h"


    typedef enum
    {
        POOLING_NONE      = 0,
        POOLING_LIMIT     = 1,
        POOLING_LIMIT_SEC = 2,
        POOLING_EXHAUSTED = 3
    }
    PoolingMode;


    typedef enum
    {
        THR_NONE = 0,
        THR_IDLE = 1,
        THR_BUSY = 2
    }
    ThreadStatus;

    typedef enum
    {
        TASK_NONE = 0,
        TASK_IDLE = 1,
        TASK_RUNNING = 2
    }
    TaskStatus;


    


    typedef void (*TaskFunc)(void*);
    typedef void (*WaitEventFunc)(LightEvent*);
    typedef void (*SignalEventFunc)(LightEvent*);
    typedef void (*SignalAllEventFunc)(LightEvent*);


    typedef struct
    {
        void*        Thread;
        LightEvent   Event;
        ThreadStatus Status;
    }
    ThreadInfo;


    typedef struct
    {
        uint64 Max;
        uint64 Count;
        ThreadInfo** Items;
    }
    ThreadList;

    typedef struct 
    {
        TaskStatus  Status;
        TaskFunc    Func;
        void*       Arg;
        ThreadInfo* Thr;
    } 
    TaskInstance;

    typedef struct
    {
        uint64         StartOfBlock;
        uint64         Position;
        uint64         Limit;
        uint64         LimitSec;
        uint64         End;
        uint64         Max;
        TaskInstance** Tasks;
    }
    TaskList;


    typedef struct
    {
        bool               Initialized;
        bool               Running;
        ThreadList         ThrQueue;
        TaskList           TaskQueue;

        PoolingMode        EventMode;

        WaitEventFunc      WaitEvent;
        SignalEventFunc    SignalEvent;
        SignalAllEventFunc SignalAllEvent;

        void*              MonitorThr;
        bool               IsPooling;
        LightEvent         PoolingEvent;
        LightEvent         PoolingWait;
    }
    ThreadPool;





#ifdef __cplusplus
}
#endif

#endif /* THREAD_POOL */








