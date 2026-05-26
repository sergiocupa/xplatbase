/*
 * thread_pool_test.c
 * Testes para thread_pool.c
 *
 * Teste 1 - Basico       : init, submete tarefas, verifica execucao e stats
 * Teste 2 - Perf 1 thread: intervalo fixo, sem sleep na task, mede latencia submit->exec
 * Teste 3 - Perf N threads: N threads configuraveis, intervalo aleatorio 1-50ms
 * Teste 4 - Handoff       : tasks rapidas (10ms) + lentas (1000ms), verifica
 *                           que tasks longas saem do worker do shard e vao para detached
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include "../Xplatbase/Xplatbase/src/thread_pool.h"

/* =========================================================================
 * Platform: contadores atomicos e threads
 * ========================================================================= */

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>

    typedef volatile LONG tp_counter_t;
    #define tp_counter_inc(p)   InterlockedIncrement(p)
    #define tp_counter_read(p)  (*(p))

    typedef HANDLE tp_thread_t;
    #define tp_thread_start(h, fn, arg)  ((h) = CreateThread(NULL, 0, (fn), (arg), 0, NULL))
    #define tp_thread_join(h)            do { WaitForSingleObject((h), INFINITE); CloseHandle(h); } while (0)
#else
    #include <pthread.h>
    #include <stdatomic.h>

    typedef volatile int tp_counter_t;
    #define tp_counter_inc(p)   __sync_add_and_fetch((p), 1)
    #define tp_counter_read(p)  (*(p))

    typedef pthread_t tp_thread_t;
    #define tp_thread_start(h, fn, arg)  pthread_create(&(h), NULL, (fn), (arg))
    #define tp_thread_join(h)            pthread_join((h), NULL)
#endif

/* =========================================================================
 * Timing (TSC calibrado, igual ao test_task_handler.c)
 * ========================================================================= */

static double   tsc_ns_ratio = 1.0;
static uint64_t tsc_overhead = 0;

static void tsc_calibrate(void)
{
#ifdef _WIN32
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);

    uint64_t c0 = __rdtsc();
    QueryPerformanceCounter(&t0);
    Sleep(50);
    uint64_t c1 = __rdtsc();
    QueryPerformanceCounter(&t1);

    double elapsed_ns     = (double)(t1.QuadPart - t0.QuadPart) * 1e9 / (double)freq.QuadPart;
    double elapsed_cycles = (double)(c1 - c0);
    tsc_ns_ratio = elapsed_ns / elapsed_cycles;
#else
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint64_t c0 = __rdtsc();
    usleep(50000);
    uint64_t c1 = __rdtsc();
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed_ns     = (double)(t1.tv_sec - t0.tv_sec) * 1e9 + (double)(t1.tv_nsec - t0.tv_nsec);
    double elapsed_cycles = (double)(c1 - c0);
    tsc_ns_ratio = elapsed_ns / elapsed_cycles;
#endif

    uint64_t min_oh = UINT64_MAX;
    for (int i = 0; i < 1000; i++) {
        uint64_t a = __rdtsc(), b = __rdtsc();
        uint64_t d = b - a;
        if (d < min_oh) min_oh = d;
    }
    tsc_overhead = min_oh;
}

static inline uint64_t tsc_now(void) { return __rdtsc(); }

static inline double tsc_elapsed_ns(uint64_t start, uint64_t end)
{
    uint64_t cycles = end - start;
    if (cycles > tsc_overhead) cycles -= tsc_overhead; else cycles = 0;
    return (double)cycles * tsc_ns_ratio;
}

static uint64_t tp_get_ns(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq = { 0 };
    LARGE_INTEGER counter;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (uint64_t)(counter.QuadPart * 1000000000ULL / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

static void tp_sleep_ms(int ms)
{
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
#endif
}

static void tp_sleep_random_ms(int min_ms, int max_ms)
{
    int ms = min_ms + (rand() % (max_ms - min_ms + 1));
    tp_sleep_ms(ms);
}

/* =========================================================================
 * TpTaskArg: argumento passado a cada task
 * ========================================================================= */

typedef struct {
    int           task_id;
    int           sleep_ms;        /* 0 = sem sleep na funcao alvo */
    volatile int  executed;        /* 0=pendente, 1=ok, -1=submit falhou */
    uint64_t      submit_tsc;      /* TSC capturado antes do pool_submit */
    uint64_t      exec_tsc;        /* TSC capturado no inicio da execucao */
    double        latency_ns;      /* tsc_elapsed_ns(submit_tsc, exec_tsc) */
    tp_counter_t* done_counter;
} TpTaskArg;

/* =========================================================================
 * Resultados de latencia
 * ========================================================================= */

typedef struct {
    int    total;
    int    executed;
    int    failed;
    double avg_ns;
    double min_ns;
    double max_ns;
    double stddev_ns;
} TpLatency;

static TpLatency compute_latency(TpTaskArg* args, int n)
{
    TpLatency r;
    memset(&r, 0, sizeof(r));
    r.total  = n;
    r.min_ns = (double)UINT64_MAX;

    double sum = 0.0;
    for (int i = 0; i < n; i++) {
        if (args[i].executed == 1) {
            r.executed++;
            sum += args[i].latency_ns;
            if (args[i].latency_ns < r.min_ns) r.min_ns = args[i].latency_ns;
            if (args[i].latency_ns > r.max_ns) r.max_ns = args[i].latency_ns;
        } else {
            r.failed++;
        }
    }
    if (r.executed == 0) { r.min_ns = 0.0; return r; }

    r.avg_ns = sum / (double)r.executed;

    double var = 0.0;
    for (int i = 0; i < n; i++)
        if (args[i].executed == 1) {
            double d = args[i].latency_ns - r.avg_ns;
            var += d * d;
        }
    r.stddev_ns = r.executed > 1 ? sqrt(var / (double)r.executed) : 0.0;

    return r;
}

static void print_latency(const TpLatency* r)
{
    printf("    Executadas : %d / %d  (falhas submit: %d)\n", r->executed, r->total, r->failed);
    printf("    Latencia   : avg=%.2f us  min=%.2f us  max=%.2f us  stddev=%.2f us\n",
           r->avg_ns    / 1000.0,
           r->min_ns    / 1000.0,
           r->max_ns    / 1000.0,
           r->stddev_ns / 1000.0);
}

/* =========================================================================
 * Macros de assert
 * ========================================================================= */

static int g_tests_run    = 0;
static int g_tests_passed = 0;

#define TP_ASSERT(cond, msg) do {                       \
    g_tests_run++;                                      \
    if (cond) {                                         \
        g_tests_passed++;                               \
        printf("    [OK]   %s\n", (msg));               \
    } else {                                            \
        printf("    [FAIL] %s\n", (msg));               \
    }                                                   \
} while (0)

/* =========================================================================
 * Callbacks de task
 * ========================================================================= */

/* Task sem sleep: captura latencia e marca como executada. */
static void task_cb_plain(void* arg)
{
    TpTaskArg* t  = (TpTaskArg*)arg;
    t->exec_tsc   = tsc_now();
    t->latency_ns = tsc_elapsed_ns(t->submit_tsc, t->exec_tsc);
    t->executed   = 1;
    tp_counter_inc(t->done_counter);
}

/* Task com sleep configuravel: captura latencia, marca, dorme. */
static void task_cb_with_sleep(void* arg)
{
    TpTaskArg* t  = (TpTaskArg*)arg;
    t->exec_tsc   = tsc_now();
    t->latency_ns = tsc_elapsed_ns(t->submit_tsc, t->exec_tsc);
    t->executed   = 1;
    tp_counter_inc(t->done_counter);
    if (t->sleep_ms > 0)
        tp_sleep_ms(t->sleep_ms);
}

/* =========================================================================
 * Aguarda conclusao de todas as tasks ou timeout
 * ========================================================================= */

static int tp_wait_done(tp_counter_t* done, int total, int timeout_ms)
{
    int elapsed = 0;
    while (tp_counter_read(done) < (LONG)total) {
        if (elapsed >= timeout_ms) return 0;
        tp_sleep_ms(10);
        elapsed += 10;
    }
    return 1;
}

/* =========================================================================
 * TESTE 1: Basico
 *   - cria pool com config padrao
 *   - submete N tarefas
 *   - verifica que todas foram chamadas
 *   - verifica stats basicas
 * ========================================================================= */

#define T1_TASK_COUNT  20

static void test_basic(void)
{
    printf("\n================================================================================\n");
    printf("  TESTE 1: Basico\n");
    printf("================================================================================\n\n");

    ShardedPool* pool = pool_create(NULL);
    TP_ASSERT(pool != NULL, "pool_create(NULL) retorna != NULL");
    if (!pool) return;

    tp_counter_t done = 0;
    TpTaskArg args[T1_TASK_COUNT];
    memset(args, 0, sizeof(args));

    for (int i = 0; i < T1_TASK_COUNT; i++) 
    {
        args[i].task_id      = i;
        args[i].done_counter = &done;
        args[i].submit_tsc   = tsc_now();

        if (!pool_submit(pool, task_cb_plain, &args[i]))
            args[i].executed = -1;
    }

    int completed = tp_wait_done(&done, T1_TASK_COUNT, 5000);
    TP_ASSERT(completed, "todas as tarefas concluidas dentro de 5s");

    int all_ok = 1;
    for (int i = 0; i < T1_TASK_COUNT; i++)
        if (args[i].executed != 1) { all_ok = 0; break; }
    TP_ASSERT(all_ok, "todos os callbacks foram chamados (executed == 1)");

    PoolStats stats;
    pool_stats(pool, &stats);
    TP_ASSERT(stats.total_submitted >= (uint64_t)T1_TASK_COUNT, "total_submitted >= T1_TASK_COUNT");
    TP_ASSERT(stats.submit_failures == 0,                        "sem falhas de submit");

    printf("  Stats: shards=%d  active_workers=%d  reserve=%d  submitted=%llu  handoffs=%llu\n",
           stats.shard_count,
           stats.active_worker_count,
           stats.reserve_count,
           (unsigned long long)stats.total_submitted,
           (unsigned long long)stats.total_handoffs);

    TpLatency r = compute_latency(args, T1_TASK_COUNT);
    print_latency(&r);

    pool_shutdown(pool);
    printf("  pool_shutdown: ok\n");
}

/* =========================================================================
 * TESTE 2: Performance single-thread, intervalo fixo
 *   - 1 thread de submit, sem sleep na funcao alvo
 *   - intervalo fixo de T2_INTERVAL_MS entre submits
 *   - mede latencia submit_tsc -> exec_tsc
 * ========================================================================= */

#define T2_TASK_COUNT    100
#define T2_INTERVAL_MS   5

static void test_single_thread_perf(void)
{
    printf("\n================================================================================\n");
    printf("  TESTE 2: Performance single-thread (intervalo fixo %d ms, %d tasks)\n",
           T2_INTERVAL_MS, T2_TASK_COUNT);
    printf("================================================================================\n\n");

    ShardedPool* pool = pool_create(NULL);
    TP_ASSERT(pool != NULL, "pool_create != NULL");
    if (!pool) return;

    tp_counter_t done = 0;
    TpTaskArg* args = (TpTaskArg*)calloc(T2_TASK_COUNT, sizeof(TpTaskArg));
    if (!args) { pool_shutdown(pool); return; }

    for (int i = 0; i < T2_TASK_COUNT; i++) {
        args[i].task_id      = i;
        args[i].done_counter = &done;
    }

    uint64_t wall_start = tp_get_ns();

    for (int i = 0; i < T2_TASK_COUNT; i++) {
        args[i].submit_tsc = tsc_now();
        if (!pool_submit(pool, task_cb_plain, &args[i]))
            args[i].executed = -1;
        tp_sleep_ms(T2_INTERVAL_MS);
    }

    int completed = tp_wait_done(&done, T2_TASK_COUNT, 10000);
    uint64_t wall_end = tp_get_ns();

    pool_shutdown(pool);

    TP_ASSERT(completed, "todas as tarefas concluidas no tempo limite");
    TpLatency r = compute_latency(args, T2_TASK_COUNT);
    TP_ASSERT(r.executed == T2_TASK_COUNT, "todos os callbacks foram chamados");
    TP_ASSERT(r.failed   == 0,             "sem falhas de submit");

    printf("  Tempo parede  : %.1f ms\n", (double)(wall_end - wall_start) / 1e6);
    print_latency(&r);

    free(args);
}

/* =========================================================================
 * TESTE 3: Performance multi-thread, intervalo aleatorio 1..50 ms
 * ========================================================================= */

#define T3_TASKS_PER_THREAD  50
#define T3_THREAD_COUNT      4     /* configuravel */
#define T3_INTERVAL_MIN_MS   1
#define T3_INTERVAL_MAX_MS   50

typedef struct {
    ShardedPool*  pool;
    TpTaskArg*    args;
    int           task_count;
    tp_counter_t* done_counter;
    tp_counter_t* submit_failed;
} T3ThreadArg;

#ifdef _WIN32
static DWORD WINAPI t3_submit_fn(void* raw)
#else
static void* t3_submit_fn(void* raw)
#endif
{
    T3ThreadArg* ta = (T3ThreadArg*)raw;
    for (int i = 0; i < ta->task_count; i++) {
        TpTaskArg* t  = &ta->args[i];
        t->submit_tsc = tsc_now();
        if (!pool_submit(ta->pool, task_cb_plain, t)) {
            t->executed = -1;
            tp_counter_inc(ta->submit_failed);
            tp_counter_inc(ta->done_counter);
        }
        tp_sleep_random_ms(T3_INTERVAL_MIN_MS, T3_INTERVAL_MAX_MS);
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static void test_multi_thread_perf(int thread_count)
{
    printf("\n================================================================================\n");
    printf("  TESTE 3: Performance multi-thread (%d threads, intervalo %d-%d ms, %d tasks/thread)\n",
           thread_count, T3_INTERVAL_MIN_MS, T3_INTERVAL_MAX_MS, T3_TASKS_PER_THREAD);
    printf("================================================================================\n\n");

    int total = thread_count * T3_TASKS_PER_THREAD;

    ShardedPool*  pool    = pool_create(NULL);
    TpTaskArg*    args    = (TpTaskArg*)   calloc(total,        sizeof(TpTaskArg));
    T3ThreadArg*  targs   = (T3ThreadArg*) calloc(thread_count, sizeof(T3ThreadArg));
    tp_thread_t*  handles = (tp_thread_t*) calloc(thread_count, sizeof(tp_thread_t));

    TP_ASSERT(pool != NULL, "pool_create != NULL");
    if (!pool || !args || !targs || !handles) {
        free(args); free(targs); free(handles);
        if (pool) pool_shutdown(pool);
        return;
    }

    tp_counter_t done        = 0;
    tp_counter_t submit_fail = 0;

    for (int i = 0; i < total; i++) {
        args[i].task_id      = i;
        args[i].done_counter = &done;
    }

    uint64_t wall_start = tp_get_ns();

    for (int th = 0; th < thread_count; th++) {
        targs[th].pool          = pool;
        targs[th].args          = &args[th * T3_TASKS_PER_THREAD];
        targs[th].task_count    = T3_TASKS_PER_THREAD;
        targs[th].done_counter  = &done;
        targs[th].submit_failed = &submit_fail;
        tp_thread_start(handles[th], t3_submit_fn, &targs[th]);
    }

    /* timeout: intervalo maximo * tarefas_por_thread + margem de execucao */
    int timeout_ms = T3_INTERVAL_MAX_MS * T3_TASKS_PER_THREAD + 10000;
    int completed  = tp_wait_done(&done, total, timeout_ms);
    uint64_t wall_end = tp_get_ns();

    for (int th = 0; th < thread_count; th++)
        tp_thread_join(handles[th]);

    pool_shutdown(pool);

    TP_ASSERT(completed, "todas as tarefas concluidas no tempo limite");

    TpLatency r = compute_latency(args, total);
    TP_ASSERT(r.executed == total, "todos os callbacks foram chamados");
    TP_ASSERT(r.failed   == 0,     "sem falhas de submit");

    printf("  Threads       : %d\n", thread_count);
    printf("  Tempo parede  : %.1f ms\n", (double)(wall_end - wall_start) / 1e6);
    print_latency(&r);

    free(args); free(targs); free(handles);
}

/* =========================================================================
 * TESTE 4: Handoff - tasks rapidas + lentas
 *
 *   Submete T4_FAST_COUNT tasks que dormem T4_FAST_SLEEP_MS (rapidas)
 *   e T4_SLOW_COUNT tasks que dormem T4_SLOW_SLEEP_MS (lentas).
 *
 *   Pool configurado com long_threshold_ns = T4_LONG_THRESHOLD_NS de forma
 *   que as tasks lentas superem o limiar e sejam detectadas pelo monitor.
 *
 *   O monitor desanexa o worker bloqueado do shard e move-o para
 *   pool->detached; um worker da reserva assume o shard.
 *
 *   Verificacoes:
 *     - todos os callbacks foram chamados
 *     - total_handoffs > 0 (tasks lentas saaram do worker ativo do shard)
 * ========================================================================= */

#define T4_FAST_COUNT        4
#define T4_SLOW_COUNT        2
#define T4_FAST_SLEEP_MS     10
#define T4_SLOW_SLEEP_MS     1000

/* threshold entre 10ms e 1000ms para que rapidas nao disparem handoff */
#define T4_LONG_THRESHOLD_NS  (200LL * 1000000LL)   /* 200 ms */

static void test_handoff(void)
{
    printf("\n================================================================================\n");
    printf("  TESTE 4: Handoff (tasks rapidas %dms + lentas %dms, threshold=%lldms)\n",
           T4_FAST_SLEEP_MS, T4_SLOW_SLEEP_MS,
           (long long)(T4_LONG_THRESHOLD_NS / 1000000LL));
    printf("================================================================================\n\n");

    PoolConfig cfg                        = pool_default_config();
    cfg.shard_count                       = 2;   /* poucos shards aumentam chance de conflito */
    cfg.reserve_size                      = 4;   /* reserva suficiente para handoffs */
    cfg.task_thresholds.long_threshold_ns = (uint64_t)T4_LONG_THRESHOLD_NS;
    cfg.task_thresholds.blocked_ratio_max = 0.5;
    cfg.task_thresholds.cpu_ratio_min     = 0.70;
    cfg.task_thresholds_set               = true;

    ShardedPool* pool = pool_create(&cfg);
    TP_ASSERT(pool != NULL, "pool_create com config customizada != NULL");
    if (!pool) return;

    int total = T4_FAST_COUNT + T4_SLOW_COUNT;
    tp_counter_t done = 0;

    TpTaskArg args[T4_FAST_COUNT + T4_SLOW_COUNT];
    memset(args, 0, sizeof(args));

    /* submete tasks rapidas */
    for (int i = 0; i < T4_FAST_COUNT; i++) {
        args[i].task_id      = i;
        args[i].sleep_ms     = T4_FAST_SLEEP_MS;
        args[i].done_counter = &done;
        args[i].submit_tsc   = tsc_now();
        if (!pool_submit(pool, task_cb_with_sleep, &args[i]))
            args[i].executed = -1;
    }

    /* submete tasks lentas */
    for (int i = 0; i < T4_SLOW_COUNT; i++) {
        int idx = T4_FAST_COUNT + i;
        args[idx].task_id      = idx;
        args[idx].sleep_ms     = T4_SLOW_SLEEP_MS;
        args[idx].done_counter = &done;
        args[idx].submit_tsc   = tsc_now();
        if (!pool_submit(pool, task_cb_with_sleep, &args[idx]))
            args[idx].executed = -1;
    }

    /*
     * Aguarda 3x o threshold para dar ao monitor tempo de detectar as tasks
     * lentas e realizar o handoff antes de checar os stats intermediarios.
     */
    int snapshot_wait_ms = (int)(T4_LONG_THRESHOLD_NS / 1000000LL) * 3;
    tp_sleep_ms(snapshot_wait_ms);

    PoolStats mid_stats;
    pool_stats(pool, &mid_stats);

    /* aguarda conclusao de todas (tasks lentas dominam o timeout) */
    int timeout_ms = T4_SLOW_SLEEP_MS * T4_SLOW_COUNT + 5000;
    int completed  = tp_wait_done(&done, total, timeout_ms);

    PoolStats final_stats;
    pool_stats(pool, &final_stats);

    pool_shutdown(pool);

    /* --- asserts --- */
    TP_ASSERT(completed, "todas as tarefas concluidas no tempo limite");

    int all_ok = 1;
    for (int i = 0; i < total; i++)
        if (args[i].executed != 1) { all_ok = 0; break; }
    TP_ASSERT(all_ok, "todos os callbacks foram chamados (executed == 1)");

    TP_ASSERT(final_stats.total_handoffs > 0,
              "total_handoffs > 0: tasks lentas saaram do worker ativo e foram para detached");

    /* --- relatorio --- */
    printf("  Stats apos %dms (snapshot intermediario):\n", snapshot_wait_ms);
    printf("    handoffs      : %llu\n", (unsigned long long)mid_stats.total_handoffs);
    printf("    detached_count: %d\n",   mid_stats.detached_count);
    printf("    reserve_count : %d\n",   mid_stats.reserve_count);
    printf("    active_workers: %d\n",   mid_stats.active_worker_count);

    printf("  Stats finais:\n");
    printf("    total_handoffs  : %llu\n", (unsigned long long)final_stats.total_handoffs);
    printf("    submit_failures : %llu\n", (unsigned long long)final_stats.submit_failures);

    TpLatency r = compute_latency(args, total);
    print_latency(&r);

    /* latencia individual por task */
    printf("  Latencia por task:\n");
    for (int i = 0; i < total; i++) {
        const char* tipo = (i < T4_FAST_COUNT) ? "rapida" : "lenta ";
        printf("    task[%d] %s  exec=%s  latencia=%.2f us\n",
               i, tipo,
               args[i].executed == 1 ? "ok" : "FALHOU",
               args[i].latency_ns / 1000.0);
    }
}

/* =========================================================================
 * Entrypoint
 * ========================================================================= */

void thread_pool_test_run(void)
{
    tsc_calibrate();
    srand(42);

    g_tests_run    = 0;
    g_tests_passed = 0;

    printf("\n");
    printf("################################################################################\n");
    printf("  thread_pool_test\n");
    printf("################################################################################\n");

    test_basic();
    /*test_single_thread_perf();
    test_multi_thread_perf(T3_THREAD_COUNT);
    test_handoff();*/

    printf("\n================================================================================\n");
    printf("  RESULTADO FINAL: %d / %d asserts passaram\n", g_tests_passed, g_tests_run);
    printf("================================================================================\n\n");
}


int main(void)
{
    thread_pool_test_run();
}