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
#include <limits.h>

#ifdef POOL_TEST_HOOKS
static volatile LONG g_pool_test_alloc_after = -1;
static volatile LONG g_pool_test_thread_start_after = -1;
static volatile LONG g_pool_test_handoff_target = 0;
static volatile LONG g_pool_test_handoff_arrivals = 0;
static volatile LONG g_pool_test_lane_barrier_enabled = 0;
static volatile LONG g_pool_test_max_spin_iterations = 0;
static volatile LONG g_pool_test_orphan_transition = 0;
static volatile LONG g_pool_test_reserve_return_after = -1;
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

void pool_test_fail_reserve_return_after(int successful_returns)
{
    InterlockedExchange(&g_pool_test_reserve_return_after, successful_returns);
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

/* Barreiras da "janela de DONO-DUPLO" (BUG 01). Duas paradas coordenadas para
 * forcar de forma deterministica o interleaving da corrida:
 *   - dbo_owner   : para o owner que sai DEPOIS do CAS worker->NULL e ANTES do
 *                   re-claim, com a lane ja sem dono.
 *   - dbo_activate: para o activate_lane ANTES do install do novo owner.
 * O teste solta dbo_owner primeiro (owner re-reivindica a lane) e so depois
 * dbo_activate (activate tenta instalar). Com a escrita simples antiga, o
 * install sobrescreve e cria 2 donos; com o CAS, o install falha e reusa. */
typedef struct {
    volatile LONG enabled;
    HANDLE        arrived;
    HANDLE        release;
} pool_test_barrier_t;

static pool_test_barrier_t g_dbo_owner;
static pool_test_barrier_t g_dbo_activate;

static void pool_test_barrier_enable(pool_test_barrier_t* b)
{
    if (!b->arrived) b->arrived = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!b->release) b->release = CreateEventW(NULL, TRUE, FALSE, NULL);
    ResetEvent(b->arrived);
    ResetEvent(b->release);
    InterlockedExchange(&b->enabled, 1);
}

static int pool_test_barrier_wait_arrived(pool_test_barrier_t* b, int timeout_ms)
{
    if (!b->arrived) return 0;
    return WaitForSingleObject(b->arrived, (DWORD)timeout_ms) == WAIT_OBJECT_0;
}

static void pool_test_barrier_release(pool_test_barrier_t* b)
{
    InterlockedExchange(&b->enabled, 0);
    if (b->release) SetEvent(b->release);
}

static void pool_test_barrier_hit(pool_test_barrier_t* b)
{
    if (!InterlockedCompareExchange(&b->enabled, 0, 0)) return;
    SetEvent(b->arrived);
    WaitForSingleObject(b->release, 5000);
}

void pool_test_enable_double_owner_window(void)
{
    pool_test_barrier_enable(&g_dbo_owner);
    pool_test_barrier_enable(&g_dbo_activate);
}
int  pool_test_wait_double_owner_owner(int timeout_ms)    { return pool_test_barrier_wait_arrived(&g_dbo_owner, timeout_ms); }
int  pool_test_wait_double_owner_activate(int timeout_ms) { return pool_test_barrier_wait_arrived(&g_dbo_activate, timeout_ms); }
void pool_test_release_double_owner_owner(void)           { pool_test_barrier_release(&g_dbo_owner); }
void pool_test_release_double_owner_activate(void)        { pool_test_barrier_release(&g_dbo_activate); }

static void pool_test_double_owner_owner_wait(void)    { pool_test_barrier_hit(&g_dbo_owner); }
static void pool_test_activate_install_wait(void)      { pool_test_barrier_hit(&g_dbo_activate); }

/* Congela expand/contract proativos do monitor: usado por testes para impedir
 * que a contracao "auto-cure" um estado (ex.: dono-duplo) antes da verificacao. */
static volatile LONG g_pool_test_freeze_autoscale = 0;
void pool_test_freeze_autoscale(int on)
{
    InterlockedExchange(&g_pool_test_freeze_autoscale, on ? 1 : 0);
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
static xatomic_int g_pool_log_lock;

static void pool_log_lock(void)
{
    for (;;) {
        int expected = 0;
        if (atomic_cas(&g_pool_log_lock, &expected, 1)) return;
        xcpu_pause();
    }
}

static void pool_log_unlock(void)
{
    atomic_set(&g_pool_log_lock, 0);
}

void pool_set_log_hook(pool_log_fn fn)
{
    pool_log_lock();
    g_pool_log_hook = fn;
    pool_log_unlock();
}

static pool_log_fn pool_log_hook_snapshot(void)
{
    pool_log_fn fn;
    pool_log_lock();
    fn = g_pool_log_hook;
    pool_log_unlock();
    return fn;
}

#ifdef XPLATBASE_WIN
    #include <intrin.h>
    typedef HANDLE           xpl_thread_t;
    typedef DWORD WINAPI     xpl_fn_sig(void*);
    static bool xpl_thread_start(xpl_thread_t* out, xpl_fn_sig* fn, void* arg) {
#ifdef POOL_TEST_HOOKS
        if (pool_test_countdown_should_fail(&g_pool_test_thread_start_after)) return false;
#endif
        *out = CreateThread(NULL, 0, fn, arg, 0, NULL);
        return *out != NULL;
    }
    static void xpl_thread_join(xpl_thread_t h) {
        WaitForSingleObject(h, INFINITE); CloseHandle(h);
    }
    static void xpl_thread_detach(xpl_thread_t h) { CloseHandle(h); }
    static bool xpl_thread_force_stop(xpl_thread_t h) {
        if (!h || !TerminateThread(h, 1)) return false;
        WaitForSingleObject(h, INFINITE);
        CloseHandle(h);
        return true;
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
    static bool xpl_thread_start(xpl_thread_t* out, xpl_fn_sig* fn, void* arg) {
        memset(out, 0, sizeof(*out));
        return pthread_create(out, NULL, fn, arg) == 0;
    }
    static void xpl_thread_join(xpl_thread_t h) { pthread_join(h, NULL); }
    static void xpl_thread_detach(xpl_thread_t h) { pthread_detach(h); }
    static bool xpl_thread_force_stop(xpl_thread_t h) {
        if (pthread_cancel(h) != 0) return false;
        return pthread_join(h, NULL) == 0;
    }
    static void xpl_yield(void)  { sched_yield(); }
    static void xpl_sleep0(void) { struct timespec z={0,0}; nanosleep(&z,NULL); }
    static uint64_t xpl_tsc(void) {
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
        return (uint64_t)ts.tv_sec*1000000000ULL+(uint64_t)ts.tv_nsec;
    }
    static uint64_t xpl_rdtscp(void) {
        return xpl_tsc();
    }
    #define XPL_FN   void*
    #define XPL_RET  return NULL
#endif

#ifdef XPLATBASE_WIN
static __declspec(thread) ShardedPool* g_current_worker_pool;
#else
static __thread ShardedPool* g_current_worker_pool;
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

/* WSLane alinhada/padded a cache-line: como sizeof(WSLane) vira multiplo de
 * XPL_CACHELINE, lanes adjacentes no array ficam a >= 1 linha de distancia e
 * nao sofrem false sharing nos campos quentes (worker/oldest) nem no ring. */
typedef struct XPL_ALIGN(XPL_CACHELINE) WSLane {
    RingQueue    ring;
    void*        ring_buf;
    xwait_t      wait;             /* dormir/acordar worker atribuido a essa lane */
    xatomic_ptr  worker;           /* WSWorker* owner da lane (apenas referencia) */
    xatomic_int  rescue_helpers;   /* nº de ajudantes de resgate atuando agora */
    xatomic_uint64 oldest_enqueue_tsc;
} WSLane;

struct WSWorker {
    ShardedPool*    pool;
    xpl_thread_t    handle;
    int             thread_started;
    int             id;                /* ID estavel pra debug */

    xatomic_int     state;             /* WSTATE_ACTIVE/STOPPING/STOPPED */
    xatomic_int     detached;          /* 1 = sair da lane apos task atual */
    xatomic_int     detached_counted;
    xatomic_int     rescue_mode;       /* 1 = ajudante transitorio; volta a reserva ao ociar */
    xatomic_ptr     lane;              /* WSLane* atribuida; NULL = na reserva */
    xatomic_int64   task_start_tsc;    /* TSC quando comecou t.fn; 0 = ocioso */
    xatomic_int64   task_start_wall_ns;
    xatomic_int64   task_start_activity;

    xwait_t         reserve_wait;      /* wait quando esta na reserva */
    int             force_killed;      /* 1 = terminado a forca no shutdown (leak controlado) */
    unsigned        sample_tick;       /* throttle de sample_neighbors (local ao thread) */
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
    int             reserve_monitor_started;
    xatomic_int     reserve_monitor_running;

    xwait_t         long_task_monitor_wait;
    xpl_thread_t    long_task_monitor;
    int             long_task_monitor_started;
    xatomic_int     long_task_monitor_running;

    uint64_t        long_threshold_tsc;
    int             long_monitor_interval_ms;
    xtask_thresholds_t task_thresholds;
    int             use_task_thresholds;

    xatomic_int     shutdown;
    xatomic_int     shutdown_started;
    xatomic_int     shutdown_complete;
    xatomic_int     shutdown_poisoned;
    xatomic_int     api_users;
    xatomic_int     stop_workers;
    xatomic_int     pending_tasks;
    xatomic_uint32  submit_seq;
    xatomic_int     workers_ready;
    int             expected_ready;

    xatomic_uint64  stat_submitted;
    xatomic_uint64  stat_failures;
    xatomic_uint64  stat_stolen;
    xatomic_uint64  stat_handoffs;
    xatomic_uint64  stat_rescued;
    xatomic_uint64  stat_backpressure;   /* nº de vezes que submit teve que esperar */
    xatomic_uint64  stat_expansions;     /* lanes ativadas dinamicamente */

    uint64_t        spin_budget_cycles;
    int             spin_iterations;
    int             ring_capacity;

    int             rescue_backlog_threshold;
    int             rescue_max_helpers;     /* teto absoluto de ajudantes por lane */
    uint64_t        park_threshold_tsc;     /* 0 = parking desativado */
    xatomic_int     warned_oversubscribe;   /* aviso de cores: emitido 1x */
    xatomic_int     warned_capacity;        /* aviso de capacidade esgotada: emitido 1x */

    xatomic_int     busy_workers;           /* workers executando task agora (ocupacao) */
    xatomic_int     detached_workers;
    xatomic_int     rescue_slots_reserved;
    uint64_t        rescue_wait_unit_tsc;   /* unidade p/ ranking de tempo de espera */

    xatomic_int     spinning_workers;       /* workers ociosos spinando agora (cap estilo Go) */
    int             spinner_cap;            /* teto de spinners simultaneos (folga de cores) */

    int             low_load_scans;         /* histerese de contracao (so o monitor toca) */
    int             high_load_scans;        /* histerese de expansao proativa (so o monitor toca) */

    int             shutdown_drain_timeout_ms;
    int             shutdown_join_timeout_ms;
    bool            shutdown_force_kill;
    bool            wait_runtime_acquired;
    pool_log_fn     log_hook;
};

static void pool_log(ShardedPool* pool, int sev, const char* fmt, ...)
{
    pool_log_fn hook = pool ? pool->log_hook : NULL;
#ifndef POOL_VERBOSE
    if (!hook) return;
#endif
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (hook) hook(sev, buf);
#ifdef POOL_VERBOSE
    fprintf(stderr, "%s\n", buf);
#endif
    (void)sev;
}

/* Total de threads terminadas a forca no processo (todos os pools). */
static xatomic_int g_force_killed_total;
int pool_force_kill_count(void) { return atomic_get(&g_force_killed_total); }

/* ─────────────────────────────────────────────────────────────────────────
 * Forward decls
 * ───────────────────────────────────────────────────────────────────────── */

static XPL_FN worker_fn(void* raw);
static XPL_FN reserve_monitor_fn(void* raw);
static XPL_FN long_task_monitor_fn(void* raw);
static XPL_FN shutdown_coordinator_fn(void* raw);

static WSWorker* worker_create(ShardedPool* pool);
static bool      worker_start (WSWorker* w);
static bool      activate_lane(ShardedPool* pool);

static bool pool_api_enter(ShardedPool* pool)
{
    if (!pool) return false;
    atomic_add(&pool->api_users, 1);
    if (atomic_get(&pool->shutdown)) {
        atomic_sub(&pool->api_users, 1);
        return false;
    }
    return true;
}

static void pool_api_leave(ShardedPool* pool)
{
    atomic_sub(&pool->api_users, 1);
}

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

static void worker_release_detached_count(WSWorker* w)
{
    int spins = 0;
    for (;;) {
        int counted = atomic_get(&w->detached_counted);
        if (counted == 0) return;
        if (counted == 1) {
            /* Estado transitorio do perform_handoff (entre =1 e =2). Pausa curta,
             * mas apos um teto cede a CPU: sob oversubscription o thread que faz o
             * handoff precisa rodar para sair do estado 1 — busy-spin puro poderia
             * livelock. xpl_yield garante progresso. */
            if (++spins < 64) {
                xcpu_pause();
            } else {
                xpl_yield();
                spins = 0;
            }
            continue;
        }
        int expected = 2;
        if (atomic_cas(&w->detached_counted, &expected, 0)) {
            atomic_sub(&w->pool->detached_workers, 1);
            return;
        }
    }
}

/* ─────────────────────────────────────────────────────────────────────────
 * Acesso a tasks: ring proprio + steal entre lanes
 * ───────────────────────────────────────────────────────────────────────── */

static bool lane_pop_task(WSLane* lane, Task* out)
{
    if (!xring_pop_mc(&lane->ring, lane->ring_buf, out))
        return false;
    if (ring_queue_count(&lane->ring) == 0) {
        atomic_u64_set(&lane->oldest_enqueue_tsc, 0);
        if (ring_queue_count(&lane->ring) > 0)
            atomic_u64_set(&lane->oldest_enqueue_tsc, xpl_rdtscp());
    }
    return true;
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
            atomic_u64_add(&pool->stat_stolen, 1);
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
        /* Spin adaptativo: se uma iteracao demorou muito mais que o esperado
         * (gap de TSC > budget/8), este worker foi PREEMPTADO — sinal de
         * oversubscription. Em vez de continuar spinando (e disputar o core que
         * quem tem trabalho/produtores precisa), abandona o spin e vai parquear,
         * liberando o core. Sob baixa carga o gap e minusculo -> spin normal,
         * preservando o p50 de microssegundos. (era v16 nos experimentos) */
        uint64_t deadline = xpl_tsc() + budget_cycles;
        uint64_t gthr = budget_cycles / 8; if (gthr < 1) gthr = 1;
        uint64_t prev = xpl_tsc();
        int checks = 0;
        for (;;) {
            if (worker_try_any(w, lane, out)) return true;
            if (!spin_check_continue(w))      return false;
            uint64_t now = xpl_tsc();
            if (now - prev > gthr) return false;   /* preemptado -> sai do spin, parqueia */
            xcpu_pause();
            prev = now;
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

/* A cada quantas entradas em P3 amostramos vizinhos. A deteccao de task longa
 * tem o monitor periodico como backstop, entao amostrar 1 em N reduz o custo
 * (rdtscp + varredura de lanes) sem perder cobertura relevante. */
#define SPIN_NEIGHBOR_SAMPLE_MASK  7u

static bool spin_phase3(WSWorker* w, WSLane* lane, Task* out)
{
    if ((w->sample_tick++ & SPIN_NEIGHBOR_SAMPLE_MASK) == 0)
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

/* Cap de spinners (inspirado no scheduler do Go: contador global de threads
 * spinando). Limita quantos workers OCIOSOS fazem spin progressivo ao mesmo
 * tempo, deixando cores livres para quem tem trabalho / para os produtores —
 * o que reduz a oversubscription que gera a cauda de latencia (quantum do SO).
 * Decisao discreta (spina ou nao) e GLOBAL ao pool. Workers que nao pegam vaga
 * dormem curto (cedem o core); a task ainda chega via wake direto da lane. */
static bool spinner_try_enter(ShardedPool* pool)
{
    for (;;) {
        int cur = atomic_get(&pool->spinning_workers);
        if (cur >= pool->spinner_cap) return false;
        int expected = cur;
        if (atomic_cas(&pool->spinning_workers, &expected, cur + 1)) return true;
        /* CAS perdeu a corrida → re-tenta com o valor atual */
    }
}

static void spinner_leave(ShardedPool* pool)
{
    atomic_sub(&pool->spinning_workers, 1);
}

static bool worker_idle_on_lane(WSWorker* w, WSLane* lane, Task* out, bool parked)
{
    ShardedPool* pool = w->pool;
    thread_wait_prepare(&lane->wait);

    if (worker_try_any(w, lane, out)) return true;

    /* Parqueado: pula o spin (economiza CPU) e vai direto ao sono profundo. */
    if (parked) {
        if (!spin_check_continue(w)) return false;
        thread_wait_sleep_for(&lane->wait, POOL_PARK_SLEEP_US);
        return worker_try_any(w, lane, out);
    }

    /* Sem vaga de spinner: nao satura o core — dorme curto e re-checa.
     * (A task que cair nesta lane acorda este worker via wake direto do submit.) */
    if (!spinner_try_enter(pool)) {
        if (!spin_check_continue(w)) return false;
        thread_wait_sleep_for(&lane->wait, 1000);
        return worker_try_any(w, lane, out);
    }

    /* Com vaga: spin progressivo (libera a vaga ao sair, achando task ou nao). */
    bool got = false;
    if      (spin_phase1(w, lane, out, pool->spin_budget_cycles))       got = true;
    else if (spin_check_continue(w) && spin_phase2(w, lane, out))       got = true;
    else if (spin_check_continue(w) && spin_phase3(w, lane, out))       got = true;
    spinner_leave(pool);

    if (got) return true;
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

static bool worker_return_to_reserve(WSWorker* w)
{
    ShardedPool* pool = w->pool;
    atomic_set(&w->detached, 0);
    worker_release_detached_count(w);
    worker_leave_lane(w);
#ifdef POOL_TEST_HOOKS
    if (pool_test_countdown_should_fail(&g_pool_test_reserve_return_after)) {
        atomic_set(&w->state, WSTATE_STOPPING);
        return false;
    }
#endif
    if (!reserve_push(pool, w)) {
        atomic_set(&w->state, WSTATE_STOPPING);
        return false;
    }
    reserve_monitor_wake(pool);
    return true;
}

static void reserve_push_or_stop(ShardedPool* pool, WSWorker* w)
{
    if (reserve_push(pool, w)) return;
    atomic_set(&w->state, WSTATE_STOPPING);
    thread_wait_wake(&w->reserve_wait);
}

static XPL_FN worker_fn(void* raw)
{
    WSWorker*    w    = (WSWorker*)raw;
    ShardedPool* pool = w->pool;

#ifndef XPLATBASE_WIN
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
#endif
    g_current_worker_pool = pool;
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
            atomic_sub(&pool->rescue_slots_reserved, 1);
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
#ifdef POOL_TEST_HOOKS
                    /* Janela de dono-duplo: lane ja sem dono, antes do re-claim. */
                    pool_test_double_owner_owner_wait();
#endif

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

    g_current_worker_pool = NULL;
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
    atomic_set(&victim->detached_counted, 1);
    if (!atomic_cas(&victim->detached, &not_detached, 1)) {
        atomic_set(&victim->detached_counted, 0);
        reserve_push_or_stop(pool, spare);
        return false;
    }
    atomic_add(&pool->detached_workers, 1);
    atomic_set(&victim->detached_counted, 2);

    atomic_set(&spare->rescue_mode, 0);
    atomic_set(&spare->detached, 0);
    atomic_set_ptr(&spare->lane, lane);

    void* expected_owner = victim;
    if (!atomic_cas_ptr(&lane->worker, &expected_owner, spare)) {
        atomic_set_ptr(&spare->lane, NULL);
        atomic_set(&victim->detached, 0);
        worker_release_detached_count(victim);
        reserve_push_or_stop(pool, spare);
        return false;
    }

    /* Acorda o spare (estava dormindo na reserva). */
    thread_wait_wake(&spare->reserve_wait);
    /* Acorda alguem na lane.wait pra que o novo worker comece imediato. */
    thread_wait_wake(&lane->wait);

    atomic_u64_add(&pool->stat_handoffs, 1);
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
        int owner_budget = atomic_get(&pool->active_lanes) +
                           atomic_get(&pool->detached_workers);
        int currently_busy = atomic_get(&pool->busy_workers);
        if (currently_busy > owner_budget)
            owner_budget = currently_busy;
        int reserved = atomic_get(&pool->rescue_slots_reserved);
        if (owner_budget + reserved >= xcpu_count()) break;

        int slot_expected = reserved;
        if (!atomic_cas(&pool->rescue_slots_reserved,
                        &slot_expected, reserved + 1))
            continue;

        int expected = cur;
        if (!atomic_cas(&lane->rescue_helpers, &expected, cur + 1)) {
            atomic_sub(&pool->rescue_slots_reserved, 1);
            continue;
        }

        WSWorker* spare = reserve_pop(pool);
        if (!spare) {
            atomic_sub(&lane->rescue_helpers, 1);  /* reverte o slot reservado */
            atomic_sub(&pool->rescue_slots_reserved, 1);
            reserve_monitor_wake(pool);            /* refazer reserva; backstop tenta de novo */
            break;
        }

        atomic_set(&spare->rescue_mode, 1);
        atomic_set_ptr(&spare->lane, lane);
        thread_wait_wake(&spare->reserve_wait);    /* tira da reserva */
        thread_wait_wake(&lane->wait);
        atomic_u64_add(&pool->stat_rescued, 1);
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

    uint64_t oldest = atomic_u64_get(&lane->oldest_enqueue_tsc);
    if (pool->rescue_wait_unit_tsc && oldest) {
        uint64_t now = xpl_rdtscp();
        if (now > oldest)
            wait_score = (now - oldest) / pool->rescue_wait_unit_tsc;
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

/* Quantos scans consecutivos de carga alta antes de expandir proativamente.
 * Pequeno (≈30ms a 10ms/scan) — so amortece micro-oscilacao; rajadas reais
 * sao cobertas imediatamente pelo caminho do submit (ring cheio → activate). */
#define LANE_EXPAND_HYSTERESIS  3

/* Expansao proativa: sob carga sustentada (todos ocupados + backlog), ativa
 * 1 lane apos a histerese, conservador. O caminho do submit ja cobre o "ring
 * cheio" de imediato (sem histerese). */
static void scan_maybe_expand(ShardedPool* pool)
{
#ifdef POOL_TEST_HOOKS
    if (InterlockedCompareExchange(&g_pool_test_freeze_autoscale, 0, 0)) return;
#endif
    int active = atomic_get(&pool->active_lanes);
    if (active >= pool->expand_cap) { pool->high_load_scans = 0; return; }
    if (atomic_get(&pool->busy_workers) < active) { pool->high_load_scans = 0; return; }  /* folga */

    int pend = 0;
    for (int i = 0; i < active; i++) pend += ring_queue_count(&pool->lanes[i].ring);
    if (pend < active) { pool->high_load_scans = 0; return; }  /* backlog insuficiente */

    if (++pool->high_load_scans < LANE_EXPAND_HYSTERESIS) return;
    pool->high_load_scans = 0;
    activate_lane(pool);                                   /* mais backlog que lanes, sustentado */
}

/* Contracao segura: sob carga baixa sustentada, desativa a lane do topo se ela
 * estiver vazia e acima do nº inicial. O owner volta a reserva ao ociar (ve o
 * index >= active_lanes); residuais de corrida sao drenados via steal
 * (que varre max_active_ever). */
static void scan_maybe_contract(ShardedPool* pool)
{
#ifdef POOL_TEST_HOOKS
    if (InterlockedCompareExchange(&g_pool_test_freeze_autoscale, 0, 0)) return;
#endif
    int active = atomic_get(&pool->active_lanes);
    int top_count = active > 0
                  ? ring_queue_count(&pool->lanes[active - 1].ring)
                  : 0;

    bool low = active > pool->initial_lanes &&
               atomic_get(&pool->busy_workers) < active &&
               top_count == 0;

    if (!low) { pool->low_load_scans = 0; return; }

    /* Histerese p/ ENTRAR em contracao; depois contrai 1 lane por scan enquanto
     * a carga seguir baixa (nao zera o contador). */
    if (++pool->low_load_scans < LANE_CONTRACT_HYSTERESIS) return;

    int exp = active;
    if (!atomic_cas(&pool->active_lanes, &exp, active - 1))
        return;

    WSLane* lane = &pool->lanes[active - 1];
    if (ring_queue_count(&lane->ring) != 0) {
        int contracted = active - 1;
        atomic_cas(&pool->active_lanes, &contracted, active);
        thread_wait_wake(&lane->wait);
    }
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
            pool_log(pool, POOL_LOG_CRIT, "all_worker_capacity (%d) esgotado",
                     pool->all_worker_capacity);
        return NULL;
    }

    /* Aviso (1x) de oversubscription: total de workers passou dos cores logicos. */
    int ncores = xcpu_count();
    if (id + 1 > ncores) {
        int warned = 0;
        if (atomic_cas(&pool->warned_oversubscribe, &warned, 1)) {
            pool_log(pool, POOL_LOG_WARN, "oversubscription: %d workers / %d cores logicos",
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
    if (!xpl_thread_start(&w->handle, worker_fn, w)) {
        atomic_set(&w->state, WSTATE_STOPPED);
        return false;
    }
    w->thread_started = 1;
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
    atomic_u64_set(&lane->oldest_enqueue_tsc, 0);
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
    c.max_spinners             = 0;   /* 0 → nº de cores / 2 */
    c.shutdown_drain_timeout_ms = POOL_DEFAULT_SHUTDOWN_DRAIN_MS;
    c.shutdown_join_timeout_ms  = POOL_DEFAULT_SHUTDOWN_JOIN_MS;
    c.shutdown_force_kill       = false;
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
    long double value = (long double)ns * (long double)cpns;
    if (value >= (long double)UINT64_MAX) return UINT64_MAX;
    return (uint64_t)value;
}

static bool round_up_power_of_two(int value, int* result)
{
    if (!result || value <= 0 || value > (1 << 30)) return false;
    uint32_t rounded = 1;
    while (rounded < (uint32_t)value)
        rounded <<= 1;
    *result = (int)rounded;
    return true;
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
        if (!reserve_push(pool, w)) return false;
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
    if (!xpl_thread_start(&pool->reserve_monitor, reserve_monitor_fn, pool))
        return false;
    pool->reserve_monitor_started = 1;
    if (!xpl_thread_start(&pool->long_task_monitor, long_task_monitor_fn, pool))
        return false;
    pool->long_task_monitor_started = 1;
    return true;
}

ShardedPool* pool_create(const PoolConfig* cfg)
{
    PoolConfig c = cfg ? *cfg : pool_default_config();
    int cores = xcpu_count();
    if (cores < 1) cores = 1;
    if (c.shard_count   <= 0) c.shard_count   = cores;
    if (c.ring_capacity <= 0) c.ring_capacity = POOL_DEFAULT_RING_CAPACITY;
    if (c.shard_count <= 0 || c.ring_capacity > (1 << 30) ||
        (c.ring_capacity & (c.ring_capacity - 1)) != 0 ||
        c.monitor_interval_ms > INT_MAX / 1000)
        return NULL;
    if (c.long_task_threshold_ns == 0) c.long_task_threshold_ns = POOL_DEFAULT_LONG_TASK_NS;

    ShardedPool* pool = (ShardedPool*)calloc(1, sizeof(ShardedPool));
    if (!pool) return NULL;
    pool->log_hook = pool_log_hook_snapshot();
    if (!thread_wait_init(false)) {
        free(pool);
        return NULL;
    }
    pool->wait_runtime_acquired = true;
    xthread_activity_init();

    int max_shards = (c.max_shards > 0) ? c.max_shards : POOL_DEFAULT_MAX_SHARDS;
    if (max_shards < c.shard_count) max_shards = c.shard_count;
    if (max_shards <= 0 || max_shards > (1 << 30)) {
        pool_destroy(pool);
        return NULL;
    }
    pool->lane_capacity            = max_shards;
    pool->initial_lanes            = c.shard_count;

    /* Teto da expansao automatica: default = nº de cores logicos. Mantem-se
     * dentro de [initial_lanes, lane_capacity]. max_shards segue como teto duro. */
    int auto_cap = (c.max_auto_expand_lanes > 0) ? c.max_auto_expand_lanes : cores;
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
                                   ? c.rescue_max_helpers_per_lane : cores;
    pool->park_threshold_tsc       = (c.park_idle_threshold_ms > 0)
                                   ? ns_to_tsc((uint64_t)c.park_idle_threshold_ms * 1000000ULL) : 0;
    pool->rescue_wait_unit_tsc     = ns_to_tsc(500000ULL);  /* 0.5ms por ponto de ranking */

    /* Cap de spinners (Go-style): default = metade dos cores, deixando folga p/
     * produtores/trabalho real e cortando a cauda por oversubscription do spin. */
    pool->spinner_cap              = (c.max_spinners > 0) ? c.max_spinners
                                   : (cores / 2 < 1 ? 1 : cores / 2);
    atomic_set(&pool->spinning_workers, 0);

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
    if (reserve_target <= 0) {
        pool_destroy(pool);
        return NULL;
    }
    atomic_set(&pool->reserve_target, reserve_target);

    /* Orcamento de workers: owners (lane_capacity) + reserva + ajudantes de
     * resgate (~nº cores) + folga. Inclui cores p/ nao esgotar sob resgate. */
    uint64_t worker_base = (uint64_t)(unsigned int)pool->lane_capacity +
                           (uint64_t)(unsigned int)reserve_target +
                           (uint64_t)(unsigned int)cores;
    if (worker_base > (uint64_t)(1 << 28)) {
        pool_destroy(pool);
        return NULL;
    }
    pool->all_worker_capacity = (int)(worker_base * 4u);
    pool->all_workers = (WSWorker**)calloc(pool->all_worker_capacity, sizeof(WSWorker*));
    if (!pool->all_workers) {
        pool_destroy(pool);
        return NULL;
    }

    int reserve_ring_cap = pool->all_worker_capacity;
    int rc;
    if (!round_up_power_of_two(reserve_ring_cap, &rc)) {
        pool_destroy(pool);
        return NULL;
    }
    if (!pool_setup_reserve_ring(pool, rc)) {
        pool_destroy(pool);
        return NULL;
    }

    if (!pool_setup_lanes_and_workers(pool, c.ring_capacity)) {
        pool_destroy(pool);
        return NULL;
    }

    if (!pool_start_all_workers(pool)) {
        pool_destroy(pool);
        return NULL;
    }
    pool_init(pool);
    if (!pool_start_monitors(pool)) {
        pool_destroy(pool);
        return NULL;
    }

    return pool;
}

/* ─────────────────────────────────────────────────────────────────────────
 * API publica: pool_init
 * ───────────────────────────────────────────────────────────────────────── */

void pool_init(ShardedPool* pool)
{
    if (!pool) return;
    int waited_ms = 0;
    while (atomic_get(&pool->workers_ready) < pool->expected_ready)
    {
        xsleep_ms(1);
        if (++waited_ms % 2000 == 0)
            pool_log(pool, POOL_LOG_INFO, "aguardando workers: %d/%d (%d ms)",
                     atomic_get(&pool->workers_ready), pool->expected_ready, waited_ms);
    }
    pool_log(pool, POOL_LOG_INFO, "workers prontos: %d", pool->expected_ready);
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
        reserve_push_or_stop(pool, w);
        return false;
    }

    WSLane* lane = &pool->lanes[idx];

    /* Instala 'w' como owner SOMENTE se a lane estiver sem dono, via CAS atomico
     * (BUG 01 - dono duplo). A versao antiga fazia "if (worker != NULL) reusa;
     * else escreve worker = w": um owner antigo saindo podia re-reivindicar a lane
     * (CAS NULL->w_old) ENTRE o check e a escrita, e a escrita simples sobrescrevia
     * deixando w_old E w como donos. Com o CAS condicional a NULL, esse caso faz o
     * CAS falhar; entao reusamos o owner existente e devolvemos o spare. */
    /* Instala 'w' como owner SOMENTE se a lane estiver sem dono, via CAS atomico
     * (BUG 01 - dono duplo). A versao antiga fazia "if (worker != NULL) reusa;
     * else escreve worker = w": um owner antigo saindo podia re-reivindicar a lane
     * (CAS NULL->w_old) ENTRE o check e a escrita, e a escrita simples sobrescrevia
     * deixando w_old E w como donos. Com o CAS condicional a NULL, esse caso faz o
     * CAS falhar; entao reusamos o owner existente e devolvemos o spare. */
    atomic_set(&w->rescue_mode, 0);
    atomic_set(&w->detached, 0);
#ifdef POOL_TEST_HOOKS
    pool_test_activate_install_wait();   /* janela de dono-duplo: antes do install */
#endif
    void* expected_none = NULL;
    if (atomic_cas_ptr(&lane->worker, &expected_none, w)) {
        atomic_set_ptr(&w->lane, lane);  /* publica o vinculo so apos virar owner */
        thread_wait_wake(&w->reserve_wait);
    } else {
        /* lane ja tem owner (antigo reusado ou concorrente): devolve o spare.
         * w->lane continua NULL (veio da reserva), entao o push e limpo. */
        reserve_push_or_stop(pool, w);
        thread_wait_wake(&lane->wait);
    }

    /* sobe o high-water p/ steal/monitor varrerem a lane nova */
    int hw = atomic_get(&pool->max_active_ever);
    while (hw < idx + 1) {
        int e = hw;
        if (atomic_cas(&pool->max_active_ever, &e, idx + 1)) break;
        hw = atomic_get(&pool->max_active_ever);
    }

    atomic_u64_add(&pool->stat_expansions, 1);
    reserve_monitor_wake(pool);  /* repor reserva */

    int ncores = xcpu_count();
    if (idx + 1 > ncores) {
        int warned = 0;
        if (atomic_cas(&pool->warned_oversubscribe, &warned, 1))
            pool_log(pool, POOL_LOG_WARN, "oversubscription: %d lanes ativas / %d cores logicos",
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

/* Maior nº de workers reivindicando a MESMA lane ativa. Conta apenas donos
 * legitimos (ACTIVE, nao-rescue, nao-detached): o owner unico da lane. > 1
 * indica dono-duplo (BUG 01). Ajudantes (rescue_mode) e em-handoff (detached)
 * sao ignorados pois legitimamente apontam para a lane sem serem o dono. */
int pool_test_max_owners_per_lane(ShardedPool* pool)
{
    int active = atomic_get(&pool->active_lanes);
    int total  = atomic_get(&pool->all_worker_count);
    int maxc   = 0;
    for (int i = 0; i < active; i++) {
        WSLane* L = &pool->lanes[i];
        int c = 0;
        for (int j = 0; j < total; j++) {
            WSWorker* w = pool->all_workers[j];
            if (!w) continue;
            if (atomic_get(&w->state) != WSTATE_ACTIVE) continue;
            if (atomic_get(&w->rescue_mode)) continue;
            if (atomic_get(&w->detached)) continue;
            if ((WSLane*)atomic_get_ptr(&w->lane) == L) c++;
        }
        if (c > maxc) maxc = c;
    }
    return maxc;
}

void pool_test_dump_workers(ShardedPool* pool)
{
    int total  = atomic_get(&pool->all_worker_count);
    int active = atomic_get(&pool->active_lanes);
    fprintf(stderr, "    [dump] active_lanes=%d total_workers=%d\n", active, total);
    for (int i = 0; i < active; i++)
        fprintf(stderr, "    [dump] lane[%d].worker=%p\n", i,
                (void*)atomic_get_ptr(&pool->lanes[i].worker));
    for (int j = 0; j < total; j++) {
        WSWorker* w = pool->all_workers[j];
        if (!w) continue;
        WSLane* L = (WSLane*)atomic_get_ptr(&w->lane);
        int idx = L ? (int)(L - pool->lanes) : -1;
        fprintf(stderr, "    [dump] w[%d]=%p state=%d lane_idx=%d rescue=%d detached=%d\n",
                j, (void*)w, atomic_get(&w->state), idx,
                atomic_get(&w->rescue_mode), atomic_get(&w->detached));
    }
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
    uint64_t no_oldest = 0;
    atomic_u64_cas(&lane->oldest_enqueue_tsc, &no_oldest, t->enqueue_tsc);
    atomic_u64_add(&pool->stat_submitted, 1);
    thread_wait_wake(&lane->wait);
    submit_check_handoff(pool, lane);
    submit_check_rescue(pool, lane);
    return true;
}

bool pool_submit(ShardedPool* pool, task_fn fn, void* arg)
{
    if (!pool || !fn) return false;
    if (!pool_api_enter(pool)) return false;

    Task t = { fn, arg, xpl_rdtscp() };   /* enqueue_tsc p/ tempo de espera real (item 5) */
    int  spins = 0;

    for (;;) {
        if (atomic_get(&pool->shutdown)) {
            atomic_u64_add(&pool->stat_failures, 1);
            pool_api_leave(pool);
            return false;
        }

        int n = atomic_get(&pool->active_lanes);
        if (n < 1) n = 1;
        int widx = (int)(atomic_u32_add(&pool->submit_seq, 1u) % (uint32_t)n);
        WSLane* lane = &pool->lanes[widx];

        if (submit_push_lane(pool, lane, &t)) {
            pool_api_leave(pool);
            return true;
        }

        /* ring cheio → tenta expandir (nova lane) e repete o round-robin */
        if (activate_lane(pool)) continue;

        /* nao expandiu (no teto ou reserva vazia) → tenta qualquer lane com espaco */
        int active = atomic_get(&pool->active_lanes);
        bool placed = false;
        for (int i = 0; i < active; i++) {
            if (submit_push_lane(pool, &pool->lanes[i], &t)) { placed = true; break; }
        }
        if (placed) {
            pool_api_leave(pool);
            return true;
        }

        /* tudo cheio e no teto → backpressure: espera (lento), nunca falha */
        if (g_current_worker_pool == pool) {
            atomic_u64_add(&pool->stat_failures, 1);
            pool_api_leave(pool);
            return false;
        }

        atomic_u64_add(&pool->stat_backpressure, 1);
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
    if (!w->thread_started) {
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
static bool force_kill_worker(WSWorker* w)
{
    if (!w->thread_started || !xpl_thread_force_stop(w->handle))
        return false;
#ifdef XPLATBASE_WIN
    w->handle = NULL;
#else
    memset(&w->handle, 0, sizeof(w->handle));
#endif
    w->thread_started = 0;
    w->force_killed = 1;
    atomic_set(&w->state, WSTATE_STOPPED);
    return true;
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
        if (force_kill_worker(w))
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

static void pool_join_and_free_workers(ShardedPool* pool)
{
    int total = atomic_get(&pool->all_worker_count);
    for (int i = 0; i < total; i++) {
        WSWorker* w = pool->all_workers[i];
        if (!w) continue;
        if (w->force_killed) continue;   /* leak controlado: nao mexe em thread morta a forca */
        if (w->thread_started) {
            xpl_thread_join(w->handle);
            w->thread_started = 0;
        }
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

static void pool_release_wait_runtime(ShardedPool* pool)
{
    if (pool->wait_runtime_acquired) {
        pool->wait_runtime_acquired = false;
        thread_wait_shutdown();
    }
}

static void pool_shutdown_finalize(ShardedPool* pool)
{
    pool_close_submissions_and_monitors(pool);

    if (pool->reserve_monitor_started) {
        xpl_thread_join(pool->reserve_monitor);
        pool->reserve_monitor_started = 0;
        memset(&pool->reserve_monitor, 0, sizeof(pool->reserve_monitor));
    }
    if (pool->long_task_monitor_started) {
        xpl_thread_join(pool->long_task_monitor);
        pool->long_task_monitor_started = 0;
        memset(&pool->long_task_monitor, 0, sizeof(pool->long_task_monitor));
    }

    pool_wake_all_workers(pool);
    uint64_t drain_budget = ns_to_tsc(
        (uint64_t)pool->shutdown_drain_timeout_ms * 1000000ULL);
    uint64_t drain_t0 = xpl_tsc();
    while (atomic_get(&pool->pending_tasks) > 0 &&
           (xpl_tsc() - drain_t0) < drain_budget) {
        pool_wake_all_workers(pool);
        xsleep_ms(1);
    }

    int left = atomic_get(&pool->pending_tasks);
    if (left > 0)
        pool_log(pool, POOL_LOG_WARN,
                 "shutdown: drain timeout (%d ms), %d task(s) restantes",
                 pool->shutdown_drain_timeout_ms, left);

    pool_stop_all_workers(pool);
    pool_wake_all_workers(pool);
    bool all_stopped = pool_wait_workers_stopped_timeout(
        pool, pool->shutdown_join_timeout_ms);

    bool poisoned = false;
    if (!all_stopped) {
        poisoned = true;
        if (pool->shutdown_force_kill) {
            int killed = force_kill_stragglers(pool);
            if (killed > 0) {
                atomic_add(&g_force_killed_total, killed);
                pool_log(pool, POOL_LOG_CRIT,
                         "shutdown: %d worker(s) terminados a forca; pool preservado",
                         killed);
            } else {
                pool_log(pool, POOL_LOG_CRIT,
                         "shutdown: force-kill falhou; pool preservado");
            }
        } else {
            poisoned = true;
            pool_log(pool, POOL_LOG_CRIT,
                     "shutdown: worker(s) nao cooperaram; force-kill desabilitado; "
                     "pool preservado");
        }
    }

    if (poisoned) {
        atomic_set(&pool->shutdown_poisoned, 1);
        pool_release_wait_runtime(pool);
        atomic_set(&pool->shutdown_complete, 1);
        return;
    }

    while (atomic_get(&pool->api_users) != 0)
        xsleep_ms(1);

    thread_wait_destroy(&pool->reserve_monitor_wait);
    thread_wait_destroy(&pool->long_task_monitor_wait);
    pool_join_and_free_workers(pool);
    pool_destroy_lanes(pool);

    if (pool->reserve_ring_buf) {
        ring_queue_destroy(&pool->reserve_ring);
        free(pool->reserve_ring_buf);
        pool->reserve_ring_buf = NULL;
    }

    pool_release_wait_runtime(pool);
    atomic_set(&pool->shutdown_complete, 1);
}

static XPL_FN shutdown_coordinator_fn(void* raw)
{
    pool_shutdown_finalize((ShardedPool*)raw);
    XPL_RET;
}

void pool_shutdown(ShardedPool* pool)
{
    if (!pool) return;

    int expected = 0;
    if (atomic_cas(&pool->shutdown_started, &expected, 1)) {
        pool_close_submissions_and_monitors(pool);
        if (g_current_worker_pool == pool) {
            xpl_thread_t coordinator;
            if (xpl_thread_start(&coordinator, shutdown_coordinator_fn, pool)) {
                xpl_thread_detach(coordinator);
                return;
            }
            atomic_set(&pool->shutdown_started, 0);
            return;
        }
        pool_shutdown_finalize(pool);
        return;
    }

    if (g_current_worker_pool == pool)
        return;
    while (!atomic_get(&pool->shutdown_complete))
        xsleep_ms(1);
}

void pool_destroy(ShardedPool* pool)
{
    if (!pool) return;
    pool_shutdown(pool);
    if (g_current_worker_pool == pool)
        return;
    if (!atomic_get(&pool->shutdown_complete) ||
        atomic_get(&pool->shutdown_poisoned))
        return;
    while (atomic_get(&pool->api_users) != 0)
        xsleep_ms(1);
    free(pool);
}

/* ─────────────────────────────────────────────────────────────────────────
 * API publica: pool_stats
 * ───────────────────────────────────────────────────────────────────────── */

void pool_stats(ShardedPool* pool, PoolStats* out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!pool || !pool_api_enter(pool)) return;

    out->shard_count     = atomic_get(&pool->active_lanes);
    out->total_submitted = atomic_u64_get(&pool->stat_submitted);
    out->submit_failures = atomic_u64_get(&pool->stat_failures);
    out->total_handoffs  = atomic_u64_get(&pool->stat_handoffs);
    out->total_stolen    = atomic_u64_get(&pool->stat_stolen);
    out->total_rescued   = atomic_u64_get(&pool->stat_rescued);
    out->submit_backpressure = atomic_u64_get(&pool->stat_backpressure);
    out->total_expansions    = atomic_u64_get(&pool->stat_expansions);
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
    pool_api_leave(pool);
}
