#include "task_handler.h"
#include "thread_handler.h"
#include "ring_queue.h"
#include "atomics.h"
//#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>


typedef struct { ShardedPool* pool; int worker_id; } WorkerArg;


static inline Shard* shard_create(int capacity)
{
    Shard* s = (Shard*)calloc(1, sizeof(Shard));
    if (!s) return NULL;

    s->buffer = (Task*)calloc(capacity, sizeof(Task));

    if (!s->buffer)
    {
        free(s);
        return NULL;
    }

    ring_queue_init(&s->ring, capacity);
    return s;
}

static inline void shard_destroy(Shard* s)
{
    if (!s) return;
    ring_queue_destroy(&s->ring);
    free(s->buffer);
    free(s);
}



//   -----------------------------------------------------------    Checagem de pressão (inline, barato)    -----------------------------------------------------

static inline int shard_usage_pct(Shard* s)
{
    int count = ring_queue_count(&s->ring);
    return (count > 0) ? (count * 100) / s->ring.capacity : 0;
}


static inline void pool_request_expand(ShardedPool* pool)
{
    int expected = 0;

    if (atomic_cas(&pool->expand_requested, &expected, 1))
    {
        if (atomic_get(&pool->monitor_sleeping))
        {
            thread_wait_wake(&pool->monitor_wait);
        }
    }
}



//   ---------------------------------------------------------------    Wake workers    ----------------------------------------------------------

static inline void pool_wake_one(ShardedPool* pool)
{
    for (int i = 0; i < pool->worker_count; i++)
    {
        if (atomic_get(&pool->workers[i]->sleeping))
        {
            thread_wait_wake(&pool->workers[i]->wait);
            return;
        }
    }
}


static inline void pool_wake_all(ShardedPool* pool)
{
    for (int i = 0; i < pool->worker_count; i++)
    {
        if (atomic_get(&pool->workers[i]->sleeping)) thread_wait_wake(&pool->workers[i]->wait);
    }
}


//   -----------------------------------------------------------    Shard / Worker helpers    -------------------------------------------------------

static boolean pool_add_shard(ShardedPool* pool)
{
    int current = atomic_get(&pool->shard_count);
    if (current >= pool->shard_capacity) return false;
    Shard* s = shard_create(SHARD_CAPACITY);
    if (!s) return false;
    pool->shards[current] = s;
    if (atomic_cas(&pool->shard_count, &current, current + 1))
        return true;
    pool->shards[current] = NULL;
    shard_destroy(s);
    return false;
}

static void* worker_fn(void* arg);                  /* forward decl */

static boolean pool_add_worker(ShardedPool* pool)
{
    int wid = pool->worker_count;
    if (wid >= pool->worker_capacity) return false;

    WorkerCtx* ctx = (WorkerCtx*)calloc(1, sizeof(WorkerCtx));
    if (!ctx) return false;
    atomic_initialize(&ctx->sleeping, 0);
    pool->workers[wid] = ctx;

    WorkerArg* wa = (WorkerArg*)malloc(sizeof(WorkerArg));
    if (!wa) { free(ctx); pool->workers[wid] = NULL; return false; }
    wa->pool      = pool;
    wa->worker_id = wid;
    pool->worker_arg_ptrs[wid] = wa;

    if (!thread_create(&pool->threads[wid], worker_fn, wa))
    {
        free(wa); pool->worker_arg_ptrs[wid] = NULL;
        free(ctx); pool->workers[wid] = NULL;
        return false;
    }
    pool->worker_count++;
    return true;
}


//   ------------------------------------------------------------------    Submit    -------------------------------------------------------------

boolean pool_submit(ShardedPool* pool, void (*fn)(void*), void* arg)
{
    int idx   = atomic_add(&pool->submit_idx, 1);
    int count = atomic_get(&pool->shard_count);
    Task task = { fn, arg };

    for (int i = 0; i < count; i++)
    {
        Shard* s = pool->shards[(idx + i) % count];

        if (s && xring_push_mp(&s->ring, s->buffer, &task))
        {
            atomic_add(&pool->pending, 1);

            pool_wake_one(pool);

            if (shard_usage_pct(s) >= PRESSURE_THRESHOLD) pool_request_expand(pool);

            return true;
        }
    }

    atomic_add(&pool->submit_fail_count, 1);
    pool_request_expand(pool);
    return false;
}


static inline boolean pool_try_consume(ShardedPool* pool, int worker_id, Task* out)
{
    int count = atomic_get(&pool->shard_count);
    if (count == 0) return false;

    int primary = worker_id % count;
    Shard* s = pool->shards[primary];
    if (s && !ring_queue_empty(&s->ring) && xring_pop_mc(&s->ring, s->buffer, out))
    {
        atomic_sub(&pool->pending, 1);
        return true;
    }

    for (int i = 1; i < count; i++)
    {
        s = pool->shards[(primary + i) % count];
        if (s && !ring_queue_empty(&s->ring) && xring_pop_mc(&s->ring, s->buffer, out))
        {
            atomic_sub(&pool->pending, 1);
            return true;
        }
    }
    return false;
}


static void* worker_fn(void* arg)
{
    WorkerArg*   wa        = (WorkerArg*)arg;
    ShardedPool* pool      = wa->pool;
    int          worker_id = wa->worker_id;
    WorkerCtx*   ctx       = pool->workers[worker_id];
    Task         task;

    SetThreadPriority(pool->threads[worker_id], THREAD_PRIORITY_TIME_CRITICAL);
    thread_wait_prepare(&ctx->wait);

    while (atomic_get(&pool->running))
    {
        if (pool_try_consume(pool, worker_id, &task))
        {
            atomic_add(&pool->active_workers, 1);
            task.fn(task.arg);
            atomic_sub(&pool->active_workers, 1);
            continue;
        }

        atomic_set(&ctx->sleeping, 1);

        if (atomic_get(&pool->pending) > 0)
        {
            atomic_set(&ctx->sleeping, 0);
            continue;
        }

        thread_wait_sleep(&ctx->wait);
        atomic_set(&ctx->sleeping, 0);
    }
    return 0;
}


//   -----------------------------------------------------------    Monitor de expansão    -----------------------------------------------------

static void* monitor_expand_fn(void* arg)
{
    ShardedPool* pool = (ShardedPool*)arg;
    thread_wait_prepare(&pool->monitor_wait);
    int cooldown_ticks = 0;

    while (atomic_get(&pool->running))
    {
        atomic_set(&pool->monitor_sleeping, 1);
        thread_wait_sleep_for(&pool->monitor_wait, MONITOR_TICK_US);
        atomic_set(&pool->monitor_sleeping, 0);

        if (!atomic_get(&pool->running)) break;

        /* --- cooldown pos-expansao ------------------------------------ */
        if (cooldown_ticks > 0)
        {
            cooldown_ticks--;
            atomic_set(&pool->expand_requested, 0);
            continue;
        }

        int total_shards  = atomic_get(&pool->shard_count);
        int total_workers = pool->worker_count;

        int occupied_shards = 0;
        int idle_shards     = 0;
        for (int i = 0; i < total_shards; i++)
        {
            if (pool->shards[i] && ring_queue_count(&pool->shards[i]->ring) > 0)
                occupied_shards++;
            else
                idle_shards++;
        }

        int shard_util_pct = total_shards > 0 ? (occupied_shards * 100) / total_shards : 0;
        int expanded       = 0;

        /* --- expansao de shards --------------------------------------- */
        /* Gatilho: pressao >= 50% E sem shards ociosos disponíveis      */
        if ((shard_util_pct >= PROACTIVE_SHARD_THRESHOLD || atomic_get(&pool->expand_requested))
            && idle_shards == 0)
        {
            int created = 0;
            for (int i = 0; i < EXPAND_BATCH; i++)
                if (pool_add_shard(pool)) created++;
            if (created > 0)
            {
                atomic_add(&pool->expand_count, created);
                expanded = 1;
            }
        }
        atomic_set(&pool->expand_requested, 0);

        /* --- expansao de workers -------------------------------------- */
        /* Gatilho separado: manter ao menos 1 worker por shard          */
        int current_shards  = atomic_get(&pool->shard_count);
        int current_workers = pool->worker_count;
        if (current_workers < current_shards)
        {
            int deficit = current_shards - current_workers;
            if (deficit > EXPAND_BATCH) deficit = EXPAND_BATCH;
            for (int i = 0; i < deficit; i++)
                pool_add_worker(pool);
            expanded = 1;
        }

        if (expanded) cooldown_ticks = EXPAND_COOLDOWN_TICKS;
    }
    return 0;
}


//   ------------------------------------------------------------    Init e Shutdown     ---------------------------------------------------------

boolean pool_init(ShardedPool* pool)
{
    memset(pool, 0, sizeof(ShardedPool));
    thread_wait_init(false);

    pool->shards = (Shard**)calloc(POOL_MAX_SHARDS, sizeof(Shard*));
    if (!pool->shards) return false;
    pool->shard_capacity = POOL_MAX_SHARDS;

#ifdef _WIN32
    pool->threads = (HANDLE*)calloc(POOL_MAX_WORKERS, sizeof(HANDLE));
#else
    pool->threads = (pthread_t*)calloc(POOL_MAX_WORKERS, sizeof(pthread_t));
#endif
    if (!pool->threads) { free(pool->shards); return false; }

    pool->workers = (WorkerCtx**)calloc(POOL_MAX_WORKERS, sizeof(WorkerCtx*));
    if (!pool->workers) { free(pool->threads); free(pool->shards); return false; }
    pool->worker_capacity = POOL_MAX_WORKERS;

    pool->worker_arg_ptrs = (void**)calloc(POOL_MAX_WORKERS, sizeof(void*));
    if (!pool->worker_arg_ptrs) { free(pool->workers); free(pool->threads); free(pool->shards); return false; }

    for (int i = 0; i < INITIAL_SHARD_COUNT; i++)
    {
        pool->shards[i] = shard_create(SHARD_CAPACITY);
        if (!pool->shards[i])
        {
            for (int j = 0; j < i; j++) shard_destroy(pool->shards[j]);
            free(pool->worker_arg_ptrs); free(pool->workers);
            free(pool->threads); free(pool->shards);
            return false;
        }
    }

    atomic_set(&pool->shard_count,        INITIAL_SHARD_COUNT);
    atomic_set(&pool->submit_idx,         0);
    atomic_set(&pool->running,            1);
    atomic_set(&pool->pending,            0);
    atomic_set(&pool->submit_fail_count,  0);
    atomic_set(&pool->expand_requested,   0);
    atomic_set(&pool->expand_count,       0);
    atomic_set(&pool->monitor_sleeping,   0);
    atomic_set(&pool->active_workers,     0);

    if (!thread_create(&pool->monitor_thread, monitor_expand_fn, pool))
    {
        pool_shutdown(pool);
        return false;
    }

    for (int i = 0; i < INITIAL_SHARD_COUNT; i++)
    {
        if (!pool_add_worker(pool))
        {
            pool_shutdown(pool);
            return false;
        }
    }

    return true;
}

void pool_shutdown(ShardedPool* pool)
{
    atomic_set(&pool->running, 0);
    atomic_set(&pool->expand_requested, 1);

    if (pool->monitor_thread && atomic_get(&pool->monitor_sleeping))
        thread_wait_wake(&pool->monitor_wait);

    pool_wake_all(pool);

    if (pool->monitor_thread)
        thread_join(&pool->monitor_thread);

    for (int i = 0; i < pool->worker_count; i++)
        thread_join(&pool->threads[i]);

    for (int i = 0; i < pool->worker_count; i++)
    {
        free(pool->workers[i]);
        free(pool->worker_arg_ptrs[i]);
    }

    int count = atomic_get(&pool->shard_count);
    for (int i = 0; i < count; i++)
        shard_destroy(pool->shards[i]);

    free(pool->shards);
    free(pool->workers);
    free(pool->threads);
    free(pool->worker_arg_ptrs);
}
