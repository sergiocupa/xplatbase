//  MIT License - Modified for Mandatory Attribution
//
//  Copyright(c) 2025 Sergio Paludo
//
//  github.com/sergiocupa
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED.

#ifndef MEMORY_POOL_H
#define MEMORY_POOL_H

#ifdef __cplusplus
extern "C" {
#endif

    #include "../include/xplatbase.h"
    #include "thread_handler.h"

    typedef struct MemBuffer
    {
        void*  Ptr;
        uint64 Size;
    }
    MemBuffer;

    typedef struct MemPoolStats
    {
        uint64 lanes_created;
        uint64 lanes_destroyed;
        uint64 alloc_count;
        uint64 free_count;
        uint64 sync_refills;
        uint64 async_refills;
        uint64 async_requests;
        uint64 remote_frees;
    }
    MemPoolStats;

    void memop_init(void);
    void memop_shutdown(void);

    MemBuffer memop_alloc(uint64 size);
    void memop_free(MemBuffer* buffer);

    void memop_get_stats(MemPoolStats* out_stats);
    void memop_test_reset(void);

    void memop_on_created_thread(const Thread* thr);
    void memop_on_ended_thread(const Thread* thr);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_POOL_H */