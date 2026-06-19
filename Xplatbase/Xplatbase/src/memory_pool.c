#include "memory_pool.h"
#include "thread_wait.h"

#include <stdlib.h>
#include <string.h>

#ifndef MEMOP_CLASS_COUNT
#define MEMOP_CLASS_COUNT 8
#endif
#ifndef MEMOP_REFILL_BLOCKS
#define MEMOP_REFILL_BLOCKS 64
#endif
#ifndef MEMOP_LOW_WATERMARK
#define MEMOP_LOW_WATERMARK 16
#endif
#ifndef MEMOP_REFILL_QUEUE_CAP
#define MEMOP_REFILL_QUEUE_CAP 128
#endif

#ifdef XPLATBASE_WIN
#define MEMOP_TLS __declspec(thread)
#else
#define MEMOP_TLS __thread
#endif

typedef struct MemFreeBlock
{
    struct MemFreeBlock* next;
} MemFreeBlock;

typedef struct MemArea
{
    struct MemArea* next;
    void* memory;
    uint32 block_count;
} MemArea;

typedef struct MemLane MemLane;

typedef struct MemBlockHeader
{
    MemLane* owner;
    uint32 class_id;
    uint32 user_size;
} MemBlockHeader;

typedef struct MemClass
{
    uint32 block_size;
    uint32 stride;
    uint32 free_count;
    uint32 used_count;
    uint32 capacity;
    uint32 low_watermark;
    uint32 refill_amount;
    int refill_pending;
    MemFreeBlock* free_head;
    MemArea* areas;
    xmutex_t lock;
} MemClass;

struct MemLane
{
    int active;
    uint32 id;
    MemClass classes[MEMOP_CLASS_COUNT];
    MemLane* next;
};

typedef struct MemRefillRequest
{
    MemLane* lane;
    uint32 class_id;
} MemRefillRequest;

typedef struct MemPool
{
    int initialized;
    int shutting_down;
    uint32 next_lane_id;
    MemLane* lanes;
    MemLane* retired_lanes;
    MemPoolStats stats;
    xmutex_t lock;
    xcond_t refill_cv;
    Thread* refill_thread;
    int refill_thread_started;
    MemRefillRequest refill_queue[MEMOP_REFILL_QUEUE_CAP];
    uint32 refill_head;
    uint32 refill_tail;
    uint32 refill_count;
} MemPool;

static const uint32 MemClassSizes[MEMOP_CLASS_COUNT] = { 64, 128, 256, 512, 1024, 4096, 8192, 16384 };
static MemPool GlobalMemPool;
static MEMOP_TLS MemLane* g_mem_lane;





static void memop_lock(void)
{
    thread_mutex_lock(&GlobalMemPool.lock);
}

static void memop_unlock(void)
{
    thread_mutex_unlock(&GlobalMemPool.lock);
}

static void memop_class_lock(MemClass* cls)
{
    thread_mutex_lock(&cls->lock);
}

static void memop_class_unlock(MemClass* cls)
{
    thread_mutex_unlock(&cls->lock);
}

static void memop_refill_signal(void)
{
    thread_cond_signal(&GlobalMemPool.refill_cv);
}

static void memop_refill_wait(void)
{
    thread_cond_wait(&GlobalMemPool.refill_cv, &GlobalMemPool.lock);
}





static uint32 memop_align_up(uint32 v, uint32 a)
{
    return (uint32)((v + a - 1u) & ~(a - 1u));
}

static void memop_class_init(MemClass* cls, uint32 block_size)
{
    memset(cls, 0, sizeof(*cls));
    cls->block_size = block_size;
    cls->stride = memop_align_up((uint32)(sizeof(MemBlockHeader) + block_size), 16u);
    cls->low_watermark = MEMOP_LOW_WATERMARK;
    cls->refill_amount = MEMOP_REFILL_BLOCKS;
    thread_mutex_init(&cls->lock);
}

static void memop_class_destroy(MemClass* cls)
{
    thread_mutex_destroy(&cls->lock);
}

static int memop_class_index(uint64 size)
{
    uint32 i;
    for (i = 0; i < MEMOP_CLASS_COUNT; i++)
    {
        if (size <= MemClassSizes[i]) return (int)i;
    }
    return -1;
}

static int memop_refill_class_unlocked(MemLane* lane, uint32 class_id, uint32 count)
{
    uint32 i;
    MemArea* area;
    char* base;
    MemClass* cls;

    if (!lane || class_id >= MEMOP_CLASS_COUNT || count == 0) return 0;

    cls = &lane->classes[class_id];
    area = (MemArea*)malloc(sizeof(MemArea));
    if (!area) return 0;

    area->memory = malloc((size_t)cls->stride * count);
    if (!area->memory)
    {
        free(area);
        return 0;
    }

    area->block_count = count;
    area->next = cls->areas;
    cls->areas = area;

    base = (char*)area->memory;
    for (i = 0; i < count; i++)
    {
        MemBlockHeader* header = (MemBlockHeader*)(base + ((size_t)i * cls->stride));
        MemFreeBlock* block = (MemFreeBlock*)((char*)header + sizeof(MemBlockHeader));
        header->owner = lane;
        header->class_id = class_id;
        header->user_size = cls->block_size;
        block->next = cls->free_head;
        cls->free_head = block;
    }

    cls->free_count += count;
    cls->capacity += count;
    return 1;
}

static int memop_refill_class(MemLane* lane, uint32 class_id, uint32 count)
{
    int ok;
    MemClass* cls;
    if (!lane || class_id >= MEMOP_CLASS_COUNT) return 0;
    cls = &lane->classes[class_id];
    memop_class_lock(cls);
    ok = memop_refill_class_unlocked(lane, class_id, count);
    memop_class_unlock(cls);
    return ok;
}

static void memop_free_areas(MemClass* cls)
{
    MemArea* area = cls->areas;
    while (area)
    {
        MemArea* next = area->next;
        free(area->memory);
        free(area);
        area = next;
    }
    cls->areas = NULL;
    cls->free_head = NULL;
    cls->free_count = 0;
    cls->used_count = 0;
    cls->capacity = 0;
}

static MemLane* memop_lane_create_locked(void)
{
    uint32 i;
    MemLane* lane = (MemLane*)malloc(sizeof(MemLane));
    if (!lane) return NULL;

    memset(lane, 0, sizeof(*lane));
    lane->active = 1;
    lane->id = ++GlobalMemPool.next_lane_id;

    for (i = 0; i < MEMOP_CLASS_COUNT; i++)
    {
        memop_class_init(&lane->classes[i], MemClassSizes[i]);
    }

    lane->next = GlobalMemPool.lanes;
    GlobalMemPool.lanes = lane;
    GlobalMemPool.stats.lanes_created++;
    return lane;
}

static void memop_lane_destroy(MemLane* lane)
{
    uint32 i;
    if (!lane) return;

    for (i = 0; i < MEMOP_CLASS_COUNT; i++)
    {
        memop_free_areas(&lane->classes[i]);
        memop_class_destroy(&lane->classes[i]);
    }
    free(lane);
}

static void memop_destroy_lane_list(MemLane* lane)
{
    while (lane)
    {
        MemLane* next = lane->next;
        memop_lane_destroy(lane);
        lane = next;
    }
}

static void memop_remove_lane_locked(MemLane* lane)
{
    MemLane** cur = &GlobalMemPool.lanes;
    while (*cur)
    {
        if (*cur == lane)
        {
            *cur = lane->next;
            lane->next = NULL;
            return;
        }
        cur = &(*cur)->next;
    }
}

static MemLane* memop_get_lane(void)
{
    MemLane* lane = g_mem_lane;
    if (lane) return lane;

    memop_init();
    memop_lock();
    lane = memop_lane_create_locked();
    memop_unlock();

    g_mem_lane = lane;
    return lane;
}

static int memop_refill_queue_push(MemLane* lane, uint32 class_id)
{
    MemRefillRequest* req;
    if (!lane || class_id >= MEMOP_CLASS_COUNT) return 0;

    memop_lock();
    if (GlobalMemPool.shutting_down || GlobalMemPool.refill_count >= MEMOP_REFILL_QUEUE_CAP)
    {
        memop_unlock();
        return 0;
    }

    req = &GlobalMemPool.refill_queue[GlobalMemPool.refill_tail];
    req->lane = lane;
    req->class_id = class_id;
    GlobalMemPool.refill_tail = (GlobalMemPool.refill_tail + 1u) % MEMOP_REFILL_QUEUE_CAP;
    GlobalMemPool.refill_count++;
    GlobalMemPool.stats.async_requests++;
    memop_refill_signal();
    memop_unlock();
    return 1;
}

static int memop_refill_queue_pop_locked(MemRefillRequest* out)
{
    if (!out) return 0;

    while (!GlobalMemPool.shutting_down && GlobalMemPool.refill_count == 0)
    {
        memop_refill_wait();
    }

    if (GlobalMemPool.refill_count == 0) return 0;

    *out = GlobalMemPool.refill_queue[GlobalMemPool.refill_head];
    GlobalMemPool.refill_head = (GlobalMemPool.refill_head + 1u) % MEMOP_REFILL_QUEUE_CAP;
    GlobalMemPool.refill_count--;
    return 1;
}

static xthread_result_t memop_refill_worker(void* arg)
{
    (void)arg;

    for (;;)
    {
        MemRefillRequest req;
        MemLane* lane;
        MemClass* cls;
        int stop;

        memop_lock();
        if (!memop_refill_queue_pop_locked(&req))
        {
            stop = GlobalMemPool.shutting_down;
            memop_unlock();
            if (stop) break;
            continue;
        }
        memop_unlock();

        lane = req.lane;
        if (!lane || req.class_id >= MEMOP_CLASS_COUNT) continue;
        if (!lane->active) continue;

        cls = &lane->classes[req.class_id];
        if (memop_refill_class(lane, req.class_id, cls->refill_amount))
        {
            memop_lock();
            GlobalMemPool.stats.async_refills++;
            memop_unlock();
        }

        memop_class_lock(cls);
        cls->refill_pending = 0;
        memop_class_unlock(cls);
    }

    return (xthread_result_t)0;
}

static void memop_start_refill_thread(void)
{
    int status = 0;
    if (GlobalMemPool.refill_thread_started) return;
    GlobalMemPool.refill_thread = thread_create(memop_refill_worker, NULL, &status);
    GlobalMemPool.refill_thread_started = status ? 1 : 0;
}

void memop_init(void)
{
    if (GlobalMemPool.initialized) return;

    thread_mutex_init(&GlobalMemPool.lock);
    thread_cond_init(&GlobalMemPool.refill_cv);

    GlobalMemPool.initialized = 1;
}

void memop_shutdown(void)
{
    MemLane* lanes;
    MemLane* retired;

    if (!GlobalMemPool.initialized) return;

    memop_lock();
    GlobalMemPool.shutting_down = 1;
    memop_refill_signal();
    memop_unlock();

    if (GlobalMemPool.refill_thread)
    {
        thread_join(&GlobalMemPool.refill_thread);
        GlobalMemPool.refill_thread_started = 0;
    }

    memop_lock();
    lanes = GlobalMemPool.lanes;
    retired = GlobalMemPool.retired_lanes;
    GlobalMemPool.lanes = NULL;
    GlobalMemPool.retired_lanes = NULL;
    memop_unlock();

    memop_destroy_lane_list(lanes);
    memop_destroy_lane_list(retired);

    thread_cond_destroy(&GlobalMemPool.refill_cv);
    thread_mutex_destroy(&GlobalMemPool.lock);

    memset(&GlobalMemPool, 0, sizeof(GlobalMemPool));
    g_mem_lane = NULL;
}

MemBuffer memop_alloc(uint64 size)
{
    MemBuffer out = { 0, 0 };
    MemLane* lane;
    MemClass* cls;
    MemFreeBlock* block;
    int class_id;

    if (size == 0) return out;

    class_id = memop_class_index(size);
    if (class_id < 0) return out;

    lane = memop_get_lane();
    if (!lane) return out;

    cls = &lane->classes[class_id];
    memop_class_lock(cls);

    if (!cls->free_head)
    {
        if (!memop_refill_class_unlocked(lane, (uint32)class_id, cls->refill_amount))
        {
            memop_class_unlock(cls);
            return out;
        }
        memop_lock();
        GlobalMemPool.stats.sync_refills++;
        memop_unlock();
    }

    block = cls->free_head;
    cls->free_head = block->next;
    cls->free_count--;
    cls->used_count++;

    if (cls->free_count <= cls->low_watermark && !cls->refill_pending)
    {
        cls->refill_pending = 1;
        memop_class_unlock(cls);
        memop_start_refill_thread();
        if (!memop_refill_queue_push(lane, (uint32)class_id))
        {
            memop_class_lock(cls);
            cls->refill_pending = 0;
            memop_class_unlock(cls);
        }
    }
    else
    {
        memop_class_unlock(cls);
    }

    out.Ptr = block;
    out.Size = cls->block_size;

    memop_lock();
    GlobalMemPool.stats.alloc_count++;
    memop_unlock();
    return out;
}

void memop_free(MemBuffer* buffer)
{
    MemBlockHeader* header;
    MemFreeBlock* block;
    MemLane* owner;
    MemClass* cls;

    if (!buffer || !buffer->Ptr) return;

    header = (MemBlockHeader*)((char*)buffer->Ptr - sizeof(MemBlockHeader));
    owner = header->owner;
    if (!owner || header->class_id >= MEMOP_CLASS_COUNT)
    {
        buffer->Ptr = NULL;
        buffer->Size = 0;
        return;
    }

    cls = &owner->classes[header->class_id];
    block = (MemFreeBlock*)buffer->Ptr;

    memop_class_lock(cls);
    block->next = cls->free_head;
    cls->free_head = block;
    if (cls->used_count > 0) cls->used_count--;
    cls->free_count++;
    memop_class_unlock(cls);

    if (owner != g_mem_lane)
    {
        memop_lock();
        GlobalMemPool.stats.remote_frees++;
        memop_unlock();
    }

    memop_lock();
    GlobalMemPool.stats.free_count++;
    memop_unlock();

    buffer->Ptr = NULL;
    buffer->Size = 0;
}

void memop_get_stats(MemPoolStats* out_stats)
{
    if (!out_stats) return;
    memop_init();
    memop_lock();
    *out_stats = GlobalMemPool.stats;
    memop_unlock();
}

void memop_test_reset(void)
{
    memop_shutdown();
    memop_init();
}

void memop_on_created_thread(const Thread* thr)
{
    (void)thr;
    g_mem_lane = memop_get_lane();
}

void memop_on_ended_thread(const Thread* thr)
{
    MemLane* lane = g_mem_lane;
    (void)thr;
    if (!lane) return;

    lane->active = 0;
    memop_lock();
    memop_remove_lane_locked(lane);
    lane->next = GlobalMemPool.retired_lanes;
    GlobalMemPool.retired_lanes = lane;
    GlobalMemPool.stats.lanes_destroyed++;
    memop_unlock();

    g_mem_lane = NULL;
}