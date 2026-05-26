//  MIT License – Modified for Mandatory Attribution
//
//  Copyright(c) 2025 Sergio Paludo
//
//  github.com/sergiocupa
//
//  Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files,
//  to use, copy, modify, merge, publish, distribute, and sublicense the software, including for commercial purposes, provided that:
//
//     01. The original author's credit is retained in all copies of the source code;
//     02. The original author's credit is included in any code generated, derived, or distributed from this software, including templates, libraries, or code - generating scripts.
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED.


#ifndef TASK_HANDLER_H
#define TASK_HANDLER_H

#ifdef __cplusplus
extern "C" {
#endif

    #include "atomics.h"
    #include "ring_queue.h"
    #include "thread_wait.h"


    /* ── Configuração ── */

    #ifndef INITIAL_SHARD_COUNT
    #define INITIAL_SHARD_COUNT     8
    #endif
    #ifndef SHARD_CAPACITY
    #define SHARD_CAPACITY          1024
    #endif

    /* Pressão: se shard ultrapassar este % de ocupação, acorda o monitor */
    #ifndef PRESSURE_THRESHOLD
    #define PRESSURE_THRESHOLD      50
    #endif

    /* Quantos shards o monitor cria por vez */
    #ifndef EXPAND_BATCH
    #define EXPAND_BATCH            2
    #endif

    #ifndef POOL_MAX_SHARDS
    #define POOL_MAX_SHARDS         1024
    #endif
    #ifndef POOL_MAX_WORKERS
    #define POOL_MAX_WORKERS        1024
    #endif
    #ifndef PROACTIVE_SHARD_THRESHOLD
    #define PROACTIVE_SHARD_THRESHOLD   50
    #endif
    #ifndef WORKER_BUSY_THRESHOLD
    #define WORKER_BUSY_THRESHOLD       70
    #endif
    #ifndef MONITOR_TICK_US
    #define MONITOR_TICK_US             10000
    #endif
    /* Ticks de cooldown apos uma expansao (evita expansao em rajada) */
    #ifndef EXPAND_COOLDOWN_TICKS
    #define EXPAND_COOLDOWN_TICKS       5
    #endif

    /* ── Cache line padding ── */
    #define CACHE_LINE 64

    typedef struct _Task
    {
        void (*fn)(void*);
        void* arg;
    }
    Task;


    typedef struct _Shard
    {
        Task*     buffer;
        RingQueue ring;
        char      pad[64];
    }
    Shard;


    typedef struct {
        xwait_t     wait;           /* handle para sleep/wake */
        xatomic_int sleeping;       /* 1 = dormindo, 0 = acordado */
    } WorkerCtx;


    typedef struct _ShardedPool
    {
        /* Shards */
        Shard**      shards;
        int          shard_capacity;
        xatomic_int  shard_count;

        /* Índices globais */
        xatomic_int  submit_idx;
        xatomic_int  running;
        xatomic_int  pending;
        xatomic_int  submit_fail_count;

        /* Workers */
        WorkerCtx**  workers;
        int          worker_count;
        int          worker_capacity;
        xatomic_int  active_workers;

        #ifdef _WIN32
           HANDLE*     threads;
        #else
           pthread_t*  threads;
        #endif

        void**       worker_arg_ptrs;

        /* Monitor de expansão */
        xwait_t      monitor_wait;           /* sleep/wake do monitor */
        xatomic_int  monitor_sleeping;       /* 1 = dormindo */
        xatomic_int  expand_requested;       /* 1 = alguém pediu expansão */
        xatomic_int  expand_count;           /* total de shards criados pelo monitor */

        #ifdef _WIN32
           HANDLE      monitor_thread;
        #else
           pthread_t   monitor_thread;
        #endif

    } ShardedPool;



    XPLATBASE_API boolean pool_submit(ShardedPool* pool, void (*fn)(void*), void* arg);
    XPLATBASE_API boolean pool_init(ShardedPool* pool);
    XPLATBASE_API void    pool_shutdown(ShardedPool* pool);



#ifdef __cplusplus
}
#endif

#endif /* TASK_HANDLER */
