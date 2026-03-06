#include "task_handler.h"
#include "thread_wait.h"
#include "thread_handler.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>


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



typedef struct {
    void (*fn)(void*);
    void* arg;
} Task;

typedef struct 
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


typedef struct {
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
    xwait_t     monitor_wait;           /* sleep/wake do monitor */
    xatomic_int  monitor_sleeping;       /* 1 = dormindo */
    xatomic_int  expand_requested;       /* 1 = alguém pediu expansão */
    xatomic_int  expand_count;           /* total de shards criados pelo monitor */

#ifdef _WIN32
    HANDLE      monitor_thread;
#else
    pthread_t   monitor_thread;
#endif

} ShardedPool;





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

    xring_init(&s->ring, capacity);
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
    int count = xring_count(&s->ring);
    return (count > 0) ? (count * 100) / s->ring.capacity : 0;
}


static inline void pool_request_expand(ShardedPool* pool)
{
    int expected = 0;

    /* CAS garante que só um thread dispara o wake */
    if (atomic_compare_exchange_strong_explicit(&pool->expand_requested, &expected, 1, memory_order_acq_rel, memory_order_relaxed))
    {
        if (atomic_load_explicit(&pool->monitor_sleeping, memory_order_acquire)) thread_wait_wake(&pool->monitor_wait);
    }
}




//   -----------------------------------------------------------    Monitor de expansão    -----------------------------------------------------

static void* monitor_expand_fn(void* arg)
{
    ShardedPool* pool = (ShardedPool*)arg;

    thread_wait_prepare(&pool->monitor_wait);

    while (atomic_load_explicit(&pool->running, memory_order_acquire))
    {
        atomic_store_explicit(&pool->monitor_sleeping, 1, memory_order_release);

        if (!atomic_load_explicit(&pool->expand_requested, memory_order_acquire))
        {
            thread_wait_sleep(&pool->monitor_wait);//                    Dorme até ser acordado
        }

        atomic_store_explicit(&pool->monitor_sleeping, 0, memory_order_release);

        /* Saiu do sleep — verifica se é shutdown */
        if (!atomic_load_explicit(&pool->running, memory_order_acquire)) break;

        /* Cria shards novos (aqui é onde o calloc acontece) */
        int created = 0;
        for (int i = 0; i < EXPAND_BATCH; i++)
        {
            int current = atomic_load_explicit(&pool->shard_count, memory_order_acquire);

            if (current >= MAX_SHARD_COUNT)
                break;

            Shard* s = shard_create(SHARD_CAPACITY);
            if (!s) break;

            if (atomic_compare_exchange_strong_explicit(
                &pool->shard_count, &current, current + 1,
                memory_order_acq_rel, memory_order_acquire))
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

        if (created > 0)
            atomic_fetch_add(&pool->expand_count, created);

        /* Reseta flag — permite novo request */
        atomic_store_explicit(&pool->expand_requested, 0, memory_order_release);
    }

    return 0;
}




//   ---------------------------------------------------------------    Wake workers    ----------------------------------------------------------

static inline void pool_wake_one(ShardedPool* pool)
{
    for (int i = 0; i < pool->worker_count; i++)
    {
        if (atomic_load_explicit(&pool->workers[i].sleeping, memory_order_acquire))
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
        if (atomic_load_explicit(&pool->workers[i].sleeping, memory_order_acquire))
            thread_wait_wake(&pool->workers[i].wait);
    }
}




//   ------------------------------------------------------------------    Submit    -------------------------------------------------------------

inline boolean pool_submit(ShardedPool* pool, void (*fn)(void*), void* arg)
{
    int idx   = atomic_fetch_add_explicit(&pool->submit_idx, 1, memory_order_relaxed);
    int count = atomic_load_explicit(&pool->shard_count, memory_order_acquire);
    Task task = { fn, arg };

    for (int i = 0; i < count; i++)
    {
        Shard* s = pool->shards[(idx + i) % count];

        if (xring_push_mp(&s->ring, s->buffer, &task))
        {
            atomic_fetch_add_explicit(&pool->pending, 1, memory_order_relaxed);
            pool_wake_one(pool);

            if (shard_usage_pct(s) >= PRESSURE_THRESHOLD) pool_request_expand(pool);// Detecção proativa: pede expansão ANTES de saturar

            return true;
        }
    }

    atomic_fetch_add_explicit(&pool->submit_fail_count, 1, memory_order_relaxed);// Todos cheios — pede expansão reativamente
    pool_request_expand(pool);
    return false;
}


static inline bool pool_try_consume(ShardedPool* pool, int worker_id, Task* out)
{
    int count = atomic_load_explicit(&pool->shard_count, memory_order_acquire);

    for (int i = 0; i < count; i++)
    {
        Shard* s = pool->shards[(worker_id + i) % count];

        if (ring_queue_empty(&s->ring)) continue;

        if (xring_pop_mc(&s->ring, s->buffer, out))
        {
            atomic_fetch_sub_explicit(&pool->pending, 1, memory_order_relaxed);
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

    while (atomic_load_explicit(&pool->running, memory_order_acquire))
    {
        if (pool_try_consume(pool, worker_id, &task))
        {
            task.fn(task.arg);
            continue;
        }

        atomic_store_explicit(&ctx->sleeping, 1, memory_order_release);

        if (atomic_load_explicit(&pool->pending, memory_order_acquire) > 0)
        {
            atomic_store_explicit(&ctx->sleeping, 0, memory_order_release);
            continue;
        }

        thread_wait_sleep(&ctx->wait);
        atomic_store_explicit(&ctx->sleeping, 0, memory_order_release);
    }

    return 0;
}



//   ------------------------------------------------------------    Init e Shutdown     ---------------------------------------------------------

inline boolean pool_init(ShardedPool* pool, int worker_count)
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

    atomic_store(&pool->shard_count, INITIAL_SHARD_COUNT);
    atomic_store(&pool->submit_idx, 0);
    atomic_store(&pool->running, 1);
    atomic_store(&pool->pending, 0);
    atomic_store(&pool->submit_fail_count, 0);
    atomic_store(&pool->expand_requested, 0);
    atomic_store(&pool->expand_count, 0);
    atomic_store(&pool->monitor_sleeping, 0);

    pool->worker_count = worker_count;
    for (int i = 0; i < worker_count; i++) atomic_store(&pool->workers[i].sleeping, 0);


    if(!thread_create(&pool->monitor_thread, monitor_expand_fn, pool)) return false;


    /* Lança worker threads */
    for (int i = 0; i < worker_count; i++)
    {
        pool->worker_args[i][0] = pool;
        pool->worker_args[i][1] = (void*)i;

        if (!thread_create(&pool->threads[i], worker_fn, pool->worker_args[i]))
        {
            atomic_store(&pool->running, 0);
            pool_wake_all(pool);
            return false;
        }
    }

    return true;
}

inline void pool_shutdown(ShardedPool* pool)
{
    atomic_store_explicit(&pool->running, 0, memory_order_release);

    atomic_store(&pool->expand_requested, 1);

    if (atomic_load(&pool->monitor_sleeping)) thread_wait_wake(&pool->monitor_wait);

    pool_wake_all(pool);
    thread_join(&pool->monitor_thread);

    for (int i = 0; i < pool->worker_count; i++)
    {
        thread_join(&pool->threads[i]);
    }

    int count = atomic_load(&pool->shard_count);
    for (int i = 0; i < count; i++)
    {
        shard_destroy(pool->shards[i]);
    }
}
