/*
 * thread_pool.h
 *
 * Sharded thread pool with:
 *   - Per-shard MPSC ring (xring) for tasks
 *   - One active worker per shard at a time (workers are swappable)
 *   - Worker hot-path: drain ring -> spin -> wait, in that order
 *   - Reserve pool of pre-created workers, replenished by monitor
 *   - Detached list for workers stuck on long blocking tasks
 *   - Monitor thread: handoff decisions, reserve replenish, joins,
 *     and (stub) shard expand
 *
 * Long-task detection uses xthread_activity (CPU activity ratio).
 * When a worker is on a long+blocked task, it's swapped out for a
 * fresh reserve worker so the shard keeps consuming.
 */

#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <stdbool.h>
#include <stdint.h>

#include "atomics.h"
#include "ring_queue.h"
#include "thread_wait.h"
#include "thread_activity.h"

 /* Forward declarations. */
typedef struct Shard         Shard;
typedef struct Worker        Worker;
typedef struct ShardedPool   ShardedPool;

typedef void (*task_fn)(void*);

typedef struct {
    task_fn fn;
    void* arg;
} Task;

/* -------------------------------------------------------------------------
 * Tunables. Override at compile time if needed.
 * ------------------------------------------------------------------------- */
#ifndef POOL_DEFAULT_RING_CAPACITY
#define POOL_DEFAULT_RING_CAPACITY  1024     /* must be power of 2 */
#endif

#ifndef POOL_DEFAULT_SPIN_ITERATIONS
#define POOL_DEFAULT_SPIN_ITERATIONS  2048   /* before parking */
#endif

#ifndef POOL_DEFAULT_RESERVE_SIZE
#define POOL_DEFAULT_RESERVE_SIZE  2         /* spare workers ready to assume */
#endif

#ifndef POOL_DEFAULT_MAX_SHARDS
#define POOL_DEFAULT_MAX_SHARDS  64
#endif

#ifndef POOL_MONITOR_INTERVAL_MS
#define POOL_MONITOR_INTERVAL_MS  10
#endif

#ifndef POOL_PRESSURE_THRESHOLD_PCT
#define POOL_PRESSURE_THRESHOLD_PCT  70
#endif

 /* -------------------------------------------------------------------------
  * Pool configuration.
  * ------------------------------------------------------------------------- */
typedef struct {
    int      shard_count;          /* initial; 0 -> auto-detect from CPU count */
    int      max_shards;
    int      ring_capacity;        /* per shard, power of 2 */
    int      spin_iterations;
    int      reserve_size;
    int      monitor_interval_ms;

    /* Long-task / handoff thresholds. NULL fields -> defaults. */
    xtask_thresholds_t  task_thresholds;
    bool                task_thresholds_set;
} PoolConfig;

/* Returns a config struct populated with defaults. */
XPLATBASE_API PoolConfig pool_default_config(void);

/* -------------------------------------------------------------------------
 * Lifecycle.
 * ------------------------------------------------------------------------- */
XPLATBASE_API ShardedPool* pool_create(const PoolConfig* cfg);   /* NULL cfg -> defaults */
XPLATBASE_API void         pool_shutdown(ShardedPool* pool);     /* drains, joins, frees */

/* -------------------------------------------------------------------------
 * Submit a task. Returns false if all shards are full (extremely rare in
 * normal operation since the pool will request expand under pressure).
 * ------------------------------------------------------------------------- */
XPLATBASE_API bool pool_submit(ShardedPool* pool, task_fn fn, void* arg);

/* -------------------------------------------------------------------------
 * Observability.
 * ------------------------------------------------------------------------- */
typedef struct {
    int      shard_count;
    int      active_worker_count;
    int      reserve_count;
    int      detached_count;
    int      pending_tasks;
    uint64_t total_submitted;
    uint64_t submit_failures;
    uint64_t total_handoffs;
} PoolStats;

XPLATBASE_API void pool_stats(ShardedPool* pool, PoolStats* out);

#endif /* THREAD_POOL_H */