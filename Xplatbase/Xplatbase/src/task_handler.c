#include "task_handler.h"
#include "thread_handler.h"
#include "ring_queue.h"
#include "atomics.h"
//#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
//#include <string.h>



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



//   -----------------------------------------------------------    Monitor de expansão    -----------------------------------------------------

static void* monitor_expand_fn(void* arg)
{
    ShardedPool* pool = (ShardedPool*)arg;

    thread_wait_prepare(&pool->monitor_wait);

    while (atomic_get(&pool->running))
    {
        atomic_set(&pool->monitor_sleeping, 1);

        if (!atomic_get(&pool->expand_requested))
        {
            thread_wait_sleep(&pool->monitor_wait);// Dorme até ser acordado
        }

        atomic_set(&pool->monitor_sleeping, 0);

        if (!atomic_get(&pool->running)) break;// Saiu do sleep — verifica se é shutdown

        /* Cria shards novos (aqui é onde o calloc acontece) */
        int created = 0;
        for (int i = 0; i < EXPAND_BATCH; i++)
        {
            int current = atomic_get(&pool->shard_count);

            if (current >= MAX_SHARD_COUNT)
                break;

            Shard* s = shard_create(SHARD_CAPACITY);
            if (!s) break;

            if (atomic_cas(&pool->shard_count, &current, current + 1))
            {
                pool->shards[current] = s;
                created++;
            }
            else
            {
                shard_destroy(s);
                i--;
            }
        }

        if (created > 0) atomic_add(&pool->expand_count, created);

        atomic_set(&pool->expand_requested, 0);
    }

    return 0;
}




//   ---------------------------------------------------------------    Wake workers    ----------------------------------------------------------

static inline void pool_wake_one(ShardedPool* pool)
{
    for (int i = 0; i < pool->worker_count; i++)
    {
        if (atomic_get(&pool->workers[i].sleeping))
        {
            thread_wait_wake(&pool->workers[i].wait);
            return;
        }
    }
}


static inline void pool_wake_all(ShardedPool* pool)
{
    for (int i = 0; i < pool->worker_count; i++)
    {
        if (atomic_get(&pool->workers[i].sleeping)) thread_wait_wake(&pool->workers[i].wait);
    }
}




//   ------------------------------------------------------------------    Submit    -------------------------------------------------------------

boolean pool_submit(ShardedPool* pool, void (*fn)(void*), void* arg)
{
    int idx   = atomic_add(&pool->submit_idx,1);
    int count = atomic_get(&pool->shard_count);
    Task task = { fn, arg };

    for (int i = 0; i < count; i++)
    {
        Shard* s = pool->shards[(idx + i) % count];

        if (xring_push_mp(&s->ring, s->buffer, &task))
        {
            atomic_add(&pool->pending,1);

            pool_wake_one(pool);

            if (shard_usage_pct(s) >= PRESSURE_THRESHOLD) pool_request_expand(pool);// Detecção proativa: pede expansão ANTES de saturar

            return true;
        }
    }

    atomic_add(&pool->submit_fail_count, 1);// Todos cheios — pede expansão reativamente
    pool_request_expand(pool);
    return false;
}


static inline boolean pool_try_consume(ShardedPool* pool, int worker_id, Task* out)
{
    int count = atomic_get(&pool->shard_count);

    for (int i = 0; i < count; i++)
    {
        Shard* s = pool->shards[(worker_id + i) % count];

        if (ring_queue_empty(&s->ring)) continue;

        if (xring_pop_mc(&s->ring, s->buffer, out))
        {
            atomic_sub(&pool->pending, 1);
            return true;
        }
    }
    return false;
}


static void* worker_fn(void* arg)
{
    ShardedPool* pool = (ShardedPool*)((void**)arg)[0];
    int          worker_id = (int)(((void**)arg)[1]);
    WorkerCtx* ctx = &pool->workers[worker_id];
    Task         task;

    thread_wait_prepare(&ctx->wait);

    while (atomic_get(&pool->running))
    {
        if (pool_try_consume(pool, worker_id, &task))
        {
            task.fn(task.arg);
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



//   ------------------------------------------------------------    Init e Shutdown     ---------------------------------------------------------

boolean pool_init(ShardedPool* pool, int worker_count)
{
    memset(pool, 0, sizeof(ShardedPool));
    thread_wait_init();

    if (worker_count > MAX_WORKER_COUNT) worker_count = MAX_WORKER_COUNT;

    /* Cria shards iniciais */
    for (int i = 0; i < INITIAL_SHARD_COUNT; i++)
    {
        pool->shards[i] = shard_create(SHARD_CAPACITY);
        if (!pool->shards[i])
        {
            for (int j = 0; j < i; j++) shard_destroy(pool->shards[j]);
            return false;
        }
    }

    atomic_set(&pool->shard_count, INITIAL_SHARD_COUNT);
    atomic_set(&pool->submit_idx, 0);
    atomic_set(&pool->running, 1);
    atomic_set(&pool->pending, 0);
    atomic_set(&pool->submit_fail_count, 0);
    atomic_set(&pool->expand_requested, 0);
    atomic_set(&pool->expand_count, 0);
    atomic_set(&pool->monitor_sleeping, 0);

    pool->worker_count = worker_count;
    for (int i = 0; i < worker_count; i++) atomic_set(&pool->workers[i].sleeping, 0);


    if (!thread_create(&pool->monitor_thread, monitor_expand_fn, pool))
    {
        return false;
    }


    /* Lança worker threads */
    for (int i = 0; i < worker_count; i++)
    {
        pool->worker_args[i][0] = pool;
        pool->worker_args[i][1] = (void*)i;

        if (!thread_create(&pool->threads[i], worker_fn, pool->worker_args[i]))
        {
            atomic_set(&pool->running, 0);
            pool_wake_all(pool);
            return false;
        }
    }

    return true;
}

void pool_shutdown(ShardedPool* pool)
{
    atomic_set(&pool->running, 0);
    atomic_set(&pool->expand_requested, 1);

    if (atomic_get(&pool->monitor_sleeping)) thread_wait_wake(&pool->monitor_wait);

    pool_wake_all(pool);
    thread_join(&pool->monitor_thread);

    for (int i = 0; i < pool->worker_count; i++)
    {
        thread_join(&pool->threads[i]);
    }

    int count = atomic_get(&pool->shard_count);
    for (int i = 0; i < count; i++)
    {
        shard_destroy(pool->shards[i]);
    }
}
