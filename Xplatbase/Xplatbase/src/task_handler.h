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
    #ifndef MAX_SHARD_COUNT
    #define MAX_SHARD_COUNT         64
    #endif
    #ifndef SHARD_CAPACITY
    #define SHARD_CAPACITY          1024
    #endif
    #ifndef MAX_WORKER_COUNT
    #define MAX_WORKER_COUNT        64
    #endif

    /* Pressão: se shard ultrapassar este % de ocupação, acorda o monitor */
    #ifndef PRESSURE_THRESHOLD
    #define PRESSURE_THRESHOLD      50
    #endif

    /* Quantos shards o monitor cria por vez */
    #ifndef EXPAND_BATCH
    #define EXPAND_BATCH            2
    #endif



    #define TASKS_PER_QUEUE  1024

    /* ── Configuração ── */
    #define SHARD_COUNT     4
    #define TASKS_PER_SHARD 1024
    #define SHARD_MASK      (TASKS_PER_SHARD - 1)
    #define TOTAL_TASKS     10000
    #define WORKER_COUNT    8
    #define QUEUE_COUNT     WORKER_COUNT

    /* ── Cache line padding ── */
    #define CACHE_LINE 64

    typedef struct _Task
    {
        void (*fn)(void*);
        void* arg;
    }
    Task;


    xatomic_int x = 0;


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
        Shard* shards[MAX_SHARD_COUNT];
        xatomic_int  shard_count;

        /* Índices globais */
        xatomic_int  submit_idx;
        xatomic_int  running;
        xatomic_int  pending;
        xatomic_int  submit_fail_count;

        /* Workers */
        WorkerCtx   workers[MAX_WORKER_COUNT];
        int         worker_count;

        #ifdef _WIN32
           HANDLE      threads[MAX_WORKER_COUNT];
        #else
           pthread_t   threads[MAX_WORKER_COUNT];
        #endif

        void* worker_args[MAX_WORKER_COUNT][2];

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


    typedef struct 
    {
        RingQueue ring;
        Task      buffer[TASKS_PER_QUEUE];
        WorkerCtx worker;
    } Queue;


    typedef struct 
    {
        Queue       queues[QUEUE_COUNT];
        xatomic_int submit_idx;
        xatomic_int running;
    } Pool;



    inline boolean pool_submit(ShardedPool* pool, void (*fn)(void*), void* arg);
    inline boolean pool_init(ShardedPool* pool, int worker_count);
    inline void    pool_shutdown(ShardedPool* pool);



#ifdef __cplusplus
}
#endif

#endif /* TASK_HANDLER */