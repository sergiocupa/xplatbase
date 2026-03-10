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

    typedef struct
    {
        void (*fn)(void* arg);
        void* arg;
    }
    Task;

    typedef struct {
        RingQueue ring;
        Task      buffer[TASKS_PER_QUEUE];
        WorkerCtx worker;
    } Queue;

    typedef struct {
        Queue       queues[QUEUE_COUNT];
        xatomic_int submit_idx;
        xatomic_int running;
    } Pool;


    xatomic_int x = 0;

    /* ── Shard: fila independente, tudo atômico, sem lock ── */
    typedef struct {
        _Alignas(CACHE_LINE) Task        buffer[TASKS_PER_SHARD];
        _Alignas(CACHE_LINE) xatomic_int  head;   /* consumidores */
        _Alignas(CACHE_LINE) xatomic_int  tail;   /* produtores */
        _Alignas(CACHE_LINE) xatomic_int  count;  /* tarefas disponíveis */
    } Shard;

    /* ── Pool ── */
    typedef struct {
        Shard       shards[SHARD_COUNT];
        xatomic_int  submit_idx;    /* round-robin para submit */
        xatomic_int  running;       /* flag de shutdown */
        xatomic_int  pending;       /* total de tarefas pendentes */
    } ShardedPool;




    inline boolean pool_submit(ShardedPool* pool, void (*fn)(void*), void* arg);
    inline boolean pool_init(ShardedPool* pool, int worker_count);
    inline void    pool_shutdown(ShardedPool* pool);



#ifdef __cplusplus
}
#endif

#endif /* TASK_HANDLER */