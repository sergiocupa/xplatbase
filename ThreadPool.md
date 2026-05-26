
Criar o THREAD POOL

Crie dois arquivos para implementar: thread_pool.c e thread_pool.h

Implementar todo mecanismo
  - Criar os workes, a quantidade por parametro default
  - Criar static void* worker_fn(void* arg), que é o loop de worker
  - Outras funcionalidades abaixo
  - Fazer o mais performatico e simples possivel;


Implementar funcao para substituir pool_wake_one() e thread_wait_sleep()
  - Funcoes em modo misto, que primeiro roda spin, depois wake/wait. A descrição abaixo nao é só funcionamento dela, é interação do o loop do worke.
    - esta funcao, no loop do worke, apos executar FN, verifica 
      - se tem ring.buffer pendente, executar FN deste item pendente. Faz isso até esvaziar ring.buffer.
      - apos executar todos da ring.buffer, entra em loop de teste (loop do spin), X iteracoes.
        - cada iteração do loop do spin, testar se tem ring.buffer a executar.
      - se apos o loop do spin, nao encontrar mais ring.buffer, entao chama o wait().


DETERMINADOR DE SCAPE DE WORKE

    Ele determina o momento de transferir o task longo, para deixar o shard livre. 
    Este worke é apontado para um lista de worker em uso (detached_workers).
    O shard em questao, que agora esta orfão de worke, é pego outro worke de uma fila de workers reserva.
    A fila de reserva de workers é preenchida por outra thread que monitora o consumo. 

    Usar funcoes de thread_activity.h para detectar se task é longo.





#include "atomics.h"
#include "ring_queue.h"
#include "thread_wait.h"

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


bool pool_submit(ShardedPool* pool, task_fn fn, void* arg)
{
    int idx   = atomic_add(&pool->submit_idx, 1);
    int count = atomic_load_acquire(&pool->shard_count);
    Task task = { fn, arg };

    for (int i = 0; i < count; i++) {
        Shard* s = pool->shards[(idx + i) % count];
        if (s && xring_push_mp(&s->ring, s->buffer, &task)) {
            atomic_add(&pool->pending, 1);
            shard_wake(s, pool);

            // proativo: sinaliza sob pressão, monitor decide
            if (shard_usage_pct(s) >= PRESSURE_THRESHOLD)
                atomic_store(&pool->expand_requested, true);

            return true;
        }
    }
    // reativo: defesa em profundidade
    atomic_add(&pool->submit_fail_count, 1);
    atomic_store(&pool->expand_requested, true);
    return false;
}





funcoes de thread_activity.h 

void xthread_activity_init(void);
xthread_sample_t xthread_sample_self(void);
xthread_sample_t xthread_sample_of(xthread_handle_t h);
xtask_eval_t xthread_evaluate_task(const xthread_sample_t* start, const xthread_sample_t* now, const xtask_thresholds_t* t);
bool xthread_should_handoff(const xtask_eval_t* eval);









Utilitario para pegar tempo de uso da cpu em funcao da task. Usado para decidir de task é longa ou não.

HEADER

/*
 * xthread_activity.h
 *
 * Cross-platform thread activity tracking for detecting long-running
 * tasks and distinguishing CPU-bound from I/O-bound execution.
 *
 * Windows: QueryThreadCycleTime + QueryPerformanceCounter
 * Linux:   clock_gettime(CLOCK_THREAD_CPUTIME_ID) + CLOCK_MONOTONIC
 *
 * Used by the thread pool monitor to decide whether to hand off a
 * worker that's been running a long task. The key insight: a thread
 * blocked in a syscall accumulates wall time but NOT CPU activity,
 * so ratio (cpu_activity / wall_time) reveals what the thread is
 * actually doing without cooperation from the task code.
 */

#ifndef XTHREAD_ACTIVITY_H
#define XTHREAD_ACTIVITY_H

#include <stdint.h>
#include <stdbool.h>

#ifdef _WIN32
    #include <windows.h>
    typedef HANDLE xthread_handle_t;
#else
    #include <pthread.h>
    typedef pthread_t xthread_handle_t;
#endif

/* -------------------------------------------------------------------------
 * Snapshot of a thread's timing state at a point in time.
 * Stored by the worker when it picks up a task; compared by the monitor
 * to decide whether the task has gone long.
 * ------------------------------------------------------------------------- */
typedef struct {
    uint64_t wall_ns;      /* wall-clock time, nanoseconds */
    uint64_t activity;     /* platform-specific: cycles (Win) or CPU ns (Linux) */
} xthread_sample_t;

/* -------------------------------------------------------------------------
 * Classification of a long-running task.
 * ------------------------------------------------------------------------- */
typedef enum {
    XTASK_STATE_NORMAL,        /* below threshold, not long yet */
    XTASK_STATE_LONG_CPU,      /* long AND burning CPU -> don't hand off */
    XTASK_STATE_LONG_BLOCKED,  /* long AND low CPU -> likely I/O, hand off */
    XTASK_STATE_LONG_UNCLEAR   /* long but ratio is ambiguous -> conservative: don't hand off */
} xtask_state_t;

/* -------------------------------------------------------------------------
 * Diagnostic info produced by xthread_evaluate_task.
 * Useful for logging, metrics, and tuning thresholds.
 * ------------------------------------------------------------------------- */
typedef struct {
    xtask_state_t state;
    uint64_t      wall_elapsed_ns;
    uint64_t      activity_delta;
    double        cpu_ratio;      /* 0.0 .. 1.0 (normalized across platforms) */
} xtask_eval_t;

/* -------------------------------------------------------------------------
 * Tuning thresholds. Exposed so the pool can configure them.
 * Defaults are sensible for typical server workloads but may want
 * adjustment for industrial / real-time contexts.
 * ------------------------------------------------------------------------- */
typedef struct {
    uint64_t long_threshold_ns;   /* task considered "long" beyond this */
    double   blocked_ratio_max;   /* cpu_ratio below this => BLOCKED */
    double   cpu_ratio_min;       /* cpu_ratio above this => CPU_BOUND */
                                  /* between the two => UNCLEAR */
} xtask_thresholds_t;

/* Sensible defaults. */
#define XTASK_DEFAULT_LONG_NS        ((uint64_t)30 * 1000 * 1000)  /* 30 ms */
#define XTASK_DEFAULT_BLOCKED_RATIO  0.30
#define XTASK_DEFAULT_CPU_RATIO      0.70

/* -------------------------------------------------------------------------
 * One-time process init. Call once before using other functions.
 * On Windows, calibrates cycles <-> ns conversion. Safe to call multiple
 * times; only the first call does work.
 * ------------------------------------------------------------------------- */
void xthread_activity_init(void);

/* -------------------------------------------------------------------------
 * Take a snapshot of the calling thread's wall time and CPU activity.
 * The worker calls this right before invoking the task function.
 * Very cheap: vDSO-backed on Linux (~30ns), RDTSC-backed on Windows (~100ns).
 * ------------------------------------------------------------------------- */
xthread_sample_t xthread_sample_self(void);

/* -------------------------------------------------------------------------
 * Take a snapshot of another thread's wall time and CPU activity.
 * The monitor calls this when checking whether a worker's task has
 * run long. Requires a valid thread handle.
 *
 * On Windows: handle must have THREAD_QUERY_INFORMATION access.
 * On Linux:   uses pthread_getcpuclockid. Thread must still be alive.
 *
 * Returns {0,0} if the query fails.
 * ------------------------------------------------------------------------- */
xthread_sample_t xthread_sample_of(xthread_handle_t h);

/* -------------------------------------------------------------------------
 * Compare two samples and classify the task.
 *
 * 'start' was taken by the worker before invoking fn.
 * 'now'   was taken by the monitor just now.
 * 't'     defines thresholds (may be NULL to use defaults).
 *
 * This is pure arithmetic; no syscalls. The monitor typically:
 *   1) reads worker->start_sample (set by worker pre-task)
 *   2) takes xthread_sample_of(worker_handle)
 *   3) calls xthread_evaluate_task(&start, &now, NULL)
 *   4) acts on result.state
 * ------------------------------------------------------------------------- */
xtask_eval_t xthread_evaluate_task(const xthread_sample_t*   start,
                                   const xthread_sample_t*   now,
                                   const xtask_thresholds_t* t);

/* -------------------------------------------------------------------------
 * Convenience: returns true iff the worker should be handed off.
 * Equivalent to (eval.state == XTASK_STATE_LONG_BLOCKED).
 * ------------------------------------------------------------------------- */
bool xthread_should_handoff(const xtask_eval_t* eval);

#endif /* XTHREAD_ACTIVITY_H */






CODE C WIN

/*
 * xthread_activity_win.c
 *
 * Windows implementation using:
 *   - QueryPerformanceCounter for wall time (sub-microsecond, TSC-backed)
 *   - QueryThreadCycleTime    for thread CPU activity (cycles, not tick-quantized)
 *
 * Both are native, cheap, and don't require elevated privileges.
 * QueryThreadCycleTime is specifically designed for this use case since Vista.
 */

#ifdef _WIN32

#include "xthread_activity.h"
#include <windows.h>

static LARGE_INTEGER g_qpc_freq;          /* counts per second */
static double        g_cycles_per_ns = 0.0; /* estimated; used only for normalization */
static LONG          g_initialized = 0;

/* -------------------------------------------------------------------------
 * Cycles to nanoseconds conversion is approximate on Windows because
 * CPU frequency varies (turbo boost, SpeedStep, etc). We don't need
 * absolute accuracy — only a consistent mapping so cpu_ratio is
 * comparable across platforms.
 *
 * Strategy: at init, measure how many cycles accumulate in a short
 * busy loop of known QPC duration. This gives a base frequency that's
 * "good enough" for ratio purposes.
 *
 * Alternative: hardcode assumption (e.g., 2.4 GHz). Less portable but
 * zero-calibration. We pick calibration for accuracy.
 * ------------------------------------------------------------------------- */
static void calibrate_cycles_per_ns(void) {
    HANDLE self = GetCurrentThread();
    ULONG64 cycles_start, cycles_end;
    LARGE_INTEGER qpc_start, qpc_end;

    /* Pin to current core during calibration to avoid TSC drift between cores. */
    DWORD_PTR old_affinity = SetThreadAffinityMask(self, 1);

    QueryThreadCycleTime(self, &cycles_start);
    QueryPerformanceCounter(&qpc_start);

    /* Busy-wait ~10 ms */
    LARGE_INTEGER target;
    target.QuadPart = qpc_start.QuadPart + (g_qpc_freq.QuadPart / 100);
    LARGE_INTEGER now;
    do {
        QueryPerformanceCounter(&now);
    } while (now.QuadPart < target.QuadPart);

    QueryThreadCycleTime(self, &cycles_end);
    QueryPerformanceCounter(&qpc_end);

    if (old_affinity) SetThreadAffinityMask(self, old_affinity);

    uint64_t cycles_delta = cycles_end - cycles_start;
    uint64_t qpc_delta    = qpc_end.QuadPart - qpc_start.QuadPart;
    double   ns_delta     = ((double)qpc_delta * 1e9) / (double)g_qpc_freq.QuadPart;

    if (ns_delta > 0.0 && cycles_delta > 0) {
        g_cycles_per_ns = (double)cycles_delta / ns_delta;
    } else {
        g_cycles_per_ns = 2.4; /* fallback: assume 2.4 GHz */
    }
}

void xthread_activity_init(void) {
    if (InterlockedCompareExchange(&g_initialized, 1, 0) != 0) return;
    QueryPerformanceFrequency(&g_qpc_freq);
    calibrate_cycles_per_ns();
}

static inline uint64_t qpc_to_ns(LARGE_INTEGER c) {
    /* Avoid overflow: split the multiplication. */
    uint64_t secs = c.QuadPart / g_qpc_freq.QuadPart;
    uint64_t rem  = c.QuadPart % g_qpc_freq.QuadPart;
    return secs * 1000000000ULL + (rem * 1000000000ULL) / g_qpc_freq.QuadPart;
}

xthread_sample_t xthread_sample_self(void) {
    xthread_sample_t s = {0, 0};
    LARGE_INTEGER qpc;
    ULONG64 cycles = 0;

    QueryPerformanceCounter(&qpc);
    QueryThreadCycleTime(GetCurrentThread(), &cycles);

    s.wall_ns  = qpc_to_ns(qpc);
    s.activity = cycles;
    return s;
}

xthread_sample_t xthread_sample_of(xthread_handle_t h) {
    xthread_sample_t s = {0, 0};
    LARGE_INTEGER qpc;
    ULONG64 cycles = 0;

    QueryPerformanceCounter(&qpc);
    if (!QueryThreadCycleTime(h, &cycles)) {
        /* thread may have exited or handle lacks rights */
        return s;
    }

    s.wall_ns  = qpc_to_ns(qpc);
    s.activity = cycles;
    return s;
}

#endif /* _WIN32 */





CODE C LINUX


/*
 * xthread_activity_linux.c
 *
 * Linux implementation using:
 *   - clock_gettime(CLOCK_MONOTONIC)        for wall time
 *   - clock_gettime(CLOCK_THREAD_CPUTIME_ID) for thread CPU time
 *   - pthread_getcpuclockid for reading another thread's CPU time
 *
 * Both clocks typically run via vDSO (no syscall trap), ~30-50ns per read.
 * CPU time has true nanosecond resolution — no scheduler tick quantization.
 */

#ifndef _WIN32

#define _GNU_SOURCE
#include "xthread_activity.h"
#include <time.h>
#include <pthread.h>

void xthread_activity_init(void) {
    /* Nothing to calibrate — clocks are already in ns. */
}

static inline uint64_t ts_to_ns(struct timespec ts) {
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

xthread_sample_t xthread_sample_self(void) {
    xthread_sample_t s = {0, 0};
    struct timespec wall, cpu;

    clock_gettime(CLOCK_MONOTONIC, &wall);
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu);

    s.wall_ns  = ts_to_ns(wall);
    s.activity = ts_to_ns(cpu);  /* CPU ns; same unit as wall */
    return s;
}

xthread_sample_t xthread_sample_of(xthread_handle_t h) {
    xthread_sample_t s = {0, 0};
    struct timespec wall, cpu;
    clockid_t cid;

    clock_gettime(CLOCK_MONOTONIC, &wall);

    if (pthread_getcpuclockid(h, &cid) != 0) return s;
    if (clock_gettime(cid, &cpu) != 0)       return s;

    s.wall_ns  = ts_to_ns(wall);
    s.activity = ts_to_ns(cpu);
    return s;
}

#endif /* !_WIN32 */







COMMON

/*
 * xthread_activity_common.c
 *
 * Platform-independent: decision logic on top of the samples.
 * This is the heart of the handoff decision — it normalizes
 * platform-specific "activity" units into a ratio and classifies
 * the task state.
 */

#include "xthread_activity.h"

/* Forward from the platform-specific files. */
#ifdef _WIN32
extern double g_cycles_per_ns; /* calibrated at init */
#endif

static double normalize_ratio(uint64_t activity_delta, uint64_t wall_delta_ns) {
    if (wall_delta_ns == 0) return 0.0;

#ifdef _WIN32
    /* Windows: activity is in cycles. Convert to ns-equivalent using
     * calibrated frequency. The calibration is approximate (CPU freq
     * varies with turbo/throttling), but for a ratio we only need
     * rough consistency.
     *
     * cpu_ns_equiv = cycles / cycles_per_ns
     * ratio        = cpu_ns_equiv / wall_ns
     */
    if (g_cycles_per_ns <= 0.0) return 0.0;
    double cpu_ns_equiv = (double)activity_delta / g_cycles_per_ns;
    double ratio = cpu_ns_equiv / (double)wall_delta_ns;
#else
    /* Linux: activity is CPU ns directly. Ratio is straight division. */
    double ratio = (double)activity_delta / (double)wall_delta_ns;
#endif

    /* Clamp to [0, 1]. Values > 1 can happen briefly due to measurement
     * noise or multiple cores being counted (shouldn't happen for a
     * single thread, but defensive). */
    if (ratio < 0.0) ratio = 0.0;
    if (ratio > 1.0) ratio = 1.0;
    return ratio;
}

xtask_eval_t xthread_evaluate_task(const xthread_sample_t*   start,
                                   const xthread_sample_t*   now,
                                   const xtask_thresholds_t* t)
{
    xtask_eval_t result = {XTASK_STATE_NORMAL, 0, 0, 0.0};

    /* Defaults if caller passes NULL. */
    uint64_t long_ns       = XTASK_DEFAULT_LONG_NS;
    double   blocked_max   = XTASK_DEFAULT_BLOCKED_RATIO;
    double   cpu_min       = XTASK_DEFAULT_CPU_RATIO;
    if (t) {
        long_ns     = t->long_threshold_ns;
        blocked_max = t->blocked_ratio_max;
        cpu_min     = t->cpu_ratio_min;
    }

    /* Defensive: malformed samples (e.g., sample_of failed). */
    if (!start || !now || start->wall_ns == 0 || now->wall_ns < start->wall_ns) {
        return result;
    }

    uint64_t wall_delta = now->wall_ns - start->wall_ns;
    uint64_t act_delta  = (now->activity >= start->activity)
                        ? (now->activity - start->activity)
                        : 0;

    result.wall_elapsed_ns = wall_delta;
    result.activity_delta  = act_delta;
    result.cpu_ratio       = normalize_ratio(act_delta, wall_delta);

    /* Not long yet — no classification needed. */
    if (wall_delta < long_ns) {
        result.state = XTASK_STATE_NORMAL;
        return result;
    }

    /* Long task. Classify by CPU ratio. */
    if (result.cpu_ratio < blocked_max) {
        result.state = XTASK_STATE_LONG_BLOCKED;   /* I/O-bound: hand off */
    } else if (result.cpu_ratio >= cpu_min) {
        result.state = XTASK_STATE_LONG_CPU;       /* CPU-bound: let it run */
    } else {
        result.state = XTASK_STATE_LONG_UNCLEAR;   /* ambiguous zone */
    }
    return result;
}

bool xthread_should_handoff(const xtask_eval_t* eval) {
    return eval && eval->state == XTASK_STATE_LONG_BLOCKED;
}






TEST

/*
 * example_integration.c
 *
 * Shows how the thread pool uses xthread_activity:
 *   - Worker calls xthread_sample_self() before invoking task.fn
 *   - Monitor periodically samples each worker's thread and evaluates
 *   - If LONG_BLOCKED, monitor triggers handoff (not implemented here,
 *     just illustrated)
 *
 * This is illustrative — real integration would use your Shard/Worker
 * structures and atomic ops from xplatbase.
 */

#include "xthread_activity.h"
#include <stdio.h>

/* Simplified worker state for illustration. */
typedef struct Worker {
    xthread_handle_t  thread;
    /* Updated by worker before each task; read by monitor. */
    xthread_sample_t  task_start_sample;
    /* Flag so monitor knows whether task_start_sample is valid. */
    volatile int      task_in_progress;  /* atomic in real code */
    int               id;
} Worker;

typedef void (*task_fn)(void*);
typedef struct { task_fn fn; void* arg; } Task;

/* -------------------------------------------------------------------------
 * Worker side: wrap every task with sampling.
 * Call this inside the worker loop after pop from the ring.
 * ------------------------------------------------------------------------- */
static void worker_run_task(Worker* w, Task t) {
    w->task_start_sample = xthread_sample_self();
    w->task_in_progress  = 1;   /* publish — real code: atomic_store_release */

    t.fn(t.arg);

    w->task_in_progress  = 0;   /* clear */
}

/* -------------------------------------------------------------------------
 * Monitor side: check one worker, decide whether to hand it off.
 * Returns true if handoff is warranted.
 * ------------------------------------------------------------------------- */
static bool monitor_check_worker(Worker* w,
                                 const xtask_thresholds_t* thresholds,
                                 xtask_eval_t* out_eval)
{
    /* Fast path: no task running. */
    if (!w->task_in_progress) return false;   /* real code: atomic_load_acquire */

    xthread_sample_t now = xthread_sample_of(w->thread);
    if (now.wall_ns == 0) return false;  /* query failed, skip */

    xtask_eval_t eval = xthread_evaluate_task(&w->task_start_sample, &now, thresholds);
    if (out_eval) *out_eval = eval;

    return xthread_should_handoff(&eval);
}

/* -------------------------------------------------------------------------
 * Example monitor loop excerpt. Real monitor would integrate with
 * pool->shutdown, expand_requested, etc.
 * ------------------------------------------------------------------------- */
static void example_monitor_cycle(Worker* workers, int count) {
    xtask_thresholds_t thresh = {
        .long_threshold_ns = XTASK_DEFAULT_LONG_NS,
        .blocked_ratio_max = XTASK_DEFAULT_BLOCKED_RATIO,
        .cpu_ratio_min     = XTASK_DEFAULT_CPU_RATIO
    };

    for (int i = 0; i < count; i++) {
        xtask_eval_t eval;
        if (monitor_check_worker(&workers[i], &thresh, &eval)) {
            printf("worker %d: long+blocked (wall=%.1fms, ratio=%.2f) -> HANDOFF\n",
                   workers[i].id,
                   (double)eval.wall_elapsed_ns / 1e6,
                   eval.cpu_ratio);
            /* pool_handoff_worker(pool, &workers[i]); */
        } else if (eval.state == XTASK_STATE_LONG_CPU) {
            printf("worker %d: long but CPU-bound (wall=%.1fms, ratio=%.2f) -> let run\n",
                   workers[i].id,
                   (double)eval.wall_elapsed_ns / 1e6,
                   eval.cpu_ratio);
        } else if (eval.state == XTASK_STATE_LONG_UNCLEAR) {
            printf("worker %d: long, unclear (wall=%.1fms, ratio=%.2f) -> conservative\n",
                   workers[i].id,
                   (double)eval.wall_elapsed_ns / 1e6,
                   eval.cpu_ratio);
        }
    }
}

/* -------------------------------------------------------------------------
 * Minimal smoke test: simulates a task that does work + sleep, checks
 * that evaluation produces sensible output.
 * ------------------------------------------------------------------------- */
#ifdef XTHREAD_ACTIVITY_SMOKE_TEST

#include <unistd.h>
#include <stdlib.h>

static void dummy_cpu_task(void* arg) {
    (void)arg;
    /* burn ~50ms of CPU */
    volatile uint64_t x = 0;
    xthread_sample_t s0 = xthread_sample_self();
    while (1) {
        for (int i = 0; i < 100000; i++) x ^= i;
        xthread_sample_t s1 = xthread_sample_self();
        if (s1.wall_ns - s0.wall_ns > 50000000) break;
    }
}

static void dummy_io_task(void* arg) {
    (void)arg;
    usleep(50000);  /* 50ms sleep — pure I/O-bound behavior */
}

int main(void) {
    xthread_activity_init();

    printf("=== CPU-bound task test ===\n");
    xthread_sample_t s0 = xthread_sample_self();
    dummy_cpu_task(NULL);
    xthread_sample_t s1 = xthread_sample_self();
    xtask_eval_t e_cpu = xthread_evaluate_task(&s0, &s1, NULL);
    printf("wall=%.2fms activity=%llu ratio=%.3f state=%d\n",
           (double)e_cpu.wall_elapsed_ns / 1e6,
           (unsigned long long)e_cpu.activity_delta,
           e_cpu.cpu_ratio, (int)e_cpu.state);

    printf("\n=== I/O-bound task test ===\n");
    s0 = xthread_sample_self();
    dummy_io_task(NULL);
    s1 = xthread_sample_self();
    xtask_eval_t e_io = xthread_evaluate_task(&s0, &s1, NULL);
    printf("wall=%.2fms activity=%llu ratio=%.3f state=%d\n",
           (double)e_io.wall_elapsed_ns / 1e6,
           (unsigned long long)e_io.activity_delta,
           e_io.cpu_ratio, (int)e_io.state);

    printf("\nExpected: CPU task -> LONG_CPU (state=1), I/O task -> LONG_BLOCKED (state=2)\n");
    return 0;
}

#endif