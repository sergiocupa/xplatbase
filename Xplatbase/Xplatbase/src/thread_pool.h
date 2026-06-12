/*
 * thread_pool.h
 *
 *   WSPool  - work-stealing: ring MPMC por worker.
 *             Submit: round-robin entre workers.
 *             Idle:   worker rouba dos outros; spin progressivo antes de dormir.
 *             PoolStats.total_stolen   = tasks roubadas (work-stealing).
 *             PoolStats.total_handoffs = handoffs de task longa (substituicao de owner).
 */

#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <stdbool.h>
#include <stdint.h>

#include "atomics.h"
#include "ring_queue.h"
#include "thread_wait.h"
#include "thread_activity.h"
#include "../include/xplatbase.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ShardedPool ShardedPool;

typedef void (*task_fn)(void*);

typedef struct {
    task_fn  fn;
    void*    arg;
    uint64_t enqueue_tsc;   /* TSC do submit; usado p/ medir tempo de espera na fila */
} Task;

/* ─────────────────────────────────────────────────────────────────────────
 * Hook de log — a biblioteca nao imprime direto; entrega o evento ao app, que
 * decide onde/como mostrar (ex.: linha unica colorida). Cada pool captura o
 * hook vigente em pool_create; trocar o hook afeta apenas pools futuros.
 * ───────────────────────────────────────────────────────────────────────── */

#define POOL_LOG_INFO  0
#define POOL_LOG_WARN  1
#define POOL_LOG_CRIT  2

typedef void (*pool_log_fn)(int severity, const char* msg);

XPLATBASE_API void pool_set_log_hook(pool_log_fn fn);

/* ─────────────────────────────────────────────────────────────────────────
 * Tunables (override em tempo de compilacao)
 * ───────────────────────────────────────────────────────────────────────── */

#ifndef POOL_DEFAULT_RING_CAPACITY
#define POOL_DEFAULT_RING_CAPACITY    1024
#endif

#ifndef POOL_DEFAULT_SPIN_ITERATIONS
#define POOL_DEFAULT_SPIN_ITERATIONS  2048
#endif

#ifndef POOL_DEFAULT_SPIN_BUDGET_US
#define POOL_DEFAULT_SPIN_BUDGET_US   2000
#endif

#ifndef POOL_DEFAULT_RESERVE_SIZE
#define POOL_DEFAULT_RESERVE_SIZE     2
#endif

#ifndef POOL_DEFAULT_MAX_SHARDS
#define POOL_DEFAULT_MAX_SHARDS       64
#endif

#ifndef POOL_MONITOR_INTERVAL_MS
#define POOL_MONITOR_INTERVAL_MS      10
#endif

/* Threshold para handoff de task longa (default 20ms). */
#ifndef POOL_DEFAULT_LONG_TASK_NS
#define POOL_DEFAULT_LONG_TASK_NS     (20ULL * 1000000ULL)
#endif

/* Intervalo do monitor de tasks longas. */
#ifndef POOL_LONG_TASK_MONITOR_MS
#define POOL_LONG_TASK_MONITOR_MS     10
#endif

/* Fator de expansao da reserva quando esgotada. */
#ifndef POOL_RESERVE_EXPAND_NUM
#define POOL_RESERVE_EXPAND_NUM       3
#endif
#ifndef POOL_RESERVE_EXPAND_DEN
#define POOL_RESERVE_EXPAND_DEN       2
#endif

/* Resgate por backlog: dispara quando ring count ultrapassa este valor. */
#ifndef POOL_DEFAULT_RESCUE_BACKLOG
#define POOL_DEFAULT_RESCUE_BACKLOG   1
#endif

/* Parking: tempo ocioso (ms) apos o qual o worker para de spinar e dorme. */
#ifndef POOL_DEFAULT_PARK_IDLE_MS
#define POOL_DEFAULT_PARK_IDLE_MS     200
#endif

/* Teto absoluto de ajudantes de resgate por lane (0 → usa nº de cores). */
#ifndef POOL_DEFAULT_RESCUE_MAX_HELPERS
#define POOL_DEFAULT_RESCUE_MAX_HELPERS  0
#endif

/* Sono profundo (us) de um worker parqueado. */
#ifndef POOL_PARK_SLEEP_US
#define POOL_PARK_SLEEP_US            100000
#endif

/* Shutdown: tempo (ms) para drenar a fila graciosamente antes de forcar. */
#ifndef POOL_DEFAULT_SHUTDOWN_DRAIN_MS
#define POOL_DEFAULT_SHUTDOWN_DRAIN_MS  5000
#endif

/* Shutdown: tempo (ms) para os workers pararem pela flag antes de matar a thread. */
#ifndef POOL_DEFAULT_SHUTDOWN_JOIN_MS
#define POOL_DEFAULT_SHUTDOWN_JOIN_MS   2000
#endif

/* ─────────────────────────────────────────────────────────────────────────
 * ShardedPool — configuracao
 * ───────────────────────────────────────────────────────────────────────── */

typedef struct {
    int      shard_count;            /* 0 → auto (ncores)            */
    int      max_shards;
    int      ring_capacity;          /* por shard, potencia de 2     */
    int      spin_iterations;        /* fallback se spin_budget_us==0 */
    int      spin_budget_us;         /* µs de spin antes de dormir   */
    int      reserve_size;           /* 0 → mesma quantidade de workers */
    int      monitor_interval_ms;
    uint64_t long_task_threshold_ns; /* 0 → POOL_DEFAULT_LONG_TASK_NS */

    int      rescue_backlog_threshold; /* resgate quando ring count >= N (default 1) */
    int      rescue_max_helpers_per_lane; /* teto de ajudantes por lane (0 → nº cores) */
    int      max_auto_expand_lanes;    /* teto da expansao automatica (0 → nº cores logicos) */
    int      park_idle_threshold_ms;   /* ocioso > N ms → parquear (default 200)   */

    int      shutdown_drain_timeout_ms; /* drenar a fila ate N ms (0 → default 5000)        */
    int      shutdown_join_timeout_ms;  /* esperar workers pararem ate N ms (0 → default 2000) */
    bool     shutdown_force_kill;       /* opt-in perigoso; default false */

    xtask_thresholds_t task_thresholds;
    bool               task_thresholds_set;
} PoolConfig;

XPLATBASE_API PoolConfig pool_default_config(void);

/* ─────────────────────────────────────────────────────────────────────────
 * ShardedPool — ciclo de vida
 * ───────────────────────────────────────────────────────────────────────── */

XPLATBASE_API ShardedPool* pool_create  (const PoolConfig* cfg);  /* NULL → defaults */
XPLATBASE_API void         pool_init    (ShardedPool* pool);       /* aguarda workers prontos */
XPLATBASE_API void         pool_shutdown(ShardedPool* pool);       /* fecha e para; preserva o handle */
XPLATBASE_API void         pool_destroy (ShardedPool* pool);       /* chamar sem usuarios concorrentes */
XPLATBASE_API bool         pool_submit  (ShardedPool* pool, task_fn fn, void* arg);

/* ─────────────────────────────────────────────────────────────────────────
 * ShardedPool — estatisticas
 * ───────────────────────────────────────────────────────────────────────── */

typedef struct {
    int      shard_count;
    int      active_worker_count;
    int      reserve_count;
    int      detached_count;
    int      pending_tasks;
    uint64_t total_submitted;
    uint64_t submit_failures;
    uint64_t total_handoffs;         /* handoffs de task longa (substituicao de owner) */
    uint64_t total_stolen;           /* tasks roubadas por work-stealing */
    uint64_t total_rescued;
    uint64_t submit_backpressure;   /* nº de esperas no submit (backpressure) */
    uint64_t total_expansions;      /* lanes ativadas dinamicamente */
} PoolStats;

XPLATBASE_API void pool_stats(ShardedPool* pool, PoolStats* out);

/* Total de threads terminadas A FORCA (TerminateThread) no processo. > 0 indica
 * que algum shutdown teve task travada: pool envenenado, recursos vazados —
 * o chamador deve PARAR DE RECICLAR pools. */
XPLATBASE_API int pool_force_kill_count(void);

#ifdef __cplusplus
}
#endif

#endif /* THREAD_POOL_H */
