/*
 * thread_pool.c — WSPool (work-stealing)
 *
 * Um ring MPMC por worker. Submit: round-robin entre workers.
 * Idle: worker rouba tasks dos rings dos outros antes de dormir.
 *
 * Spin progressivo:
 *   P1: xcpu_pause × N  (ou RDTSC deadline)  — 0 syscall, 100% CPU
 *   P2: SwitchToThread × 128                 — cede HT/core, CPU cai
 *   P3: Sleep(0) × 4                         — cede OS, CPU cai muito
 *   P4: WaitOnAddress 1ms                     — 0% CPU, wake imediato via WakeByAddressSingle
 */

#include "thread_pool.h"

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>

#ifdef XPLATBASE_WIN
    #include <intrin.h>
    typedef HANDLE           xpl_thread_t;
    typedef DWORD WINAPI     xpl_fn_sig(void*);
    static xpl_thread_t xpl_thread_start(xpl_fn_sig* fn, void* arg) {
        return CreateThread(NULL, 0, fn, arg, 0, NULL);
    }
    static void xpl_thread_join(xpl_thread_t h) {
        WaitForSingleObject(h, INFINITE); CloseHandle(h);
    }
    static void xpl_yield(void)  { SwitchToThread(); }
    static void xpl_sleep0(void) { Sleep(0); }
    static uint64_t xpl_tsc(void){ return (uint64_t)__rdtsc(); }
    #define XPL_FN   DWORD WINAPI
    #define XPL_RET  return 0
#else
    #include <pthread.h>
    #include <sched.h>
    #include <time.h>
    typedef pthread_t        xpl_thread_t;
    typedef void*            xpl_fn_sig(void*);
    static xpl_thread_t xpl_thread_start(xpl_fn_sig* fn, void* arg) {
        pthread_t t; pthread_create(&t, NULL, fn, arg); return t;
    }
    static void xpl_thread_join(xpl_thread_t h) { pthread_join(h, NULL); }
    static void xpl_yield(void)  { sched_yield(); }
    static void xpl_sleep0(void) { struct timespec z={0,0}; nanosleep(&z,NULL); }
    static uint64_t xpl_tsc(void) {
        #if defined(__x86_64__)||defined(__i386__)
            return (uint64_t)__rdtsc();
        #else
            struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
            return (uint64_t)ts.tv_sec*1000000000ULL+(uint64_t)ts.tv_nsec;
        #endif
    }
    #define XPL_FN   static void*
    #define XPL_RET  return NULL
#endif

/* ─────────────────────────────────────────────────────────────────────────
 * Contagens de spin por fase
 * ───────────────────────────────────────────────────────────────────────── */

#define SPIN_P1_ITER  512
#define SPIN_P2_ITER  128
#define SPIN_P3_ITER    4

/* ─────────────────────────────────────────────────────────────────────────
 * Estados de worker
 * ───────────────────────────────────────────────────────────────────────── */

#define WSTATE_ACTIVE    0
#define WSTATE_STOPPING  3
#define WSTATE_STOPPED   4

/* ─────────────────────────────────────────────────────────────────────────
 * Estruturas internas
 * ───────────────────────────────────────────────────────────────────────── */

typedef struct WSWorker {
    RingQueue    ring;
    void*        ring_buf;
    xwait_t      wait;
    ShardedPool* pool;
    xpl_thread_t handle;
    xatomic_int  state;
    int          idx;
} WSWorker;

struct ShardedPool {
    WSWorker*    workers;
    int          worker_count;

    xatomic_int  shutdown;
    xatomic_int  submit_seq;
    xatomic_int  workers_ready;
    int          expected_ready;

    xatomic_int  stat_submitted;
    xatomic_int  stat_failures;
    xatomic_int  stat_stolen;

    uint64_t     spin_budget_cycles;
    int          spin_iterations;
};

/* ─────────────────────────────────────────────────────────────────────────
 * Tenta pop no proprio ring
 * ───────────────────────────────────────────────────────────────────────── */

static bool worker_try_own(WSWorker* w, Task* out)
{
    return xring_pop_mc(&w->ring, w->ring_buf, out);
}

/* ─────────────────────────────────────────────────────────────────────────
 * Tenta roubar do ring de outro worker (round-robin a partir de idx+1)
 * ───────────────────────────────────────────────────────────────────────── */

static bool worker_try_steal(WSWorker* w, Task* out)
{
    ShardedPool* pool  = w->pool;
    int          n     = pool->worker_count;
    int          start = (w->idx + 1) % n;

    for (int i = 0; i < n - 1; i++)
    {
        WSWorker* victim = &pool->workers[(start + i) % n];
        if (xring_pop_mc(&victim->ring, victim->ring_buf, out)) 
        {
            atomic_add(&pool->stat_stolen, 1);
            return true;
        }
    }
    return false;
}

static bool worker_try_any(WSWorker* w, Task* out)
{
    return worker_try_own(w, out) || worker_try_steal(w, out);
}

/* ─────────────────────────────────────────────────────────────────────────
 * Fases de spin progressivo
 * ───────────────────────────────────────────────────────────────────────── */

static bool spin_check_state(WSWorker* w)
{
    return atomic_get(&w->state) == WSTATE_ACTIVE && !atomic_get(&w->pool->shutdown);
}

static bool spin_phase1(WSWorker* w, Task* out, uint64_t budget_cycles)
{
    if (budget_cycles > 0) 
    {
        uint64_t deadline = xpl_tsc() + budget_cycles;
        int checks = 0;
        for (;;) 
        {
            if (worker_try_any(w, out)) return true;
            if (!spin_check_state(w))  return false;

            xcpu_pause();

            if (++checks >= 64) 
            {
                checks = 0;
                if (xpl_tsc() >= deadline) return false;
            }
        }
    }

    for (int i = 0; i < SPIN_P1_ITER; i++) 
    {
        if (worker_try_any(w, out)) return true;
        if (!spin_check_state(w))  return false;
        xcpu_pause();
    }
    return false;
}

static bool spin_phase2(WSWorker* w, Task* out)
{
    for (int i = 0; i < SPIN_P2_ITER; i++) 
    {
        if (worker_try_any(w, out)) return true;
        if (!spin_check_state(w))  return false;
        xpl_yield();
    }
    return false;
}

static bool spin_phase3(WSWorker* w, Task* out)
{
    for (int i = 0; i < SPIN_P3_ITER; i++) 
    {
        if (worker_try_any(w, out)) return true;
        if (!spin_check_state(w))  return false;
        xpl_sleep0();
    }
    return false;
}

/* ─────────────────────────────────────────────────────────────────────────
 * worker_idle: P1 → P2 → P3 → WaitOnAddress 1ms (acordavel por pool_submit)
 * Retorna true + *out preenchida se encontrou task.
 * ───────────────────────────────────────────────────────────────────────── */

static bool worker_idle(WSWorker* w, Task* out)
{
    /* Reseta signal=0 antes de checar tasks: garante que um wake concorrente
     * nao se perde — WaitOnAddress so dorme se signal==0 no momento da chamada. */
    thread_wait_prepare(&w->wait);

    if (worker_try_any(w, out)) return true;

    if (spin_phase1(w, out, w->pool->spin_budget_cycles)) return true;
    if (!spin_check_state(w)) return false;

    if (spin_phase2(w, out)) return true;
    if (!spin_check_state(w)) return false;

    if (spin_phase3(w, out)) return true;
    if (!spin_check_state(w)) return false;

    /* Dorme ate 1ms ou ate pool_submit/pool_shutdown chamarem thread_wait_wake.
     * WaitOnAddress acorda imediatamente via WakeByAddressSingle, sem depender
     * da granularidade do timer do Windows (sem timeBeginPeriod). */
    thread_wait_sleep_for(&w->wait, 1000);
    return worker_try_any(w, out);
}

/* ─────────────────────────────────────────────────────────────────────────
 * Funcao de thread do worker
 * ───────────────────────────────────────────────────────────────────────── */

XPL_FN worker_fn(void* raw)
{
    WSWorker*    w    = (WSWorker*)raw;
    ShardedPool* pool = w->pool;

    atomic_add(&pool->workers_ready, 1);
    fprintf(stderr, "[worker %d] started\n", w->idx);

    Task t;
    memset(&t, 0, sizeof(t));

    while (!atomic_get(&pool->shutdown) &&
           atomic_get(&w->state) != WSTATE_STOPPING)
    {
        t.fn = NULL;
        if (worker_try_any(w, &t)) {
            t.fn(t.arg);
        } else {
            if (worker_idle(w, &t) && t.fn) {
                t.fn(t.arg);
            }
        }
    }

    atomic_set(&w->state, WSTATE_STOPPED);
    XPL_RET;
}

/* ─────────────────────────────────────────────────────────────────────────
 * API publica: pool_submit
 * ───────────────────────────────────────────────────────────────────────── */

bool pool_submit(ShardedPool* pool, task_fn fn, void* arg)
{
    int       widx = (int)((unsigned int)atomic_add(&pool->submit_seq, 1) % (unsigned int)pool->worker_count);
    WSWorker* w = &pool->workers[widx];

    Task t = { fn, arg };
    if (!xring_push_mp(&w->ring, w->ring_buf, &t))
    {
        atomic_add(&pool->stat_failures, 1);
        return false;
    }

    atomic_add(&pool->stat_submitted, 1);
    thread_wait_wake(&w->wait);
    return true;
}



/* ─────────────────────────────────────────────────────────────────────────
 * API publica: pool_default_config
 * ───────────────────────────────────────────────────────────────────────── */

PoolConfig pool_default_config(void)
{
    PoolConfig c;
    memset(&c, 0, sizeof(c));
    c.shard_count         = xcpu_count();
    c.max_shards          = POOL_DEFAULT_MAX_SHARDS;
    c.ring_capacity       = POOL_DEFAULT_RING_CAPACITY;
    c.spin_iterations     = POOL_DEFAULT_SPIN_ITERATIONS;
    c.spin_budget_us      = POOL_DEFAULT_SPIN_BUDGET_US;
    c.reserve_size        = POOL_DEFAULT_RESERVE_SIZE;
    c.monitor_interval_ms = POOL_MONITOR_INTERVAL_MS;
    c.task_thresholds.long_threshold_ns = (uint64_t)XTASK_DEFAULT_LONG_NS;
    c.task_thresholds.blocked_ratio_max = XTASK_DEFAULT_BLOCKED_RATIO;
    c.task_thresholds.cpu_ratio_min     = XTASK_DEFAULT_CPU_RATIO;
    c.task_thresholds_set = false;
    return c;
}

/* ─────────────────────────────────────────────────────────────────────────
 * API publica: pool_create
 * ───────────────────────────────────────────────────────────────────────── */

ShardedPool* pool_create(const PoolConfig* cfg)
{
    PoolConfig c = cfg ? *cfg : pool_default_config();
    if (c.shard_count   <= 0) c.shard_count  = xcpu_count();
    if (c.ring_capacity <= 0) c.ring_capacity = POOL_DEFAULT_RING_CAPACITY;

    thread_wait_init(false);

    ShardedPool* pool = (ShardedPool*)calloc(1, sizeof(ShardedPool));
    if (!pool) return NULL;

    pool->worker_count    = c.shard_count;
    pool->spin_iterations = c.spin_iterations;

    if (c.spin_budget_us > 0) {
        double cpns = xthread_cycles_per_ns();
        if (cpns <= 0.0) cpns = 2.4;
        pool->spin_budget_cycles = (uint64_t)((double)c.spin_budget_us * 1000.0 * cpns);
    }

    pool->workers = (WSWorker*)calloc(pool->worker_count, sizeof(WSWorker));
    if (!pool->workers) { free(pool); return NULL; }

    size_t ring_buf_size = (size_t)c.ring_capacity * sizeof(Task);
    for (int i = 0; i < pool->worker_count; i++) {
        WSWorker* w = &pool->workers[i];
        w->idx      = i;
        w->pool     = pool;
        atomic_set(&w->state, WSTATE_ACTIVE);
        w->ring_buf = malloc(ring_buf_size);
        if (!w->ring_buf) { pool_shutdown(pool); return NULL; }
        ring_queue_init(&w->ring, c.ring_capacity);
        thread_wait_prepare(&w->wait);
    }

    pool->expected_ready = pool->worker_count;
    for (int i = 0; i < pool->worker_count; i++) {
        pool->workers[i].handle = xpl_thread_start(worker_fn, &pool->workers[i]);
    }

    pool_init(pool);

    return pool;
}

/* ─────────────────────────────────────────────────────────────────────────
 * API publica: pool_init — aguarda workers girando
 * ───────────────────────────────────────────────────────────────────────── */

void pool_init(ShardedPool* pool)
{
    int waited_ms = 0;
    while (atomic_get(&pool->workers_ready) < pool->expected_ready)
    {
        xsleep_ms(1);
        if (++waited_ms % 2000 == 0)
            fprintf(stderr, "[pool_init] aguardando workers: %d/%d (%d ms)\n",
                    atomic_get(&pool->workers_ready), pool->expected_ready, waited_ms);
    }
    fprintf(stderr, "[pool_init] todos os workers prontos: %d\n", pool->expected_ready);
}

/* ─────────────────────────────────────────────────────────────────────────
 * API publica: pool_shutdown
 * ───────────────────────────────────────────────────────────────────────── */

void pool_shutdown(ShardedPool* pool)
{
    if (!pool) return;

    atomic_set(&pool->shutdown, 1);

    if (pool->workers) {
        for (int i = 0; i < pool->worker_count; i++)
            atomic_set(&pool->workers[i].state, WSTATE_STOPPING);

        /* Reenvia wakes a cada 1ms ate todos marcarem WSTATE_STOPPED.
         * Se a thread morreu sem setar STOPPED (crash em t.fn ou similar),
         * detecta via WaitForSingleObject(handle, 0) e forca STOPPED. */
        int pending;
        int waited_ms = 0;
        do {
            pending = 0;
            for (int i = 0; i < pool->worker_count; i++) {
                if (atomic_get(&pool->workers[i].state) == WSTATE_STOPPED)
                    continue;

#ifdef XPLATBASE_WIN
                if (pool->workers[i].handle &&
                    WaitForSingleObject(pool->workers[i].handle, 0) == WAIT_OBJECT_0) {
                    /* Thread saiu (normal ou crash) sem setar STOPPED. Forca. */
                    atomic_set(&pool->workers[i].state, WSTATE_STOPPED);
                    fprintf(stderr, "[pool_shutdown] worker[%d] morto sem setar STOPPED — forcando\n", i);
                    continue;
                }
#endif

                pending++;
                thread_wait_wake(&pool->workers[i].wait);
            }
            if (pending) {
                xsleep_ms(1);
                if (++waited_ms % 2000 == 0) {
                    fprintf(stderr, "[pool_shutdown] %d worker(s) ainda nao pararam (%d ms):\n",
                            pending, waited_ms);
                    for (int i = 0; i < pool->worker_count; i++) {
                        int s = atomic_get(&pool->workers[i].state);
                        if (s != WSTATE_STOPPED)
                            fprintf(stderr, "  worker[%d] state=%d\n", i, s);
                    }
                }
            }
        } while (pending);
        fprintf(stderr, "[pool_shutdown] todos os workers pararam\n");

        /* Todos em WSTATE_STOPPED: join e libera. */
        for (int i = 0; i < pool->worker_count; i++) {
            if (pool->workers[i].handle)
                xpl_thread_join(pool->workers[i].handle);
        }
        for (int i = 0; i < pool->worker_count; i++) {
            thread_wait_destroy(&pool->workers[i].wait);
            free(pool->workers[i].ring_buf);
            ring_queue_destroy(&pool->workers[i].ring);
        }
        free(pool->workers);
    }

    free(pool);
}

/* ─────────────────────────────────────────────────────────────────────────
 * API publica: pool_stats
 * ───────────────────────────────────────────────────────────────────────── */

void pool_stats(ShardedPool* pool, PoolStats* out)
{
    memset(out, 0, sizeof(*out));
    out->shard_count     = pool->worker_count;
    out->total_submitted = (uint64_t)(unsigned int)atomic_get(&pool->stat_submitted);
    out->submit_failures = (uint64_t)(unsigned int)atomic_get(&pool->stat_failures);
    out->total_handoffs  = (uint64_t)(unsigned int)atomic_get(&pool->stat_stolen);

    int active  = 0;
    int pending = 0;
    for (int i = 0; i < pool->worker_count; i++) {
        if (atomic_get(&pool->workers[i].state) == WSTATE_ACTIVE) active++;
        pending += ring_queue_count(&pool->workers[i].ring);
    }
    out->active_worker_count = active;
    out->pending_tasks       = pending;
}
