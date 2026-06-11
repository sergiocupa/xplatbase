/*
 * thread_pool.c — WSPool (work-stealing) com handoff de tasks longas
 *
 * Lanes: cada lane tem ring + wait + worker ativo. Submit publica numa lane.
 * Worker: thread executora; pode estar atribuida a uma lane ou na reserva.
 * Reserva: ring MPMC de WSWorker* prontos pra assumir uma lane.
 * Monitor reserva: refaz a reserva quando esgotada (expansao 1.5x).
 * Monitor tasks longas: varre lanes; se worker passou do threshold, faz handoff.
 *
 * Spin progressivo:
 *   P1: xcpu_pause × N   — 0 syscall, 100% CPU
 *   P2: SwitchToThread × N
 *   P3: Sleep(0) × N + amostragem de vizinhos (detecao oportunista)
 *   P4: WaitOnAddress 1ms
 */

#include "thread_pool.h"

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>

#ifdef POOL_TEST_HOOKS
static volatile LONG g_pool_test_alloc_after = -1;
static volatile LONG g_pool_test_thread_start_after = -1;
static volatile LONG g_pool_test_handoff_target = 0;
static volatile LONG g_pool_test_handoff_arrivals = 0;
static volatile LONG g_pool_test_lane_barrier_enabled = 0;
static volatile LONG g_pool_test_max_spin_iterations = 0;
static volatile LONG g_pool_test_orphan_transition = 0;
static HANDLE g_pool_test_handoff_event = NULL;
static HANDLE g_pool_test_lane_arrived_event = NULL;
static HANDLE g_pool_test_lane_release_event = NULL;

static int pool_test_countdown_should_fail(volatile LONG* countdown)
{
    for (;;) {
        LONG current = InterlockedCompareExchange(countdown, 0, 0);
        if (current < 0) return 0;
        if (current == 0) {
            if (InterlockedCompareExchange(countdown, -1, 0) == 0) return 1;
            continue;
        }
        if (InterlockedCompareExchange(countdown, current - 1, current) == current)
            return 0;
    }
}

void pool_test_fail_alloc_after(int successful_allocations)
{
    InterlockedExchange(&g_pool_test_alloc_after, successful_allocations);
}

void pool_test_fail_thread_start_after(int successful_starts)
{
    InterlockedExchange(&g_pool_test_thread_start_after, successful_starts);
}

static void* pool_test_malloc(size_t size)
{
    if (pool_test_countdown_should_fail(&g_pool_test_alloc_after)) return NULL;
    return malloc(size);
}

static void* pool_test_calloc(size_t count, size_t size)
{
    if (pool_test_countdown_should_fail(&g_pool_test_alloc_after)) return NULL;
    return calloc(count, size);
}

#define malloc pool_test_malloc
#define calloc pool_test_calloc

void pool_test_set_handoff_barrier(int target)
{
    if (!g_pool_test_handoff_event)
        g_pool_test_handoff_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    ResetEvent(g_pool_test_handoff_event);
    InterlockedExchange(&g_pool_test_handoff_arrivals, 0);
    InterlockedExchange(&g_pool_test_handoff_target, target);
}

static void pool_test_handoff_barrier_wait(void)
{
    LONG target = InterlockedCompareExchange(&g_pool_test_handoff_target, 0, 0);
    if (target <= 0 || !g_pool_test_handoff_event) return;
    LONG arrivals = InterlockedIncrement(&g_pool_test_handoff_arrivals);
    if (arrivals >= target) SetEvent(g_pool_test_handoff_event);
    WaitForSingleObject(g_pool_test_handoff_event, 2000);
}

void pool_test_enable_lane_release_barrier(void)
{
    if (!g_pool_test_lane_arrived_event)
        g_pool_test_lane_arrived_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_pool_test_lane_release_event)
        g_pool_test_lane_release_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    ResetEvent(g_pool_test_lane_arrived_event);
    ResetEvent(g_pool_test_lane_release_event);
    InterlockedExchange(&g_pool_test_orphan_transition, 0);
    InterlockedExchange(&g_pool_test_lane_barrier_enabled, 1);
}

int pool_test_wait_lane_release_barrier(int timeout_ms)
{
    return WaitForSingleObject(g_pool_test_lane_arrived_event, (DWORD)timeout_ms) == WAIT_OBJECT_0;
}

void pool_test_release_lane_barrier(void)
{
    InterlockedExchange(&g_pool_test_lane_barrier_enabled, 0);
    SetEvent(g_pool_test_lane_release_event);
}

static void pool_test_lane_release_barrier_wait(void)
{
    if (!InterlockedCompareExchange(&g_pool_test_lane_barrier_enabled, 0, 0)) return;
    SetEvent(g_pool_test_lane_arrived_event);
    WaitForSingleObject(g_pool_test_lane_release_event, 5000);
}

/* Barreira da "janela orfa": diferente da lane_release (que para o owner ANTES
 * do re-check de active_lanes), esta para o owner DEPOIS do re-check e ANTES do
 * CAS worker->NULL — exatamente a janela TOCTOU em que uma reativacao concorrente
 * pode deixar a lane ativa sem owner (orfa). So bloqueia quando armada. */
static volatile LONG g_pool_test_orphan_window_enabled = 0;
static HANDLE g_pool_test_orphan_window_arrived_event = NULL;
static HANDLE g_pool_test_orphan_window_release_event = NULL;

void pool_test_enable_orphan_window_barrier(void)
{
    if (!g_pool_test_orphan_window_arrived_event)
        g_pool_test_orphan_window_arrived_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_pool_test_orphan_window_release_event)
        g_pool_test_orphan_window_release_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    ResetEvent(g_pool_test_orphan_window_arrived_event);
    ResetEvent(g_pool_test_orphan_window_release_event);
    InterlockedExchange(&g_pool_test_orphan_window_enabled, 1);
}

int pool_test_wait_orphan_window_arrived(int timeout_ms)
{
    if (!g_pool_test_orphan_window_arrived_event) return 0;
    return WaitForSingleObject(g_pool_test_orphan_window_arrived_event,
                               (DWORD)timeout_ms) == WAIT_OBJECT_0;
}

void pool_test_release_orphan_window_barrier(void)
{
    InterlockedExchange(&g_pool_test_orphan_window_enabled, 0);
    if (g_pool_test_orphan_window_release_event)
        SetEvent(g_pool_test_orphan_window_release_event);
}

static void pool_test_orphan_window_wait(void)
{
    if (!InterlockedCompareExchange(&g_pool_test_orphan_window_enabled, 0, 0)) return;
    SetEvent(g_pool_test_orphan_window_arrived_event);
    WaitForSingleObject(g_pool_test_orphan_window_release_event, 5000);
}

void pool_test_reset_spin_observation(void)
{
    InterlockedExchange(&g_pool_test_max_spin_iterations, 0);
}

int pool_test_max_spin_iterations_observed(void)
{
    return (int)InterlockedCompareExchange(&g_pool_test_max_spin_iterations, 0, 0);
}

static void pool_test_record_spin_iterations(int iterations)
{
    LONG current = InterlockedCompareExchange(&g_pool_test_max_spin_iterations, 0, 0);
    while (current < iterations) {
        LONG previous = InterlockedCompareExchange(&g_pool_test_max_spin_iterations,
                                                   iterations, current);
        if (previous == current) break;
        current = previous;
    }
}
#endif

/* Log da biblioteca via hook registravel (pool_set_log_hook). Sem hook fica
 * silencioso por padrao; com -DPOOL_VERBOSE tambem ecoa no stderr. */
static pool_log_fn g_pool_log_hook = NULL;

void pool_set_log_hook(pool_log_fn fn) { g_pool_log_hook = fn; }

static void pool_log(int sev, const char* fmt, ...)
{
    pool_log_fn hook = g_pool_log_hook;
#ifndef POOL_VERBOSE
    if (!hook) return;                 /* silencioso sem hook */
#endif
    char buf[256];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    if (hook) hook(sev, buf);
#ifdef POOL_VERBOSE
    fprintf(stderr, "%s\n", buf);
#endif
    (void)sev;
}

#ifdef XPLATBASE_WIN
    #include <intrin.h>
    typedef HANDLE           xpl_thread_t;
    typedef DWORD WINAPI     xpl_fn_sig(void*);
    static xpl_thread_t xpl_thread_start(xpl_fn_sig* fn, void* arg) {
#ifdef POOL_TEST_HOOKS
        if (pool_test_countdown_should_fail(&g_pool_test_thread_start_after)) return NULL;
#endif
        return CreateThread(NULL, 0, fn, arg, 0, NULL);
    }
    static void xpl_thread_join(xpl_thread_t h) {
        WaitForSingleObject(h, INFINITE); CloseHandle(h);
    }
    static void xpl_yield(void)  { SwitchToThread(); }
    static void xpl_sleep0(void) { Sleep(0); }
    static uint64_t xpl_tsc(void){ return (uint64_t)__rdtsc(); }
    static uint64_t xpl_rdtscp(void){ unsigned int aux; return (uint64_t)__rdtscp(&aux); }
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
    static uint64_t xpl_rdtscp(void) {
        #if defined(__x86_64__)||defined(__i386__)
            unsigned int aux; return (uint64_t)__rdtscp(&aux);
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
 * Estruturas
 * ───────────────────────────────────────────────────────────────────────── */

typedef struct WSWorker WSWorker;

typedef struct WSLane {
    RingQueue    ring;
    void*        ring_buf;
    xwait_t      wait;             /* dormir/acordar worker atribuido a essa lane */
    xatomic_ptr  worker;           /* WSWorker* owner da lane (apenas referencia) */
    xatomic_int  rescue_helpers;   /* nº de ajudantes de resgate atuando agora */
} WSLane;

struct WSWorker {
    ShardedPool*    pool;
    xpl_thread_t    handle;
    int             id;                /* ID estavel pra debug */

    xatomic_int     state;             /* WSTATE_ACTIVE/STOPPING/STOPPED */
    xatomic_int     detached;          /* 1 = sair da lane apos task atual */
    xatomic_int     rescue_mode;       /* 1 = ajudante transitorio; volta a reserva ao ociar */
    xatomic_ptr     lane;              /* WSLane* atribuida; NULL = na reserva */
    xatomic_int64   task_start_tsc;    /* TSC quando comecou t.fn; 0 = ocioso */
    xatomic_int64   task_start_wall_ns;
    xatomic_int64   task_start_activity;

    xwait_t         reserve_wait;      /* wait quando esta na reserva */
    int             force_killed;      /* 1 = terminado a forca no shutdown (leak controlado) */
};

struct ShardedPool {
    WSLane*         lanes;
    int             lane_capacity;     /* lanes pre-alocadas (= max_shards)           */
    int             lanes_initialized;
    int             expand_cap;        /* teto da expansao AUTOMATICA (<= lane_capacity) */
    int             initial_lanes;     /* nº inicial = shard_count (piso da contracao) */
    xatomic_int     active_lanes;      /* lanes ativas p/ submit (cresce/encolhe)     */
    xatomic_int     max_active_ever;   /* high-water: steal/monitor varrem este range  */

    WSWorker**      all_workers;
    xatomic_int     all_worker_count;
    int             all_worker_capacity;

    RingQueue       reserve_ring;        /* MPMC de WSWorker* */
    void*           reserve_ring_buf;
    int             reserve_capacity;
    xatomic_int     reserve_target;      /* tamanho alvo (cresce 1.5x quando esgota) */
    xatomic_int     reserve_count;       /* aproximado; usado para diagnostico */

    xwait_t         reserve_monitor_wait;
    xpl_thread_t    reserve_monitor;
    xatomic_int     reserve_monitor_running;

    xwait_t         long_task_monitor_wait;
    xpl_thread_t    long_task_monitor;
    xatomic_int     long_task_monitor_running;

    uint64_t        long_threshold_tsc;
    int             long_monitor_interval_ms;
    xtask_thresholds_t task_thresholds;
    int             use_task_thresholds;

    xatomic_int     shutdown;
    xatomic_int     stop_workers;
    xatomic_int     pending_tasks;
    xatomic_int     submit_seq;
    xatomic_int     workers_ready;
    int             expected_ready;

    xatomic_int     stat_submitted;
    xatomic_int     stat_failures;
    xatomic_int     stat_stolen;
    xatomic_int     stat_handoffs;
    xatomic_int     stat_rescued;
    xatomic_int     stat_backpressure;   /* nº de vezes que submit teve que esperar */
    xatomic_int     stat_expansions;     /* lanes ativadas dinamicamente */

    uint64_t        spin_budget_cycles;
    int             spin_iterations;
    int             ring_capacity;

    int             rescue_backlog_threshold;
    int             rescue_max_helpers;     /* teto absoluto de ajudantes por lane */
    uint64_t        park_threshold_tsc;     /* 0 = parking desativado */
    xatomic_int     warned_oversubscribe;   /* aviso de cores: emitido 1x */
    xatomic_int     warned_capacity;        /* aviso de capacidade esgotada: emitido 1x */

    xatomic_int     busy_workers;           /* workers executando task agora (ocupacao) */
    uint64_t        rescue_wait_unit_tsc;   /* unidade p/ ranking de tempo de espera */

    int             low_load_scans;         /* histerese de contracao (so o monitor toca) */

    int             shutdown_drain_timeout_ms;
    int             shutdown_join_timeout_ms;
    bool            shutdown_force_kill;
};

/* Total de threads terminadas a forca no processo (todos os pools). */
static xatomic_int g_force_killed_total;
int pool_force_kill_count(void) { return atomic_get(&g_force_killed_total); }

/* ─────────────────────────────────────────────────────────────────────────
 * Forward decls
 * ───────────────────────────────────────────────────────────────────────── */

static XPL_FN worker_fn(void* raw);
static XPL_FN reserve_monitor_fn(void* raw);
static XPL_FN long_task_monitor_fn(void* raw);

static WSWorker* worker_create(ShardedPool* pool);
static bool      worker_start (WSWorker* w);
static bool      activate_lane(ShardedPool* pool);

/* ─────────────────────────────────────────────────────────────────────────
 * Marcador de inicio/fim de task — chamado pelo worker
 * ───────────────────────────────────────────────────────────────────────── */

static inline void worker_mark_task_start(WSWorker* w)
{
    if (w->pool->use_task_thresholds) {
        xthread_sample_t sample = xthread_sample_self();
        atomic_set64(&w->task_start_wall_ns, (int64_t)sample.wall_ns);
        atomic_set64(&w->task_start_activity, (int64_t)sample.activity);
    }

    atomic_set64(&w->task_start_tsc, (int64_t)xpl_rdtscp());
    atomic_add(&w->pool->busy_workers, 1);
}

static inline void worker_mark_task_end(WSWorker* w)
{
    atomic_set64(&w->task_start_tsc, 0);
    atomic_set64(&w->task_start_wall_ns, 0);
    atomic_set64(&w->task_start_activity, 0);
    atomic_sub(&w->pool->busy_workers, 1);
}

/* ─────────────────────────────────────────────────────────────────────────
 * Detecao de task longa
 *   Retorna o TSC do start se a task atual ja excedeu o threshold; 0 caso contrario.
 *   O caller usa esse TSC para CAS-confirmar o handoff (evita race com fim de task).
 * ───────────────────────────────────────────────────────────────────────── */

static uint64_t worker_long_task_start(WSWorker* w, uint64_t now_tsc)
{
    ShardedPool* pool = w->pool;
    uint64_t start = (uint64_t)atomic_get64(&w->task_start_tsc);
    if (start == 0) return 0;
    if (now_tsc - start < pool->long_threshold_tsc) return 0;

    if (pool->use_task_thresholds) {
        xthread_sample_t task_start;
        task_start.wall_ns = (uint64_t)atomic_get64(&w->task_start_wall_ns);
        task_start.activity = (uint64_t)atomic_get64(&w->task_start_activity);
        if (task_start.wall_ns == 0) return 0;

        xthread_sample_t now = xthread_sample_of(w->handle);
        xtask_eval_t eval = xthread_evaluate_task(&task_start, &now,
                                                  &pool->task_thresholds);
        if (!xthread_should_handoff(&eval)) return 0;
    }

    return start;
}

/* ─────────────────────────────────────────────────────────────────────────
 * Reserva — push/pop de WSWorker*
 * ───────────────────────────────────────────────────────────────────────── */

static bool reserve_push(ShardedPool* pool, WSWorker* w)
{
    if (!xring_push_mp(&pool->reserve_ring, pool->reserve_ring_buf, &w))
        return false;
    atomic_add(&pool->reserve_count, 1);
    return true;
}

static WSWorker* reserve_pop(ShardedPool* pool)
{
    WSWorker* w = NULL;
    if (!xring_pop_mc(&pool->reserve_ring, pool->reserve_ring_buf, &w))
        return NULL;
    atomic_sub(&pool->reserve_count, 1);
    return w;
}

static void reserve_monitor_wake(ShardedPool* pool)
{
    thread_wait_wake(&pool->reserve_monitor_wait);
}

/* ─────────────────────────────────────────────────────────────────────────
 * Atribuicao worker ↔ lane
 * ───────────────────────────────────────────────────────────────────────── */

static void worker_assign_to_lane(WSWorker* w, WSLane* lane)
{
    atomic_set_ptr(&w->lane, lane);
    atomic_set_ptr(&lane->worker, w);
    thread_wait_wake(&w->reserve_wait);  /* tira o worker do sleep da reserva */
    thread_wait_wake(&lane->wait);       /* acorda no caso de ja estar na lane */
}

static void worker_leave_lane(WSWorker* w)
{
    atomic_set_ptr(&w->lane, NULL);
}

/* ─────────────────────────────────────────────────────────────────────────
 * Acesso a tasks: ring proprio + steal entre lanes
 * ───────────────────────────────────────────────────────────────────────── */

static bool lane_pop_task(WSLane* lane, Task* out)
{
    return xring_pop_mc(&lane->ring, lane->ring_buf, out);
}

static bool worker_try_steal(WSWorker* w, WSLane* my_lane, Task* out)
{
    ShardedPool* pool = w->pool;
    int n = atomic_get(&pool->max_active_ever);  /* varre ate o high-water: pega residuais de lanes desativadas */
    if (n <= 1) return false;

    int start = (int)((unsigned int)w->id + 1) % n;
    for (int i = 0; i < n; i++) {
        WSLane* victim = &pool->lanes[(start + i) % n];
        if (victim == my_lane) continue;
        if (lane_pop_task(victim, out)) {
            atomic_add(&pool->stat_stolen, 1);
            return true;
        }
    }
    return false;
}

static bool worker_try_any(WSWorker* w, WSLane* my_lane, Task* out)
{
    if (my_lane && lane_pop_task(my_lane, out)) return true;
    return worker_try_steal(w, my_lane, out);
}

/* ─────────────────────────────────────────────────────────────────────────
 * Spin progressivo
 * ───────────────────────────────────────────────────────────────────────── */

static bool spin_check_continue(WSWorker* w)
{
    if (atomic_get(&w->state) != WSTATE_ACTIVE) return false;
    if (atomic_get(&w->pool->stop_workers))     return false;
    if (atomic_get(&w->detached))               return false;
    return true;
}

static bool spin_phase1(WSWorker* w, WSLane* lane, Task* out, uint64_t budget_cycles)
{
    if (budget_cycles > 0) {
        uint64_t deadline = xpl_tsc() + budget_cycles;
        int checks = 0;
        for (;;) {
            if (worker_try_any(w, lane, out)) return true;
            if (!spin_check_continue(w))      return false;
            xcpu_pause();
            if (++checks >= 64) {
                checks = 0;
                if (xpl_tsc() >= deadline) return false;
            }
        }
    }

    int iterations = w->pool->spin_iterations;
    for (int i = 0; i < iterations; i++) {
        if (worker_try_any(w, lane, out)) return true;
        if (!spin_check_continue(w))      return false;
        xcpu_pause();
    }
#ifdef POOL_TEST_HOOKS
    pool_test_record_spin_iterations(iterations);
#endif
    return false;
}

static bool spin_phase2(WSWorker* w, WSLane* lane, Task* out)
{
    for (int i = 0; i < SPIN_P2_ITER; i++) {
        if (worker_try_any(w, lane, out)) return true;
        if (!spin_check_continue(w))      return false;
        xpl_yield();
    }
    return false;
}

/* Amostragem oportunista de vizinhos: detecta tasks longas em workers ativos.
 * Roda apenas quando este worker esta ocioso em P3 (custo amortizado). */
static void sample_neighbors_for_long_tasks(WSWorker* self)
{
    ShardedPool* pool = self->pool;
    uint64_t now      = xpl_rdtscp();

    int n = atomic_get(&pool->max_active_ever);
    for (int i = 0; i < n; i++) {
        WSLane*   lane = &pool->lanes[i];
        WSWorker* w    = (WSWorker*)atomic_get_ptr(&lane->worker);
        if (!w || w == self) continue;
        if (atomic_get(&w->detached)) continue;

        uint64_t start = worker_long_task_start(w, now);
        if (start == 0) continue;

        /* Aciona handoff via monitor — caminho unico evita race entre detectores. */
        thread_wait_wake(&pool->long_task_monitor_wait);
        return;
    }
}

static bool spin_phase3(WSWorker* w, WSLane* lane, Task* out)
{
    sample_neighbors_for_long_tasks(w);

    for (int i = 0; i < SPIN_P3_ITER; i++) {
        if (worker_try_any(w, lane, out)) return true;
        if (!spin_check_continue(w))      return false;
        xpl_sleep0();
    }
    return false;
}

/* ─────────────────────────────────────────────────────────────────────────
 * Worker idle (na lane) — spin progressivo + sleep na lane.wait
 * ───────────────────────────────────────────────────────────────────────── */

static bool worker_idle_on_lane(WSWorker* w, WSLane* lane, Task* out, bool parked)
{
    thread_wait_prepare(&lane->wait);

    if (worker_try_any(w, lane, out)) return true;

    /* Parqueado: pula o spin (economiza CPU) e vai direto ao sono profundo. */
    if (parked) {
        if (!spin_check_continue(w)) return false;
        thread_wait_sleep_for(&lane->wait, POOL_PARK_SLEEP_US);
        return worker_try_any(w, lane, out);
    }

    if (spin_phase1(w, lane, out, w->pool->spin_budget_cycles)) return true;
    if (!spin_check_continue(w)) return false;

    if (spin_phase2(w, lane, out)) return true;
    if (!spin_check_continue(w)) return false;

    if (spin_phase3(w, lane, out)) return true;
    if (!spin_check_continue(w)) return false;

    thread_wait_sleep_for(&lane->wait, 1000);
    return worker_try_any(w, lane, out);
}

/* ─────────────────────────────────────────────────────────────────────────
 * Worker na reserva — dorme aguardando atribuicao
 * ───────────────────────────────────────────────────────────────────────── */

static WSLane* worker_wait_for_assignment(WSWorker* w)
{
    thread_wait_prepare(&w->reserve_wait);
    for (;;) {
        WSLane* lane = (WSLane*)atomic_get_ptr(&w->lane);
        if (lane) return lane;
        if (atomic_get(&w->pool->stop_workers))  return NULL;
        if (atomic_get(&w->state) == WSTATE_STOPPING) return NULL;
        thread_wait_sleep_for(&w->reserve_wait, 100000);  /* 100ms */
    }
}

/* ─────────────────────────────────────────────────────────────────────────
 * Worker — loop principal
 * ───────────────────────────────────────────────────────────────────────── */

static void worker_run_task(WSWorker* w, Task* t)
{
    worker_mark_task_start(w);
    t->fn(t->arg);
    worker_mark_task_end(w);
    atomic_sub(&w->pool->pending_tasks, 1);
}

static void worker_return_to_reserve(WSWorker* w)
{
    ShardedPool* pool = w->pool;
    atomic_set(&w->detached, 0);
    worker_leave_lane(w);
    reserve_push(pool, w);
    reserve_monitor_wake(pool);
}

static XPL_FN worker_fn(void* raw)
{
    WSWorker*    w    = (WSWorker*)raw;
    ShardedPool* pool = w->pool;

    atomic_add(&pool->workers_ready, 1);

    Task t;
    memset(&t, 0, sizeof(t));

    uint64_t last_work = xpl_tsc();  /* timer de ociosidade, local ao thread */

    while (!atomic_get(&pool->stop_workers) && atomic_get(&w->state) != WSTATE_STOPPING)
    {
        WSLane* lane = (WSLane*)atomic_get_ptr(&w->lane);

        if (lane && atomic_get(&w->detached)) {
            worker_return_to_reserve(w);
            continue;
        }

        if (!lane) {
            lane = worker_wait_for_assignment(w);
            if (!lane) break;
            last_work = xpl_tsc();
            continue;
        }

        t.fn = NULL;
        bool got = worker_try_any(w, lane, &t);
        if (!got) {
            bool parked = pool->park_threshold_tsc > 0 &&
                          (xpl_tsc() - last_work) > pool->park_threshold_tsc;
            got = worker_idle_on_lane(w, lane, &t, parked);
        }

        if (got && t.fn) {
            worker_run_task(w, &t);
            last_work = xpl_tsc();

            if (atomic_get(&w->detached)) {
                worker_return_to_reserve(w);
            }
        }
        else if (!got && atomic_get(&w->rescue_mode)) {
            /* Backlog drenado — ajudante de resgate volta para a reserva. */
            WSLane* L = (WSLane*)atomic_get_ptr(&w->lane);
            atomic_set(&w->rescue_mode, 0);
            if (L) atomic_sub(&L->rescue_helpers, 1);
            worker_return_to_reserve(w);
            last_work = xpl_tsc();
        }
        else if (!got) {
            /* Owner de lane DESATIVADA (contracao): se a lane saiu do range
             * ativo e seu ring esvaziou, libera a lane e volta a reserva. */
            WSLane* L = (WSLane*)atomic_get_ptr(&w->lane);
            if (L) {
                int index = (int)(L - pool->lanes);
                if (index >= atomic_get(&pool->active_lanes) &&
                    ring_queue_count(&L->ring) == 0) {
#ifdef POOL_TEST_HOOKS
                    pool_test_lane_release_barrier_wait();
#endif
                    if (index < atomic_get(&pool->active_lanes))
                        continue;
#ifdef POOL_TEST_HOOKS
                    pool_test_orphan_window_wait();
#endif
                    void* expected = w;
                    if (!atomic_cas_ptr(&L->worker, &expected, NULL))
                        continue;

                    /* Fecha a janela TOCTOU: se a lane foi REATIVADA entre o
                     * re-check acima e este CAS, a lane ficaria ativa sem owner
                     * (orfa). Re-reivindica imediatamente; se nao conseguir, e
                     * porque outro worker ja assumiu — em ambos os casos a lane
                     * permanece com owner. */
                    if (index < atomic_get(&pool->active_lanes)) {
                        void* none = NULL;
                        if (atomic_cas_ptr(&L->worker, &none, w))
                            continue;   /* re-assumiu a lane reativada (segue owner) */
#ifdef POOL_TEST_HOOKS
                        /* So sinaliza se restou orfa de verdade (nunca deveria). */
                        if (atomic_get_ptr(&L->worker) == NULL)
                            InterlockedExchange(&g_pool_test_orphan_transition, 1);
#endif
                    }
                    worker_return_to_reserve(w);
                    last_work = xpl_tsc();
                }
            }
        }
    }

    atomic_set(&w->state, WSTATE_STOPPED);
    XPL_RET;
}

/* ─────────────────────────────────────────────────────────────────────────
 * Handoff de uma lane (worker travado em task longa → reserva substitui)
 *   CAS-confirma que o start_tsc nao mudou desde a detecao.
 *   Retorna true se efetivamente realizou o handoff.
 * ───────────────────────────────────────────────────────────────────────── */

static bool perform_handoff(ShardedPool* pool, WSLane* lane, WSWorker* victim, uint64_t expected_start)
{
    int64_t exp = (int64_t)expected_start;
    if (!atomic_cas64(&victim->task_start_tsc, &exp, exp)) {
        /* Task terminou entre detecao e CAS — sem handoff necessario. */
        return false;
    }

#ifdef POOL_TEST_HOOKS
    pool_test_handoff_barrier_wait();
#endif
    WSWorker* spare = reserve_pop(pool);
    if (!spare) {
        /* Reserva vazia — acorda monitor pra refazer; pula handoff desta vez. */
        reserve_monitor_wake(pool);
        return false;
    }

    int not_detached = 0;
    if (!atomic_cas(&victim->detached, &not_detached, 1)) {
        reserve_push(pool, spare);
        return false;
    }

    atomic_set(&spare->rescue_mode, 0);
    atomic_set(&spare->detached, 0);
    atomic_set_ptr(&spare->lane, lane);

    void* expected_owner = victim;
    if (!atomic_cas_ptr(&lane->worker, &expected_owner, spare)) {
        atomic_set_ptr(&spare->lane, NULL);
        atomic_set(&victim->detached, 0);
        reserve_push(pool, spare);
        return false;
    }

    /* Acorda o spare (estava dormindo na reserva). */
    thread_wait_wake(&spare->reserve_wait);
    /* Acorda alguem na lane.wait pra que o novo worker comece imediato. */
    thread_wait_wake(&lane->wait);

    atomic_add(&pool->stat_handoffs, 1);
    reserve_monitor_wake(pool);  /* refazer a reserva */
    return true;
}

/* ─────────────────────────────────────────────────────────────────────────
 * Resgate por backlog — traz ajudantes da reserva para drenar a lane
 *   Diferente do handoff: nao substitui o owner; adiciona workers extras que
 *   puxam do mesmo ring (MPMC) e, ao ociar, voltam para a reserva.
 *
 *   Teto dinamico de ajudantes (fusao dos antigos itens "multiplos ajudantes"
 *   + "ranking decide quantos"): a quantidade desejada e dirigida pela
 *   profundidade da fila (1 ajudante por task pendente, pois o owner esta
 *   ocupado), limitada por: teto absoluto, reserva disponivel e folga de cores
 *   (nunca oversubscrever). lane->rescue_helpers conta os ativos.
 * ───────────────────────────────────────────────────────────────────────── */

static int lane_helper_cap(ShardedPool* pool, int pending)
{
    if (pending < 1) return 0;
    int cap = pending;  /* owner ocupado → cada pendente justifica um ajudante */
    if (pool->rescue_max_helpers > 0 && cap > pool->rescue_max_helpers)
        cap = pool->rescue_max_helpers;
    return cap;
}

static void dispatch_rescue_worker(ShardedPool* pool, WSLane* lane)
{
    for (;;) {
        int pending = ring_queue_count(&lane->ring);
        int cap     = lane_helper_cap(pool, pending);
        int cur     = atomic_get(&lane->rescue_helpers);
        if (cur >= cap) break;

        /* folga de cores: nunca passar do nº de cores logicos ocupados */
        if (atomic_get(&pool->busy_workers) >= xcpu_count()) break;

        int expected = cur;
        if (!atomic_cas(&lane->rescue_helpers, &expected, cur + 1))
            continue;  /* outro dispatcher mexeu — reavalia */

        WSWorker* spare = reserve_pop(pool);
        if (!spare) {
            atomic_sub(&lane->rescue_helpers, 1);  /* reverte o slot reservado */
            reserve_monitor_wake(pool);            /* refazer reserva; backstop tenta de novo */
            break;
        }

        atomic_set(&spare->rescue_mode, 1);
        atomic_set_ptr(&spare->lane, lane);
        thread_wait_wake(&spare->reserve_wait);    /* tira da reserva */
        thread_wait_wake(&lane->wait);
        atomic_add(&pool->stat_rescued, 1);
    }
    reserve_monitor_wake(pool);  /* repor reserva — salvaguarda */
}

/* ─────────────────────────────────────────────────────────────────────────
 * Ranking de resgate — combina 3 indicadores para decidir SEM atropelar o steal
 *   1. profundidade : pending >= rescue_backlog_threshold (default 1)
 *   2. saturacao    : GATE — passa se TODOS ocupados (busy >= lane_count) OU se
 *                     ha mais pendentes que lanes (pending >= lane_count). O 2º
 *                     criterio engata de imediato numa rajada, antes de
 *                     busy_workers atualizar; e nesse caso o steal nao consegue
 *                     espalhar (ha mais backlog que lanes), entao nao atropela.
 *   3. tempo preso  : ha quanto tempo o owner esta na task atual (proxy do
 *                     tempo que a task enfileirada vai esperar).
 *   Retorna 0 = nao resgatar; >0 = urgencia (maior = mais prioritario).
 * ───────────────────────────────────────────────────────────────────────── */

static int lane_rescue_score(ShardedPool* pool, WSLane* lane)
{
    int pending = ring_queue_count(&lane->ring);
    if (pending < pool->rescue_backlog_threshold) return 0;          /* (1) */

    int active = atomic_get(&pool->active_lanes);
    if (atomic_get(&pool->busy_workers) < active &&
        pending < active) return 0;                                  /* (2) gate */

    /* ja tem ajudantes suficientes para a profundidade atual? */
    if (atomic_get(&lane->rescue_helpers) >= lane_helper_cap(pool, pending)) return 0;

    uint64_t wait_score = 0;                                          /* (3) */

    /* Item 5: tempo de espera REAL da cabeca da fila (enqueue_tsc no Task).
     * peek e MPMC-racy, entao defende contra leitura suja limitando a ~5s. */
    Task head;
    if (pool->rescue_wait_unit_tsc &&
        xring_peek(&lane->ring, lane->ring_buf, &head) && head.enqueue_tsc) {
        uint64_t now = xpl_rdtscp();
        if (now > head.enqueue_tsc) {
            uint64_t waited = now - head.enqueue_tsc;
            if (waited < pool->rescue_wait_unit_tsc * 10000ULL)  /* sanidade */
                wait_score = waited / pool->rescue_wait_unit_tsc;
        }
    }

    /* Fallback: proxy pelo tempo que o owner esta preso na task atual. */
    if (wait_score == 0) {
        WSWorker* w = (WSWorker*)atomic_get_ptr(&lane->worker);
        if (w) {
            uint64_t start = (uint64_t)atomic_get64(&w->task_start_tsc);
            if (start) {
                uint64_t busy = xpl_rdtscp() - start;
                wait_score = busy / pool->rescue_wait_unit_tsc;
            }
        }
    }

    return pending * 16 + (int)wait_score;
}

/* Varre as lanes e resgata em ordem de maior score (ranking), enquanto houver
 * reserva disponivel. Usado pelo monitor como backstop. */
static void scan_lanes_for_rescue(ShardedPool* pool)
{
    int n = atomic_get(&pool->active_lanes);
    for (int guard = 0; guard < n; guard++) {
        int best = -1, best_score = 0;
        for (int i = 0; i < n; i++) {
            int s = lane_rescue_score(pool, &pool->lanes[i]);
            if (s > best_score) { best_score = s; best = i; }
        }
        if (best < 0) break;  /* nenhuma lane qualifica */

        int before = atomic_get(&pool->reserve_count);
        dispatch_rescue_worker(pool, &pool->lanes[best]);
        if (atomic_get(&pool->reserve_count) >= before) break;  /* reserva vazia */
    }
}

/* ─────────────────────────────────────────────────────────────────────────
 * Monitor de tasks longas — varre todas as lanes a cada N ms
 * ───────────────────────────────────────────────────────────────────────── */

static void scan_lanes_for_handoff(ShardedPool* pool)
{
    uint64_t now    = xpl_rdtscp();

    int n = atomic_get(&pool->max_active_ever);
    for (int i = 0; i < n; i++) {
        WSLane*   lane = &pool->lanes[i];
        WSWorker* w    = (WSWorker*)atomic_get_ptr(&lane->worker);
        if (!w) continue;
        if (atomic_get(&w->detached)) continue;

        uint64_t start = worker_long_task_start(w, now);
        if (start == 0) continue;

        perform_handoff(pool, lane, w, start);
    }
}

/* Item 6: ajudantes de resgate (rescue_mode) nao estao em lane->worker, entao o
 * scan acima nao os ve. Se um ajudante esta preso em task longa e a lane ainda
 * tem backlog, despacha mais ajuda para a lane continuar drenando. */
static void scan_helpers_for_long_tasks(ShardedPool* pool)
{
    uint64_t now    = xpl_rdtscp();
    int total = atomic_get(&pool->all_worker_count);

    for (int i = 0; i < total; i++) {
        WSWorker* w = pool->all_workers[i];
        if (!w) continue;
        if (!atomic_get(&w->rescue_mode)) continue;   /* so ajudantes */

        WSLane* L = (WSLane*)atomic_get_ptr(&w->lane);
        if (!L) continue;
        if (worker_long_task_start(w, now) == 0) continue;

        if (ring_queue_count(&L->ring) > 0)
            dispatch_rescue_worker(pool, L);           /* lane segue drenando */
    }
}

/* Quantos scans consecutivos de carga baixa antes de contrair 1 lane (histerese).
 * Com intervalo de 10ms, 50 scans ≈ 500ms de ociosidade sustentada. */
#define LANE_CONTRACT_HYSTERESIS  50

/* Expansao proativa: sob carga sustentada (todos ocupados + backlog), ativa
 * 1 lane por scan, conservador. O caminho do submit ja cobre o "ring cheio". */
static void scan_maybe_expand(ShardedPool* pool)
{
    int active = atomic_get(&pool->active_lanes);
    if (active >= pool->expand_cap) return;
    if (atomic_get(&pool->busy_workers) < active) return;  /* ha folga → nao expande */

    int pend = 0;
    for (int i = 0; i < active; i++) pend += ring_queue_count(&pool->lanes[i].ring);
    if (pend >= active) activate_lane(pool);               /* mais backlog que lanes */
}

/* Contracao segura: sob carga baixa sustentada, desativa a lane do topo se ela
 * estiver vazia e acima do nº inicial. O owner volta a reserva ao ociar (ve o
 * index >= active_lanes); residuais de corrida sao drenados via steal
 * (que varre max_active_ever). */
static void scan_maybe_contract(ShardedPool* pool)
{
    int active = atomic_get(&pool->active_lanes);

    bool low = active > pool->initial_lanes &&
               atomic_get(&pool->busy_workers) < active &&
               ring_queue_count(&pool->lanes[active - 1].ring) == 0;

    if (!low) { pool->low_load_scans = 0; return; }

    /* Histerese p/ ENTRAR em contracao; depois contrai 1 lane por scan enquanto
     * a carga seguir baixa (nao zera o contador). */
    if (++pool->low_load_scans < LANE_CONTRACT_HYSTERESIS) return;

    int exp = active;
    atomic_cas(&pool->active_lanes, &exp, active - 1);  /* desativa a top lane */
}

static XPL_FN long_task_monitor_fn(void* raw)
{
    ShardedPool* pool = (ShardedPool*)raw;
    atomic_set(&pool->long_task_monitor_running, 1);
    thread_wait_prepare(&pool->long_task_monitor_wait);

    while (!atomic_get(&pool->shutdown)) {
        thread_wait_sleep_for(&pool->long_task_monitor_wait,
                              pool->long_monitor_interval_ms * 1000);
        if (atomic_get(&pool->shutdown)) break;
        scan_lanes_for_handoff(pool);
        scan_helpers_for_long_tasks(pool);
        scan_lanes_for_rescue(pool);
        scan_maybe_expand(pool);
        scan_maybe_contract(pool);
    }

    atomic_set(&pool->long_task_monitor_running, 0);
    XPL_RET;
}

/* ─────────────────────────────────────────────────────────────────────────
 * Monitor da reserva — refaz workers ate atingir reserve_target
 * ───────────────────────────────────────────────────────────────────────── */

static void grow_reserve_target_if_depleted(ShardedPool* pool)
{
    if (atomic_get(&pool->reserve_count) > 0) return;

    /* Teto: nunca passar de metade do orcamento de workers. Sem isto, sob
     * depleção sustentada (rescue+expansao consumindo a reserva) o target cresce
     * 1.5x a cada scan ate estourar o int. */
    int cap     = pool->all_worker_capacity / 2;
    int current = atomic_get(&pool->reserve_target);
    if (current >= cap) return;                     /* ja no teto: nao cresce */

    int grown = (current * POOL_RESERVE_EXPAND_NUM) / POOL_RESERVE_EXPAND_DEN;
    if (grown <= current) grown = current + 1;
    if (grown > cap)      grown = cap;
    atomic_set(&pool->reserve_target, grown);
}

static void refill_reserve_to_target(ShardedPool* pool)
{
    int target = atomic_get(&pool->reserve_target);
    while (atomic_get(&pool->reserve_count) < target &&
           !atomic_get(&pool->shutdown))
    {
        WSWorker* w = worker_create(pool);
        if (!w) break;
        if (!worker_start(w)) break;
        if (!reserve_push(pool, w)) {
            atomic_set(&w->state, WSTATE_STOPPING);
            thread_wait_wake(&w->reserve_wait);
            break;
        }
    }
}

static XPL_FN reserve_monitor_fn(void* raw)
{
    ShardedPool* pool = (ShardedPool*)raw;
    atomic_set(&pool->reserve_monitor_running, 1);
    thread_wait_prepare(&pool->reserve_monitor_wait);

    while (!atomic_get(&pool->shutdown)) {
        thread_wait_sleep_for(&pool->reserve_monitor_wait, 100000);  /* 100ms */
        if (atomic_get(&pool->shutdown)) break;

        grow_reserve_target_if_depleted(pool);
        refill_reserve_to_target(pool);
    }

    atomic_set(&pool->reserve_monitor_running, 0);
    XPL_RET;
}

/* ─────────────────────────────────────────────────────────────────────────
 * Criacao de worker — usado por pool_create e pelo monitor da reserva
 * ───────────────────────────────────────────────────────────────────────── */

static WSWorker* worker_create(ShardedPool* pool)
{
    int id = atomic_add(&pool->all_worker_count, 1);  /* retorna OLD */
    if (id >= pool->all_worker_capacity) {
        /* Capacidade pre-alocada esgotada — nao expande para evitar race com leitores.
         * Loga 1x (gate) p/ nao floodar sob carga sustentada. */
        atomic_sub(&pool->all_worker_count, 1);
        int warned = 0;
        if (atomic_cas(&pool->warned_capacity, &warned, 1))
            pool_log(POOL_LOG_CRIT, "all_worker_capacity (%d) esgotado",
                     pool->all_worker_capacity);
        return NULL;
    }

    /* Aviso (1x) de oversubscription: total de workers passou dos cores logicos. */
    int ncores = xcpu_count();
    if (id + 1 > ncores) {
        int warned = 0;
        if (atomic_cas(&pool->warned_oversubscribe, &warned, 1)) {
            pool_log(POOL_LOG_WARN, "oversubscription: %d workers / %d cores logicos",
                     id + 1, ncores);
        }
    }

    WSWorker* w = (WSWorker*)calloc(1, sizeof(WSWorker));
    if (!w) {
        atomic_sub(&pool->all_worker_count, 1);
        return NULL;
    }

    w->pool = pool;
    w->id   = id;
    atomic_set(&w->state, WSTATE_ACTIVE);
    thread_wait_prepare(&w->reserve_wait);

    /* Publica DEPOIS de inicializar — outros threads sempre veem worker pronto. */
    atomic_set_ptr((xatomic_ptr*)&pool->all_workers[id], w);
    return w;
}

static bool worker_start(WSWorker* w)
{
    w->handle = xpl_thread_start(worker_fn, w);
    if (!w->handle) {
        atomic_set(&w->state, WSTATE_STOPPED);
        return false;
    }
    return true;
}

/* ─────────────────────────────────────────────────────────────────────────
 * Lanes — criacao
 * ───────────────────────────────────────────────────────────────────────── */

static bool lane_init(WSLane* lane, int ring_capacity)
{
    lane->ring_buf = malloc((size_t)ring_capacity * sizeof(Task));
    if (!lane->ring_buf) return false;
    ring_queue_init(&lane->ring, ring_capacity);
    if (!lane->ring.seqno) {
        free(lane->ring_buf);
        lane->ring_buf = NULL;
        return false;
    }
    thread_wait_prepare(&lane->wait);
    atomic_set_ptr(&lane->worker, NULL);
    return true;
}

static void lane_destroy(WSLane* lane)
{
    thread_wait_destroy(&lane->wait);
    free(lane->ring_buf);
    ring_queue_destroy(&lane->ring);
}

/* ─────────────────────────────────────────────────────────────────────────
 * API publica: pool_default_config
 * ───────────────────────────────────────────────────────────────────────── */

PoolConfig pool_default_config(void)
{
    PoolConfig c;
    memset(&c, 0, sizeof(c));
    c.shard_count            = xcpu_count();
    c.max_shards             = POOL_DEFAULT_MAX_SHARDS;
    c.ring_capacity          = POOL_DEFAULT_RING_CAPACITY;
    c.spin_iterations        = POOL_DEFAULT_SPIN_ITERATIONS;
    c.spin_budget_us         = POOL_DEFAULT_SPIN_BUDGET_US;
    c.reserve_size           = 0;  /* 0 → mesma quantidade de workers */
    c.monitor_interval_ms    = POOL_MONITOR_INTERVAL_MS;
    c.long_task_threshold_ns = POOL_DEFAULT_LONG_TASK_NS;
    c.rescue_backlog_threshold = POOL_DEFAULT_RESCUE_BACKLOG;
    c.rescue_max_helpers_per_lane = POOL_DEFAULT_RESCUE_MAX_HELPERS;
    c.max_auto_expand_lanes    = 0;   /* 0 → nº de cores logicos */
    c.park_idle_threshold_ms   = POOL_DEFAULT_PARK_IDLE_MS;
    c.shutdown_drain_timeout_ms = POOL_DEFAULT_SHUTDOWN_DRAIN_MS;
    c.shutdown_join_timeout_ms  = POOL_DEFAULT_SHUTDOWN_JOIN_MS;
    c.shutdown_force_kill       = true;
    c.task_thresholds.long_threshold_ns = (uint64_t)XTASK_DEFAULT_LONG_NS;
    c.task_thresholds.blocked_ratio_max = XTASK_DEFAULT_BLOCKED_RATIO;
    c.task_thresholds.cpu_ratio_min     = XTASK_DEFAULT_CPU_RATIO;
    c.task_thresholds_set = false;
    return c;
}

/* ─────────────────────────────────────────────────────────────────────────
 * API publica: pool_create
 * ───────────────────────────────────────────────────────────────────────── */

static uint64_t ns_to_tsc(uint64_t ns)
{
    double cpns = xthread_cycles_per_ns();
    if (cpns <= 0.0) cpns = 2.4;
    return (uint64_t)((double)ns * cpns);
}

static bool pool_setup_lanes_and_workers(ShardedPool* pool, int ring_capacity)
{
    /* Pre-aloca TODAS as lanes ate lane_capacity (= max_shards). As extras ficam
     * prontas (ring inicializado) e sao ativadas sob demanda — sem realloc. */
    pool->lanes = (WSLane*)calloc(pool->lane_capacity, sizeof(WSLane));
    if (!pool->lanes) return false;

    for (int i = 0; i < pool->lane_capacity; i++) {
        if (!lane_init(&pool->lanes[i], ring_capacity)) return false;
        pool->lanes_initialized++;
    }

    /* Workers ativos: 1 por lane INICIAL (as demais lanes ganham owner ao ativar). */
    for (int i = 0; i < pool->initial_lanes; i++) {
        WSWorker* w = worker_create(pool);
        if (!w) return false;
        atomic_set_ptr(&w->lane, &pool->lanes[i]);
        atomic_set_ptr(&pool->lanes[i].worker, w);
    }

    /* Workers de reserva (mesma quantidade dos ativos). */
    int reserve_initial = atomic_get(&pool->reserve_target);
    for (int i = 0; i < reserve_initial; i++) {
        WSWorker* w = worker_create(pool);
        if (!w) return false;
        reserve_push(pool, w);
    }

    return true;
}

static bool pool_setup_reserve_ring(ShardedPool* pool, int reserve_capacity)
{
    pool->reserve_capacity = reserve_capacity;
    pool->reserve_ring_buf = malloc((size_t)reserve_capacity * sizeof(WSWorker*));
    if (!pool->reserve_ring_buf) return false;
    ring_queue_init(&pool->reserve_ring, reserve_capacity);
    if (!pool->reserve_ring.seqno) {
        free(pool->reserve_ring_buf);
        pool->reserve_ring_buf = NULL;
        return false;
    }
    return true;
}

static bool pool_start_all_workers(ShardedPool* pool)
{
    int total = atomic_get(&pool->all_worker_count);
    pool->expected_ready = 0;
    for (int i = 0; i < total; i++) {
        if (!worker_start(pool->all_workers[i])) {
            for (int j = i + 1; j < total; j++)
                atomic_set(&pool->all_workers[j]->state, WSTATE_STOPPED);
            return false;
        }
        pool->expected_ready++;
    }
    return true;
}

static bool pool_start_monitors(ShardedPool* pool)
{
    thread_wait_prepare(&pool->reserve_monitor_wait);
    thread_wait_prepare(&pool->long_task_monitor_wait);
    pool->reserve_monitor   = xpl_thread_start(reserve_monitor_fn,   pool);
    if (!pool->reserve_monitor) return false;
    pool->long_task_monitor = xpl_thread_start(long_task_monitor_fn, pool);
    if (!pool->long_task_monitor) return false;
    return true;
}

ShardedPool* pool_create(const PoolConfig* cfg)
{
    PoolConfig c = cfg ? *cfg : pool_default_config();
    if (c.shard_count   <= 0) c.shard_count   = xcpu_count();
    if (c.ring_capacity <= 0) c.ring_capacity = POOL_DEFAULT_RING_CAPACITY;
    if ((c.ring_capacity & (c.ring_capacity - 1)) != 0)
        return NULL;
    if (c.long_task_threshold_ns == 0) c.long_task_threshold_ns = POOL_DEFAULT_LONG_TASK_NS;

    thread_wait_init(false);
    xthread_activity_init();

    ShardedPool* pool = (ShardedPool*)calloc(1, sizeof(ShardedPool));
    if (!pool) return NULL;

    int max_shards = (c.max_shards > 0) ? c.max_shards : POOL_DEFAULT_MAX_SHARDS;
    if (max_shards < c.shard_count) max_shards = c.shard_count;
    pool->lane_capacity            = max_shards;
    pool->initial_lanes            = c.shard_count;

    /* Teto da expansao automatica: default = nº de cores logicos. Mantem-se
     * dentro de [initial_lanes, lane_capacity]. max_shards segue como teto duro. */
    int auto_cap = (c.max_auto_expand_lanes > 0) ? c.max_auto_expand_lanes : xcpu_count();
    if (auto_cap > max_shards)      auto_cap = max_shards;
    if (auto_cap < c.shard_count)   auto_cap = c.shard_count;
    pool->expand_cap               = auto_cap;
    atomic_set(&pool->active_lanes,    c.shard_count);
    atomic_set(&pool->max_active_ever, c.shard_count);

    pool->ring_capacity            = c.ring_capacity;
    pool->spin_iterations          = c.spin_iterations >= 0
                                   ? c.spin_iterations : POOL_DEFAULT_SPIN_ITERATIONS;
    pool->long_monitor_interval_ms = c.monitor_interval_ms > 0
                                   ? c.monitor_interval_ms : POOL_MONITOR_INTERVAL_MS;
    pool->use_task_thresholds      = c.task_thresholds_set ? 1 : 0;
    pool->task_thresholds          = c.task_thresholds;
    if (pool->use_task_thresholds &&
        pool->task_thresholds.long_threshold_ns == 0)
        pool->task_thresholds.long_threshold_ns = c.long_task_threshold_ns;
    uint64_t effective_long_ns = pool->use_task_thresholds
                               ? pool->task_thresholds.long_threshold_ns
                               : c.long_task_threshold_ns;
    pool->long_threshold_tsc       = ns_to_tsc(effective_long_ns);

    pool->rescue_backlog_threshold = (c.rescue_backlog_threshold > 0)
                                   ? c.rescue_backlog_threshold : POOL_DEFAULT_RESCUE_BACKLOG;
    pool->rescue_max_helpers       = (c.rescue_max_helpers_per_lane > 0)
                                   ? c.rescue_max_helpers_per_lane : xcpu_count();
    pool->park_threshold_tsc       = (c.park_idle_threshold_ms > 0)
                                   ? ns_to_tsc((uint64_t)c.park_idle_threshold_ms * 1000000ULL) : 0;
    pool->rescue_wait_unit_tsc     = ns_to_tsc(500000ULL);  /* 0.5ms por ponto de ranking */

    pool->shutdown_drain_timeout_ms = (c.shutdown_drain_timeout_ms > 0)
                                    ? c.shutdown_drain_timeout_ms : POOL_DEFAULT_SHUTDOWN_DRAIN_MS;
    pool->shutdown_join_timeout_ms  = (c.shutdown_join_timeout_ms > 0)
                                    ? c.shutdown_join_timeout_ms : POOL_DEFAULT_SHUTDOWN_JOIN_MS;
    pool->shutdown_force_kill       = c.shutdown_force_kill;

    if (c.spin_budget_us > 0) {
        pool->spin_budget_cycles = ns_to_tsc((uint64_t)c.spin_budget_us * 1000);
    }

    /* Capacidade inicial de all_workers: lanes + reserva. Reserva = mesma qtd. */
    int reserve_target = (c.reserve_size > 0) ? c.reserve_size : c.shard_count;
    atomic_set(&pool->reserve_target, reserve_target);

    /* Orcamento de workers: owners (lane_capacity) + reserva + ajudantes de
     * resgate (~nº cores) + folga. Inclui cores p/ nao esgotar sob resgate. */
    pool->all_worker_capacity = (pool->lane_capacity + reserve_target + xcpu_count()) * 4;
    pool->all_workers = (WSWorker**)calloc(pool->all_worker_capacity, sizeof(WSWorker*));
    if (!pool->all_workers) { free(pool); return NULL; }

    int reserve_ring_cap = pool->all_worker_capacity;
    /* arredonda para potencia de 2 */
    int rc = 1;
    while (rc < reserve_ring_cap) rc <<= 1;
    if (!pool_setup_reserve_ring(pool, rc)) {
        pool_shutdown(pool);
        return NULL;
    }

    if (!pool_setup_lanes_and_workers(pool, c.ring_capacity)) {
        pool_shutdown(pool);
        return NULL;
    }

    if (!pool_start_all_workers(pool)) {
        pool_shutdown(pool);
        return NULL;
    }
    pool_init(pool);
    if (!pool_start_monitors(pool)) {
        pool_shutdown(pool);
        return NULL;
    }

    return pool;
}

/* ─────────────────────────────────────────────────────────────────────────
 * API publica: pool_init
 * ───────────────────────────────────────────────────────────────────────── */

void pool_init(ShardedPool* pool)
{
    int waited_ms = 0;
    while (atomic_get(&pool->workers_ready) < pool->expected_ready)
    {
        xsleep_ms(1);
        if (++waited_ms % 2000 == 0)
            pool_log(POOL_LOG_INFO, "aguardando workers: %d/%d (%d ms)",
                     atomic_get(&pool->workers_ready), pool->expected_ready, waited_ms);
    }
    pool_log(POOL_LOG_INFO, "workers prontos: %d", pool->expected_ready);
}

/* ─────────────────────────────────────────────────────────────────────────
 * API publica: pool_submit — round-robin entre lanes + detecao no caminho
 * ───────────────────────────────────────────────────────────────────────── */

/* Faz handoff direto antes do submit, garantindo que a nova task seja
 * processada pela reserva imediatamente — nao espera o monitor periodico. */
static void submit_check_handoff(ShardedPool* pool, WSLane* lane)
{
    WSWorker* w = (WSWorker*)atomic_get_ptr(&lane->worker);
    if (!w) return;
    if (atomic_get(&w->detached)) return;

    uint64_t now   = xpl_rdtscp();
    uint64_t start = worker_long_task_start(w, now);
    if (start == 0) return;

    /* Tenta handoff sincrono. Se falhar (reserva vazia), acorda monitor pra
     * refazer reserva e dispara monitor de tasks longas como fallback. */
    if (!perform_handoff(pool, lane, w, start)) {
        thread_wait_wake(&pool->long_task_monitor_wait);
    }
}

/* Resgate sincrono: usa o ranking (profundidade + ocupacao + tempo preso).
 * O gate de ocupacao garante que so age quando o steal nao tem saida — nao
 * atropela o work-stealing. Custo baixo, sem criar thread. */
static void submit_check_rescue(ShardedPool* pool, WSLane* lane)
{
    if (lane_rescue_score(pool, lane) > 0)
        dispatch_rescue_worker(pool, lane);
}

/* Ativa a proxima lane pre-alocada, atribuindo um worker da reserva como owner.
 * So ativa COM worker (evita lane orfa). Retorna false se no teto (max_shards)
 * ou sem worker pronto (nesse caso acorda o monitor da reserva p/ criar). */
static bool activate_lane(ShardedPool* pool)
{
    int idx = atomic_get(&pool->active_lanes);
    if (idx >= pool->expand_cap) return false;             /* no teto de expansao automatica */

    WSWorker* w = reserve_pop(pool);
    if (!w) { reserve_monitor_wake(pool); return false; }  /* sem worker → adiado */

    int exp = idx;
    if (!atomic_cas(&pool->active_lanes, &exp, idx + 1)) {
        reserve_push(pool, w);   /* perdeu a corrida de ativacao; devolve worker */
        return false;
    }

    WSLane* lane = &pool->lanes[idx];

    /* Se a lane ainda tem um owner antigo (contracao recente que ainda nao
     * voltou a reserva), reusa-o: ele vera index < active_lanes e permanece.
     * Devolve o spare que pegamos. */
    if (atomic_get_ptr(&lane->worker) != NULL) {
        reserve_push(pool, w);
        thread_wait_wake(&lane->wait);
    } else {
        atomic_set(&w->rescue_mode, 0);
        atomic_set(&w->detached, 0);
        atomic_set_ptr(&w->lane, lane);
        atomic_set_ptr(&lane->worker, w);
        thread_wait_wake(&w->reserve_wait);
    }

    /* sobe o high-water p/ steal/monitor varrerem a lane nova */
    int hw = atomic_get(&pool->max_active_ever);
    while (hw < idx + 1) {
        int e = hw;
        if (atomic_cas(&pool->max_active_ever, &e, idx + 1)) break;
        hw = atomic_get(&pool->max_active_ever);
    }

    atomic_add(&pool->stat_expansions, 1);
    reserve_monitor_wake(pool);  /* repor reserva */

    int ncores = xcpu_count();
    if (idx + 1 > ncores) {
        int warned = 0;
        if (atomic_cas(&pool->warned_oversubscribe, &warned, 1))
            pool_log(POOL_LOG_WARN, "oversubscription: %d lanes ativas / %d cores logicos",
                     idx + 1, ncores);
    }
    return true;
}

#ifdef POOL_TEST_HOOKS
int pool_test_activate_next_lane(ShardedPool* pool)
{
    return activate_lane(pool) ? 1 : 0;
}

int pool_test_orphan_lane_count(ShardedPool* pool)
{
    int active = atomic_get(&pool->active_lanes);
    int orphans = 0;
    for (int i = 0; i < active; i++) {
        if (atomic_get_ptr(&pool->lanes[i].worker) == NULL) orphans++;
    }
    return orphans;
}

int pool_test_orphan_transition_observed(void)
{
    return InterlockedCompareExchange(&g_pool_test_orphan_transition, 0, 0) != 0;
}
#endif

/* Backoff progressivo do submit quando tudo esta cheio: fica LENTO, nao falha. */
static void submit_backoff(int* spins)
{
    int s = (*spins)++;
    if      (s < 64)  xcpu_pause();
    else if (s < 256) xpl_yield();
    else              xsleep_ms(1);
}

static bool submit_push_lane(ShardedPool* pool, WSLane* lane, Task* t)
{
    atomic_add(&pool->pending_tasks, 1);
    if (!xring_push_mp(&lane->ring, lane->ring_buf, t)) {
        atomic_sub(&pool->pending_tasks, 1);
        return false;
    }
    atomic_add(&pool->stat_submitted, 1);
    thread_wait_wake(&lane->wait);
    submit_check_handoff(pool, lane);
    submit_check_rescue(pool, lane);
    return true;
}

bool pool_submit(ShardedPool* pool, task_fn fn, void* arg)
{
    Task t = { fn, arg, xpl_rdtscp() };   /* enqueue_tsc p/ tempo de espera real (item 5) */
    int  spins = 0;

    for (;;) {
        if (atomic_get(&pool->shutdown)) {
            atomic_add(&pool->stat_failures, 1);  /* unica condicao de falha */
            return false;
        }

        int n = atomic_get(&pool->active_lanes);
        if (n < 1) n = 1;
        int     widx = (int)((unsigned int)atomic_add(&pool->submit_seq, 1) % (unsigned int)n);
        WSLane* lane = &pool->lanes[widx];

        if (submit_push_lane(pool, lane, &t)) return true;

        /* ring cheio → tenta expandir (nova lane) e repete o round-robin */
        if (activate_lane(pool)) continue;

        /* nao expandiu (no teto ou reserva vazia) → tenta qualquer lane com espaco */
        int active = atomic_get(&pool->active_lanes);
        bool placed = false;
        for (int i = 0; i < active; i++) {
            if (submit_push_lane(pool, &pool->lanes[i], &t)) { placed = true; break; }
        }
        if (placed) return true;

        /* tudo cheio e no teto → backpressure: espera (lento), nunca falha */
        atomic_add(&pool->stat_backpressure, 1);
        submit_backoff(&spins);
    }
}

/* ─────────────────────────────────────────────────────────────────────────
 * API publica: pool_shutdown
 * ───────────────────────────────────────────────────────────────────────── */

static bool worker_handle_dead(WSWorker* w)
{
    /* Worker CRIADO mas nunca INICIADO (falha de alocacao na construcao do pool
     * deixa WSWorker* com state=ACTIVE e handle=NULL): nao existe thread para
     * esperar nem terminar. Trata como ja parado — evita esperar o join timeout
     * e disparar force-kill fantasma (poison/leak) num caminho de erro limpo. */
    if (!w->handle) {
        atomic_set(&w->state, WSTATE_STOPPED);
        return true;
    }
#ifdef XPLATBASE_WIN
    if (WaitForSingleObject(w->handle, 0) == WAIT_OBJECT_0) {
        atomic_set(&w->state, WSTATE_STOPPED);
        return true;
    }
#endif
    return false;
}

/* Terminacao FORCADA de um worker preso dentro de t->fn (nivel mais baixo).
 * PERIGOSO: nao roda cleanup, pode deixar lock travado / vazar / corromper heap.
 * Por isso a struct NAO e liberada (leak controlado) e o chamador para de reciclar. */
static void force_kill_worker(WSWorker* w)
{
#ifdef XPLATBASE_WIN
    if (w->handle) { TerminateThread(w->handle, 1); CloseHandle(w->handle); w->handle = NULL; }
#else
    pthread_cancel(w->handle);   /* limitado: so age em cancellation point */
#endif
    w->force_killed = 1;
    atomic_set(&w->state, WSTATE_STOPPED);
}

/* Espera bounded: retorna true se todos pararam dentro de timeout_ms. */
static bool pool_wait_workers_stopped_timeout(ShardedPool* pool, int timeout_ms)
{
    /* Timeout por tempo REAL (TSC) — robusto se a thread for starved por carga
     * de CPU (contar iteracoes de sleep dava timeout incoerente sob pressao). */
    uint64_t budget = ns_to_tsc((uint64_t)timeout_ms * 1000000ULL);
    uint64_t t0     = xpl_tsc();
    for (;;) {
        int pending = 0;
        int total = atomic_get(&pool->all_worker_count);
        for (int i = 0; i < total; i++) {
            WSWorker* w = pool->all_workers[i];
            if (!w) continue;
            if (atomic_get(&w->state) == WSTATE_STOPPED) continue;
            if (worker_handle_dead(w)) continue;
            pending++;
            thread_wait_wake(&w->reserve_wait);
        }
        if (pool->lanes) {
            for (int i = 0; i < pool->lanes_initialized; i++)
                thread_wait_wake(&pool->lanes[i].wait);
        }
        if (!pending)                 return true;
        if (xpl_tsc() - t0 >= budget) return false;
        xsleep_ms(1);
    }
}

/* Mata as threads que nao pararam pela flag. Retorna quantas. */
static int force_kill_stragglers(ShardedPool* pool)
{
    int killed = 0;
    int total = atomic_get(&pool->all_worker_count);
    for (int i = 0; i < total; i++) {
        WSWorker* w = pool->all_workers[i];
        if (!w) continue;
        if (atomic_get(&w->state) == WSTATE_STOPPED) continue;
        if (worker_handle_dead(w)) continue;
        force_kill_worker(w);
        killed++;
    }
    return killed;
}

static void pool_close_submissions_and_monitors(ShardedPool* pool)
{
    atomic_set(&pool->shutdown, 1);
    thread_wait_wake(&pool->reserve_monitor_wait);
    thread_wait_wake(&pool->long_task_monitor_wait);
}

static void pool_stop_all_workers(ShardedPool* pool)
{
    atomic_set(&pool->stop_workers, 1);
    int total = atomic_get(&pool->all_worker_count);
    for (int i = 0; i < total; i++) {
        WSWorker* w = pool->all_workers[i];
        if (w && atomic_get(&w->state) != WSTATE_STOPPED)
            atomic_set(&w->state, WSTATE_STOPPING);
    }
}

static void pool_wake_all_workers(ShardedPool* pool)
{
    /* acorda quem esta na reserva e quem esta em alguma lane */
    int total = atomic_get(&pool->all_worker_count);
    for (int i = 0; i < total; i++) {
        WSWorker* w = pool->all_workers[i];
        if (!w) continue;
        thread_wait_wake(&w->reserve_wait);
    }
    if (!pool->lanes) return;
    for (int i = 0; i < pool->lanes_initialized; i++) {
        thread_wait_wake(&pool->lanes[i].wait);
    }
}

static void pool_wait_workers_stopped(ShardedPool* pool)
{
    int pending;
    int waited_ms = 0;
    do {
        pending = 0;
        int total = atomic_get(&pool->all_worker_count);
        for (int i = 0; i < total; i++) {
            WSWorker* w = pool->all_workers[i];
            if (!w) continue;
            if (atomic_get(&w->state) == WSTATE_STOPPED) continue;
            if (worker_handle_dead(w)) continue;
            pending++;
            thread_wait_wake(&w->reserve_wait);
        }
        if (pool->lanes) {
            for (int i = 0; i < pool->lanes_initialized; i++)
                thread_wait_wake(&pool->lanes[i].wait);
        }
        if (pending) {
            xsleep_ms(1);
            if (++waited_ms % 2000 == 0)
                pool_log(POOL_LOG_WARN, "shutdown: %d worker(s) ainda nao pararam (%d ms)",
                         pending, waited_ms);
        }
    } while (pending);
}

static void pool_join_and_free_workers(ShardedPool* pool)
{
    int total = atomic_get(&pool->all_worker_count);
    for (int i = 0; i < total; i++) {
        WSWorker* w = pool->all_workers[i];
        if (!w) continue;
        if (w->force_killed) continue;   /* leak controlado: nao mexe em thread morta a forca */
        if (w->handle) xpl_thread_join(w->handle);
        thread_wait_destroy(&w->reserve_wait);
        free(w);
    }
    free(pool->all_workers);
    pool->all_workers = NULL;
}

static void pool_destroy_lanes(ShardedPool* pool)
{
    if (!pool->lanes) return;
    for (int i = 0; i < pool->lanes_initialized; i++) lane_destroy(&pool->lanes[i]);
    free(pool->lanes);
    pool->lanes = NULL;
}

void pool_shutdown(ShardedPool* pool)
{
    if (!pool) return;

    pool_close_submissions_and_monitors(pool);

    /* Monitores primeiro — eles podem criar workers; precisa garantir que
     * pararam antes de iterar pool->all_workers para join. */
    if (pool->reserve_monitor)   xpl_thread_join(pool->reserve_monitor);
    if (pool->long_task_monitor) xpl_thread_join(pool->long_task_monitor);

    pool_wake_all_workers(pool);

    /* ── Fase A: drain gracioso com timeout WALL-CLOCK (TSC) — nunca trava nem
     * estoura o tempo real mesmo sob starvation de CPU. ── */
    uint64_t drain_budget = ns_to_tsc((uint64_t)pool->shutdown_drain_timeout_ms * 1000000ULL);
    uint64_t drain_t0     = xpl_tsc();
    while (atomic_get(&pool->pending_tasks) > 0 && (xpl_tsc() - drain_t0) < drain_budget) {
        pool_wake_all_workers(pool);
        xsleep_ms(1);
    }
    int left = atomic_get(&pool->pending_tasks);
    if (left > 0)
        pool_log(POOL_LOG_WARN, "shutdown: drain timeout (%d ms), %d task(s) restantes — forcando parada",
                 pool->shutdown_drain_timeout_ms, left);

    /* ── Fase B: parada cooperativa pela flag, com timeout ── */
    pool_stop_all_workers(pool);
    pool_wake_all_workers(pool);
    bool all_stopped = pool_wait_workers_stopped_timeout(pool, pool->shutdown_join_timeout_ms);

    /* ── Fase C: terminacao forcada dos que nao pararam (task travada) ── */
    bool poisoned = false;
    if (!all_stopped) {
        if (pool->shutdown_force_kill) {
            int killed = force_kill_stragglers(pool);
            if (killed > 0) {
                atomic_add(&g_force_killed_total, killed);
                poisoned = true;
                pool_log(POOL_LOG_CRIT,
                    "shutdown: %d worker(s) TERMINADOS A FORCA (task travada). Pool ENVENENADO: "
                    "recursos vazados, heap possivelmente travado — PARE DE RECICLAR pools.", killed);
            }
        } else {
            /* force-kill desabilitado: ultimo recurso e esperar (pode travar). */
            pool_wait_workers_stopped(pool);
        }
    }

    /* Pool envenenado: qualquer free() pode deadlockar no lock de heap que a
     * thread morta possa ter retido. Leak controlado TOTAL e retorno imediato. */
    if (poisoned) return;

    thread_wait_destroy(&pool->reserve_monitor_wait);
    thread_wait_destroy(&pool->long_task_monitor_wait);

    pool_join_and_free_workers(pool);
    pool_destroy_lanes(pool);

    if (pool->reserve_ring_buf) {
        ring_queue_destroy(&pool->reserve_ring);
        free(pool->reserve_ring_buf);
    }

    free(pool);
}

/* ─────────────────────────────────────────────────────────────────────────
 * API publica: pool_stats
 * ───────────────────────────────────────────────────────────────────────── */

void pool_stats(ShardedPool* pool, PoolStats* out)
{
    memset(out, 0, sizeof(*out));
    out->shard_count     = atomic_get(&pool->active_lanes);
    out->total_submitted = (uint64_t)(unsigned int)atomic_get(&pool->stat_submitted);
    out->submit_failures = (uint64_t)(unsigned int)atomic_get(&pool->stat_failures);
    out->total_handoffs  = (uint64_t)(unsigned int)atomic_get(&pool->stat_handoffs);
    out->total_stolen    = (uint64_t)(unsigned int)atomic_get(&pool->stat_stolen);
    out->total_rescued   = (uint64_t)(unsigned int)atomic_get(&pool->stat_rescued);
    out->submit_backpressure = (uint64_t)(unsigned int)atomic_get(&pool->stat_backpressure);
    out->total_expansions    = (uint64_t)(unsigned int)atomic_get(&pool->stat_expansions);
    out->reserve_count   = atomic_get(&pool->reserve_count);

    int active   = 0;
    int detached = 0;
    int pending  = 0;
    int total    = atomic_get(&pool->all_worker_count);
    for (int i = 0; i < total; i++) {
        WSWorker* w = pool->all_workers[i];
        if (!w) continue;
        if (atomic_get(&w->state) == WSTATE_ACTIVE && atomic_get_ptr(&w->lane) != NULL) active++;
        if (atomic_get(&w->detached)) detached++;
    }
    int nlanes = atomic_get(&pool->max_active_ever);
    for (int i = 0; i < nlanes; i++) {
        pending += ring_queue_count(&pool->lanes[i].ring);
    }
    out->active_worker_count = active;
    out->detached_count      = detached;
    out->pending_tasks       = pending;
}
