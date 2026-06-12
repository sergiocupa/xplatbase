/*
 * thread_pool_test.c
 * Testes para thread_pool.c (WSPool: work-stealing)
 *
 * Teste 1 - Basico       : init, submete tarefas, verifica execucao e stats
 * Teste 2 - Perf 1 thread: intervalo fixo, sem sleep na task, mede latencia submit->exec
 * Teste 3 - Perf N threads: N threads configuraveis, intervalo aleatorio 1-50ms
 * Teste 4 - Stealing      : tasks rapidas + lentas; verifica que workers ociosos
 *                           roubam tasks dos rings de workers ocupados (stolen > 0)
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
    /* seg*1e9 + fracao: evita overflow de (counter*1e9) em uptimes longos */
    uint64_t c = (uint64_t)counter.QuadPart, f = (uint64_t)freq.QuadPart;
    return (c / f) * 1000000000ULL + ((c % f) * 1000000000ULL) / f;
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
 * Monitor de CPU do processo
 *   Coleta a cada interval_ms: % CPU = delta_cpu / (delta_wall * ncpus)
 *   Apresenta avg/min/max no fim.
 * ========================================================================= */

#define CPU_MON_MAX_SAMPLES  600   /* 60s @ 100ms */

#ifdef _WIN32
static uint64_t cpu_time_us(void)
{
    FILETIME ct, et, kt, ut;
    GetProcessTimes(GetCurrentProcess(), &ct, &et, &kt, &ut);
    uint64_t k = ((uint64_t)kt.dwHighDateTime << 32 | (uint64_t)kt.dwLowDateTime) / 10;
    uint64_t u = ((uint64_t)ut.dwHighDateTime << 32 | (uint64_t)ut.dwLowDateTime) / 10;
    return k + u;
}
static uint64_t wall_time_us(void)
{
    static LARGE_INTEGER freq = {0};
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart * 1000000ULL / freq.QuadPart);
}
#else
#include <sys/resource.h>
static uint64_t cpu_time_us(void)
{
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return (uint64_t)ru.ru_utime.tv_sec  * 1000000ULL + (uint64_t)ru.ru_utime.tv_usec
         + (uint64_t)ru.ru_stime.tv_sec  * 1000000ULL + (uint64_t)ru.ru_stime.tv_usec;
}
static uint64_t wall_time_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000;
}
#endif

typedef struct {
    tp_thread_t   thread;
    volatile int  stop;
    int           interval_ms;
    int           num_cpus;
    double        samples[CPU_MON_MAX_SAMPLES];
    int           count;
} CpuMonitor;

#ifdef _WIN32
static DWORD WINAPI cpu_monitor_fn(void* raw)
#else
static void* cpu_monitor_fn(void* raw)
#endif
{
    CpuMonitor* m = (CpuMonitor*)raw;

    uint64_t prev_cpu  = cpu_time_us();
    uint64_t prev_wall = wall_time_us();

    while (!m->stop) {
        tp_sleep_ms(m->interval_ms);
        if (m->stop) break;

        uint64_t cur_cpu  = cpu_time_us();
        uint64_t cur_wall = wall_time_us();
        uint64_t dcpu     = cur_cpu  - prev_cpu;
        uint64_t dwall    = cur_wall - prev_wall;

        if (dwall > 0 && m->count < CPU_MON_MAX_SAMPLES)
            m->samples[m->count++] = (double)dcpu / (double)dwall * 100.0 / m->num_cpus;

        prev_cpu  = cur_cpu;
        prev_wall = cur_wall;
    }

#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static void cpu_monitor_start(CpuMonitor* m, int interval_ms)
{
    memset(m, 0, sizeof(*m));
    m->interval_ms = interval_ms;
    m->num_cpus    = xcpu_count();
    tp_thread_start(m->thread, cpu_monitor_fn, m);
}

static void cpu_monitor_stop(CpuMonitor* m)
{
    m->stop = 1;
    tp_thread_join(m->thread);
}

static void cpu_monitor_print(const CpuMonitor* m)
{
    if (m->count == 0) {
        printf("  CPU: sem amostras\n");
        return;
    }
    double sum = 0.0, mn = 1e9, mx = -1.0;
    for (int i = 0; i < m->count; i++) {
        sum += m->samples[i];
        if (m->samples[i] < mn) mn = m->samples[i];
        if (m->samples[i] > mx) mx = m->samples[i];
    }
    printf("  CPU processo (%d amostras @ %dms, %d CPUs): "
           "avg=%.1f%%  min=%.1f%%  max=%.1f%%\n",
           m->count, m->interval_ms, m->num_cpus,
           sum / m->count, mn, mx);
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

/* Task com sleep configuravel: captura latencia, marca, dorme.
 * Conta no INICIO (mede latencia submit->exec, nao conclusao). */
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

/* Igual a anterior, mas conta no FIM — o done_counter so sobe quando a task
 * realmente conclui. Permite medir tempo parede ate o throughput completo. */
static void task_cb_sleep_finish(void* arg)
{
    TpTaskArg* t  = (TpTaskArg*)arg;
    t->exec_tsc   = tsc_now();
    t->latency_ns = tsc_elapsed_ns(t->submit_tsc, t->exec_tsc);
    t->executed   = 1;
    if (t->sleep_ms > 0)
        tp_sleep_ms(t->sleep_ms);
    tp_counter_inc(t->done_counter);
}

/* =========================================================================
 * Aguarda conclusao de todas as tasks ou timeout
 * ========================================================================= */

static int tp_wait_done(tp_counter_t* done, int total, int timeout_ms)
{
    int elapsed = 0;
    while (tp_counter_read(done) < (LONG)total) {
        if (elapsed >= timeout_ms) return 0;
        tp_sleep_ms(1);          /* granularidade fina p/ medir tempo parede */
        elapsed += 1;
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

static void test_basic_run(ShardedPool* pool, const char* label)
{
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
    TP_ASSERT(all_ok, "todos os callbacks foram chamados");

    TpLatency r = compute_latency(args, T1_TASK_COUNT);
    printf("  [%s] ", label);
    print_latency(&r);
}

static void test_basic(void)
{
    printf("\n================================================================================\n");
    printf("  TESTE 1: Basico\n");
    printf("================================================================================\n\n");

    /* --- variante A: config padrao (spin_budget_us = 2000) --- */
    {
        fprintf(stderr, "[T1-A] pool_create...\n");
        ShardedPool* pool = pool_create(NULL);
        TP_ASSERT(pool != NULL, "pool_create padrao != NULL");
        if (pool)
        {
            fprintf(stderr, "[T1-A] test_basic_run...\n");
            test_basic_run(pool, "default spin_budget=2000us");
            fprintf(stderr, "[T1-A] pool_shutdown...\n");
            pool_shutdown(pool);
            fprintf(stderr, "[T1-A] done\n");
        }
    }

    /* --- variante B: 1 shard (sem rotacao, sem pressao de OS) --- */
    {
        fprintf(stderr, "[T1-B] pool_create...\n");
        PoolConfig cfg      = pool_default_config();
        cfg.shard_count     = 1;
        ShardedPool* pool = pool_create(&cfg);
        TP_ASSERT(pool != NULL, "pool_create 1 shard != NULL");
        if (pool)
        {
            fprintf(stderr, "[T1-B] test_basic_run...\n");
            test_basic_run(pool, "1 shard, budget=2000us");
            fprintf(stderr, "[T1-B] pool_shutdown...\n");
            pool_shutdown(pool);
            fprintf(stderr, "[T1-B] done\n");
        }
    }

    /* --- variante C: shards = ncores/2 (menos pressao, ainda paralelo) --- */
    {
        fprintf(stderr, "[T1-C] pool_create...\n");
        PoolConfig cfg      = pool_default_config();
        cfg.shard_count     = xcpu_count() / 2;
        if (cfg.shard_count < 1) cfg.shard_count = 1;
        ShardedPool* pool = pool_create(&cfg);
        TP_ASSERT(pool != NULL, "pool_create ncores/2 shards != NULL");
        if (pool)
        {
            char label[64];
            sprintf(label, "%d shards (ncores/2), budget=2000us", cfg.shard_count);
            fprintf(stderr, "[T1-C] test_basic_run...\n");
            test_basic_run(pool, label);
            fprintf(stderr, "[T1-C] pool_shutdown...\n");
            pool_shutdown(pool);
            fprintf(stderr, "[T1-C] done\n");
        }
    }

    /* --- variante D: spin=0 contagem (baseline original) --- */
    {
        fprintf(stderr, "[T1-D] pool_create...\n");
        PoolConfig cfg      = pool_default_config();
        cfg.spin_budget_us  = 0;
        ShardedPool* pool = pool_create(&cfg);
        TP_ASSERT(pool != NULL, "pool_create spin=0 (contagem) != NULL");
        if (pool)
        {
            fprintf(stderr, "[T1-D] test_basic_run...\n");
            test_basic_run(pool, "spin_budget=0 (contagem 2048)");
            fprintf(stderr, "[T1-D] pool_shutdown...\n");
            pool_shutdown(pool);
            fprintf(stderr, "[T1-D] done\n");
        }
    }

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

    printf("  Stats: workers=%d  active=%d  submitted=%llu  stolen=%llu\n",
           stats.shard_count,
           stats.active_worker_count,
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
 * TESTE 4: Stealing - tasks rapidas + lentas
 *
 *   Submete T4_FAST_COUNT tasks rapidas e T4_SLOW_COUNT tasks lentas.
 *   Com poucos workers e tasks lentas ocupando alguns deles, workers
 *   ociosos devem roubar tasks do ring de workers ocupados.
 *
 *   Verificacoes:
 *     - todos os callbacks foram chamados
 *     - total_handoffs (= stolen) > 0: houve roubo de tasks
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
    printf("  TESTE 4: Stealing (tasks rapidas %dms + lentas %dms)\n",
           T4_FAST_SLEEP_MS, T4_SLOW_SLEEP_MS);
    printf("================================================================================\n\n");

    PoolConfig cfg      = pool_default_config();
    cfg.shard_count     = 2;   /* poucos workers aumentam chance de roubo */

    ShardedPool* pool = pool_create(&cfg);
    TP_ASSERT(pool != NULL, "pool_create com config customizada != NULL");
    if (!pool) return;

    int total = T4_FAST_COUNT + T4_SLOW_COUNT;
    tp_counter_t done = 0;

    TpTaskArg args[T4_FAST_COUNT + T4_SLOW_COUNT];
    memset(args, 0, sizeof(args));

    /* submete tasks lentas */
    for (int i = 0; i < T4_SLOW_COUNT; i++) {
        int idx = T4_FAST_COUNT + i;
        args[idx].task_id = idx;
        args[idx].sleep_ms = T4_SLOW_SLEEP_MS;
        args[idx].done_counter = &done;
        args[idx].submit_tsc = tsc_now();
        if (!pool_submit(pool, task_cb_with_sleep, &args[idx]))
            args[idx].executed = -1;
    }

    tp_sleep_ms(100);

    /* submete tasks rapidas */
    for (int i = 0; i < T4_FAST_COUNT; i++) {
        args[i].task_id      = i;
        args[i].sleep_ms     = T4_FAST_SLEEP_MS;
        args[i].done_counter = &done;
        args[i].submit_tsc   = tsc_now();
        if (!pool_submit(pool, task_cb_with_sleep, &args[i]))
            args[i].executed = -1;
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
              "stolen > 0: workers ociosos roubaram tasks dos rings de workers ocupados");

    /* --- relatorio --- */
    printf("  Stats apos %dms (snapshot intermediario):\n", snapshot_wait_ms);
    printf("    stolen        : %llu\n", (unsigned long long)mid_stats.total_handoffs);
    printf("    active_workers: %d\n",   mid_stats.active_worker_count);

    printf("  Stats finais:\n");
    printf("    stolen          : %llu\n", (unsigned long long)final_stats.total_handoffs);
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
 * TESTE 5: Resgate por backlog
 *
 *   1 lane (shard_count=1) força backlog: uma rajada de tasks curtas (10ms,
 *   abaixo do threshold de task longa) empilha no ring. O owner sozinho
 *   serializaria (~10ms * posicao). O monitor de resgate deve trazer um
 *   ajudante da reserva para drenar em paralelo.
 *
 *   Verificacoes:
 *     - todos os callbacks foram chamados
 *     - total_rescued > 0: houve resgate por backlog
 * ========================================================================= */

#define T5_TASK_COUNT     8
#define T5_SLEEP_MS       10

/* Mede tempo parede ATE A CONCLUSAO (task_cb_sleep_finish). Com reserva
 * generosa, o teto dinamico de ajudantes deixa as tasks rodarem quase em
 * paralelo (owner + N ajudantes), aproximando o tempo de 1 rodada (~sleep_ms). */
static void test_rescue(void)
{
    printf("\n================================================================================\n");
    printf("  TESTE 5: Resgate por backlog (1 lane, %d tasks de %dms em rajada)\n",
           T5_TASK_COUNT, T5_SLEEP_MS);
    printf("================================================================================\n\n");

    PoolConfig cfg     = pool_default_config();
    cfg.shard_count    = 1;              /* 1 lane → forca backlog no unico ring */
    cfg.reserve_size   = T5_TASK_COUNT;  /* reserva generosa p/ escalar ajudantes */

    ShardedPool* pool = pool_create(&cfg);
    TP_ASSERT(pool != NULL, "pool_create 1 shard != NULL");
    if (!pool) return;

    tp_counter_t done = 0;
    TpTaskArg args[T5_TASK_COUNT];
    memset(args, 0, sizeof(args));

    uint64_t wall_start = tp_get_ns();

    /* rajada: submete tudo de uma vez, sem intervalo */
    for (int i = 0; i < T5_TASK_COUNT; i++) {
        args[i].task_id      = i;
        args[i].sleep_ms     = T5_SLEEP_MS;
        args[i].done_counter = &done;
        args[i].submit_tsc   = tsc_now();
        if (!pool_submit(pool, task_cb_sleep_finish, &args[i]))
            args[i].executed = -1;
    }

    int timeout_ms = T5_SLEEP_MS * T5_TASK_COUNT + 5000;
    int completed  = tp_wait_done(&done, T5_TASK_COUNT, timeout_ms);
    uint64_t wall_end = tp_get_ns();

    PoolStats stats;
    pool_stats(pool, &stats);

    pool_shutdown(pool);

    TP_ASSERT(completed, "todas as tarefas concluidas no tempo limite");

    int all_ok = 1;
    for (int i = 0; i < T5_TASK_COUNT; i++)
        if (args[i].executed != 1) { all_ok = 0; break; }
    TP_ASSERT(all_ok, "todos os callbacks foram chamados (executed == 1)");

    TP_ASSERT(stats.total_rescued > 0,
              "total_rescued > 0: ajudantes da reserva drenaram o backlog");

    /* com reserva generosa, o tempo ate a conclusao deve ficar bem abaixo do
     * serial (8x10=80ms): owner + ajudantes rodam quase em paralelo */
    double wall_ms = (double)(wall_end - wall_start) / 1e6;
    TP_ASSERT(wall_ms < (double)(T5_SLEEP_MS * T5_TASK_COUNT) * 0.6,
              "tempo ate conclusao < 60% do serial (ajudantes paralelizaram)");

    printf("  Tempo parede (ate conclusao): %.1f ms (serial seria ~%d ms)\n",
           wall_ms, T5_SLEEP_MS * T5_TASK_COUNT);
    printf("  total_rescued (ajudantes despachados): %llu\n",
           (unsigned long long)stats.total_rescued);

    TpLatency r = compute_latency(args, T5_TASK_COUNT);
    print_latency(&r);

    printf("  Latencia por task (submit->inicio):\n");
    for (int i = 0; i < T5_TASK_COUNT; i++)
        printf("    task[%d]  exec=%s  latencia=%.2f us\n",
               i, args[i].executed == 1 ? "ok" : "FALHOU",
               args[i].latency_ns / 1000.0);
}

/* =========================================================================
 * TESTE 6: Gate de ocupacao — NAO resgatar quando ha capacidade ociosa
 *
 *   Muitas lanes, poucas tasks curtas: nenhum backlog se forma e ha workers
 *   ociosos. O gate de ocupacao deve impedir qualquer resgate (total_rescued
 *   == 0) — prova que o resgate nao atropela o caminho normal/steal.
 * ========================================================================= */

#define T6_TASK_COUNT     4

static void test_rescue_no_overfire(void)
{
    printf("\n================================================================================\n");
    printf("  TESTE 6: Gate de ocupacao (muitas lanes, %d tasks curtas → sem resgate)\n",
           T6_TASK_COUNT);
    printf("================================================================================\n\n");

    PoolConfig cfg  = pool_default_config();
    int nc          = xcpu_count();
    cfg.shard_count = nc < 4 ? 4 : nc;   /* lanes de sobra → sempre ha worker ocioso */

    ShardedPool* pool = pool_create(&cfg);
    TP_ASSERT(pool != NULL, "pool_create != NULL");
    if (!pool) return;

    tp_counter_t done = 0;
    TpTaskArg args[T6_TASK_COUNT];
    memset(args, 0, sizeof(args));

    for (int i = 0; i < T6_TASK_COUNT; i++) {
        args[i].task_id      = i;
        args[i].sleep_ms     = 0;       /* curtas: terminam na hora, nada acumula */
        args[i].done_counter = &done;
        args[i].submit_tsc   = tsc_now();
        if (!pool_submit(pool, task_cb_with_sleep, &args[i]))
            args[i].executed = -1;
        tp_sleep_ms(2);                 /* espaca p/ garantir que nao enfileira */
    }

    int completed = tp_wait_done(&done, T6_TASK_COUNT, 5000);

    PoolStats stats;
    pool_stats(pool, &stats);

    pool_shutdown(pool);

    TP_ASSERT(completed, "todas as tarefas concluidas no tempo limite");
    TP_ASSERT(stats.total_rescued == 0,
              "total_rescued == 0: gate impediu resgate espurio (steal/owner bastam)");

    printf("  lanes=%d  total_rescued=%llu (esperado 0)\n",
           stats.shard_count, (unsigned long long)stats.total_rescued);
}

/* =========================================================================
 * TESTE 7: Latencia de engate do resgate numa rajada
 *
 *   1 lane + reserva generosa. Rajada de N tasks de Xms. As tasks enfileiradas
 *   (indices 1..N-1) sao drenadas por ajudantes de resgate.
 *
 *   Objetivo: medir QUANDO os ajudantes engatam. No estado atual eles so
 *   engatam no backstop do monitor (~POOL_LONG_TASK_MONITOR_MS), porque durante
 *   a rajada de microssegundos o owner ainda nao marcou busy_workers e o gate
 *   de ocupacao reprova o resgate sincrono. Logo a latencia media de engate
 *   fica ~= intervalo do monitor.
 *
 *   Este teste assere o comportamento ALVO (engate rapido): ele FALHA hoje
 *   (vermelho/TDD) e deve passar apos o fix do engate sincrono.
 * ========================================================================= */

#define T7_TASK_COUNT       8
#define T7_SLEEP_MS         10
#define T7_ENGAGE_TARGET_MS 5    /* alvo: ajudantes engatam em < 5ms */

static void test_rescue_engage_latency(void)
{
    printf("\n================================================================================\n");
    printf("  TESTE 7: Latencia de engate do resgate (rajada, alvo < %d ms)\n",
           T7_ENGAGE_TARGET_MS);
    printf("================================================================================\n\n");

    PoolConfig cfg     = pool_default_config();
    cfg.shard_count    = 1;
    cfg.reserve_size   = T7_TASK_COUNT;  /* ajudantes de sobra na reserva */

    ShardedPool* pool = pool_create(&cfg);
    TP_ASSERT(pool != NULL, "pool_create 1 shard != NULL");
    if (!pool) return;

    tp_counter_t done = 0;
    TpTaskArg args[T7_TASK_COUNT];
    memset(args, 0, sizeof(args));

    /* rajada apertada */
    for (int i = 0; i < T7_TASK_COUNT; i++) {
        args[i].task_id      = i;
        args[i].sleep_ms     = T7_SLEEP_MS;
        args[i].done_counter = &done;
        args[i].submit_tsc   = tsc_now();
        if (!pool_submit(pool, task_cb_with_sleep, &args[i]))
            args[i].executed = -1;
    }

    int timeout_ms = T7_SLEEP_MS * T7_TASK_COUNT + 5000;
    int completed  = tp_wait_done(&done, T7_TASK_COUNT, timeout_ms);

    pool_shutdown(pool);

    TP_ASSERT(completed, "todas as tarefas concluidas no tempo limite");

    /* latencia de engate = media submit->inicio das tasks ENFILEIRADAS (1..N-1).
     * task[0] e pega pelo owner na hora; as demais dependem dos ajudantes. */
    double sum = 0.0; int n = 0;
    double mn = 1e30, mx = -1.0;
    for (int i = 1; i < T7_TASK_COUNT; i++) {
        if (args[i].executed != 1) continue;
        double ms = args[i].latency_ns / 1e6;
        sum += ms; n++;
        if (ms < mn) mn = ms;
        if (ms > mx) mx = ms;
    }
    double avg_engage = n ? sum / n : 0.0;

    printf("  Engate das tasks enfileiradas (submit->inicio):\n");
    printf("    avg=%.2f ms  min=%.2f ms  max=%.2f ms  (alvo avg < %d ms)\n",
           avg_engage, mn, mx, T7_ENGAGE_TARGET_MS);

    TP_ASSERT(avg_engage < (double)T7_ENGAGE_TARGET_MS,
              "ajudantes engatam rapido (avg engate < alvo) [TDD: vermelho ate o fix]");
}

/* =========================================================================
 * TESTE 8: Pressao sustentada — afericao do cenario que a expansao de lanes
 *          (item 1, ainda NAO implementado) deve atacar.
 *
 *   Muitos produtores inundam o pool com tasks curtas. Com POUCAS lanes, os
 *   poucos rings sofrem contencao de push e enchem → submit_failures e latencia
 *   alta. Rodamos o MESMO workload com shard_count=2 (pressao) e
 *   shard_count=ncores (folga, ~= o que a expansao dinamica convergiria) para
 *   medir a lacuna.
 *
 *   Hoje e CARACTERIZACAO (mede e compara). Apos a expansao de lanes, a config
 *   de poucas lanes (com max_shards alto) deve auto-crescer e fechar a lacuna.
 * ========================================================================= */

#define T8_PRODUCERS     8
#define T8_PER_THREAD    1000
#define T8_SLEEP_MS      1
#define T8_RING_CAP      256    /* ring pequeno p/ evidenciar saturacao */

typedef struct {
    ShardedPool*  pool;
    TpTaskArg*    args;
    int           count;
    tp_counter_t* done;
    tp_counter_t* failed;
} T8ThreadArg;

#ifdef _WIN32
static DWORD WINAPI t8_flood_fn(void* raw)
#else
static void* t8_flood_fn(void* raw)
#endif
{
    T8ThreadArg* ta = (T8ThreadArg*)raw;
    for (int i = 0; i < ta->count; i++) {
        TpTaskArg* t = &ta->args[i];
        t->submit_tsc = tsc_now();
        if (!pool_submit(ta->pool, task_cb_with_sleep, t)) {  /* sem intervalo: inunda */
            t->executed = -1;
            tp_counter_inc(ta->failed);
            tp_counter_inc(ta->done);
        }
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

typedef struct {
    double   wall_ms;
    int      executed;
    int      failed;
    int      backpressure;   /* esperas no submit (ficou lento) */
    int      expansions;     /* lanes ativadas dinamicamente    */
    int      final_lanes;    /* active_lanes ao final           */
    double   avg_us;
    double   max_us;
    double   throughput;     /* tasks executadas / s            */
} T8Result;

static void t8_run_pressure(const char* label, int shard_count, T8Result* out)
{
    int total = T8_PRODUCERS * T8_PER_THREAD;

    PoolConfig cfg     = pool_default_config();
    cfg.shard_count    = shard_count;
    cfg.ring_capacity  = T8_RING_CAP;

    ShardedPool* pool = pool_create(&cfg);
    TP_ASSERT(pool != NULL, "pool_create (pressao) != NULL");
    if (!pool) { memset(out, 0, sizeof(*out)); return; }

    TpTaskArg*    args    = (TpTaskArg*)   calloc(total,        sizeof(TpTaskArg));
    T8ThreadArg*  targs   = (T8ThreadArg*) calloc(T8_PRODUCERS, sizeof(T8ThreadArg));
    tp_thread_t*  handles = (tp_thread_t*) calloc(T8_PRODUCERS, sizeof(tp_thread_t));

    tp_counter_t done = 0, failed = 0;
    for (int i = 0; i < total; i++) {
        args[i].task_id      = i;
        args[i].sleep_ms     = T8_SLEEP_MS;
        args[i].done_counter = &done;
    }

    uint64_t wall_start = tp_get_ns();

    for (int p = 0; p < T8_PRODUCERS; p++) {
        targs[p].pool   = pool;
        targs[p].args   = &args[p * T8_PER_THREAD];
        targs[p].count  = T8_PER_THREAD;
        targs[p].done   = &done;
        targs[p].failed = &failed;
        tp_thread_start(handles[p], t8_flood_fn, &targs[p]);
    }

    int completed = tp_wait_done(&done, total, 60000);
    uint64_t wall_end = tp_get_ns();

    for (int p = 0; p < T8_PRODUCERS; p++) tp_thread_join(handles[p]);

    PoolStats st;
    pool_stats(pool, &st);
    pool_shutdown(pool);

    TP_ASSERT(completed, "pressao: todas contabilizadas (executadas + falhas) no tempo limite");

    TpLatency r = compute_latency(args, total);
    out->wall_ms      = (double)(wall_end - wall_start) / 1e6;
    out->executed     = r.executed;
    out->failed       = (int)st.submit_failures;
    out->backpressure = (int)st.submit_backpressure;
    out->expansions   = (int)st.total_expansions;
    out->final_lanes  = st.shard_count;   /* = active_lanes ao final */
    out->avg_us       = r.avg_ns / 1000.0;
    out->max_us       = r.max_ns / 1000.0;
    out->throughput   = out->wall_ms > 0 ? (double)r.executed / (out->wall_ms / 1000.0) : 0.0;

    printf("  [%s] inicio=%d → final=%d lanes  exec=%d falhas=%d  expans=%d backpressure=%d  "
           "wall=%.1fms  throughput=%.0f tasks/s\n",
           label, shard_count, out->final_lanes, r.executed, out->failed,
           out->expansions, out->backpressure, out->wall_ms, out->throughput);

    free(args); free(targs); free(handles);
}

static void test_pressure(void)
{
    printf("\n================================================================================\n");
    printf("  TESTE 8: Pressao sustentada (%d produtores x %d tasks de %dms, ring=%d)\n",
           T8_PRODUCERS, T8_PER_THREAD, T8_SLEEP_MS, T8_RING_CAP);
    printf("================================================================================\n\n");

    int nc = xcpu_count();
    if (nc < 4) nc = 4;

    T8Result few, many;
    printf("  -- poucas lanes iniciais (deve expandir sob pressao) --\n");
    t8_run_pressure("poucas", 2, &few);
    printf("  -- muitas lanes iniciais --\n");
    t8_run_pressure("muitas", nc, &many);

    /* Novo contrato: submit NUNCA falha — sob saturacao ele fica LENTO
     * (backpressure por espera) e/ou o pool EXPANDE lanes. Sinais robustos: */
    TP_ASSERT(few.failed == 0 && many.failed == 0,
              "submit nunca falha sob pressao (0 rejeicoes)");
    TP_ASSERT(few.expansions > 0,
              "poucas lanes EXPANDIRAM sob pressao (item 1)");
    TP_ASSERT(few.backpressure > 0 || few.expansions > 0,
              "sob saturacao o submit ficou lento ou expandiu (nao falhou)");
    TP_ASSERT(few.final_lanes > 2,
              "config de 2 lanes cresceu o nº de lanes ativas");
    TP_ASSERT(few.final_lanes <= xcpu_count(),
              "expansao automatica nao passou do nº de cores (cap em cores)");

    printf("\n  poucas: %d→%d lanes, expans=%d, backpressure=%d, throughput=%.0f, falhas=%d\n",
           2, few.final_lanes, few.expansions, few.backpressure, few.throughput, few.failed);
    printf("  muitas: %d→%d lanes, expans=%d, backpressure=%d, throughput=%.0f, falhas=%d\n",
           nc, many.final_lanes, many.expansions, many.backpressure, many.throughput, many.failed);
}

/* =========================================================================
 * TESTE 9: Contracao de lanes (item 4)
 *
 *   Expande sob rajada e, apos ficar ocioso, deve encolher de volta as lanes
 *   ativas em direcao ao nº inicial (parking/contracao). Valida o ciclo
 *   expandir → ocioso → contrair.
 * ========================================================================= */

#define T9_PRODUCERS   4
#define T9_PER_THREAD  500
#define T9_RING_CAP    256

static void test_contraction(void)
{
    printf("\n================================================================================\n");
    printf("  TESTE 9: Contracao de lanes (expande sob rajada, contrai ocioso)\n");
    printf("================================================================================\n\n");

    PoolConfig cfg    = pool_default_config();
    cfg.shard_count   = 2;
    cfg.ring_capacity = T9_RING_CAP;

    ShardedPool* pool = pool_create(&cfg);
    TP_ASSERT(pool != NULL, "pool_create != NULL");
    if (!pool) return;

    int total = T9_PRODUCERS * T9_PER_THREAD;
    TpTaskArg*   args    = (TpTaskArg*)   calloc(total,        sizeof(TpTaskArg));
    T8ThreadArg* targs   = (T8ThreadArg*) calloc(T9_PRODUCERS, sizeof(T8ThreadArg));
    tp_thread_t* handles = (tp_thread_t*) calloc(T9_PRODUCERS, sizeof(tp_thread_t));
    tp_counter_t done = 0, failed = 0;

    for (int i = 0; i < total; i++) {
        args[i].sleep_ms     = 1;
        args[i].done_counter = &done;
    }
    for (int p = 0; p < T9_PRODUCERS; p++) {
        targs[p].pool = pool; targs[p].args = &args[p * T9_PER_THREAD];
        targs[p].count = T9_PER_THREAD; targs[p].done = &done; targs[p].failed = &failed;
        tp_thread_start(handles[p], t8_flood_fn, &targs[p]);
    }
    for (int p = 0; p < T9_PRODUCERS; p++) tp_thread_join(handles[p]);

    PoolStats peak;
    pool_stats(pool, &peak);   /* logo apos a rajada: lanes expandidas */

    tp_wait_done(&done, total, 30000);

    /* ocioso o suficiente p/ a contracao (histerese ~500ms + 1 lane/scan) */
    tp_sleep_ms(2500);

    PoolStats after;
    pool_stats(pool, &after);

    pool_shutdown(pool);

    TP_ASSERT(peak.shard_count > 2, "expandiu sob rajada (lanes > inicial)");
    TP_ASSERT(after.shard_count < peak.shard_count, "contraiu apos ocioso (lanes < pico)");
    TP_ASSERT(after.shard_count >= 2, "nao contraiu abaixo do inicial (piso)");

    printf("  lanes: inicial=2  pico=%d  apos ocioso=%d  (expansoes=%llu)\n",
           peak.shard_count, after.shard_count,
           (unsigned long long)peak.total_expansions);

    free(args); free(targs); free(handles);
}

/* =========================================================================
 * TESTE 10: SOAK de concorrencia — cenarios aleatorios, execucao prolongada
 *
 *   Roda por <run_minutes> minutos (default 1; pensado p/ deixar 24h = 1440).
 *   A cada round sorteia um cenario realista (nº de lanes inicial, max_shards,
 *   ring, produtores, volume, mix de tasks curtas/longas, ociosidade ocasional)
 *   para exercitar expansao/contracao/resgate/handoff sob timings variados —
 *   o tipo de carga que revela CORRIDAS no resize dinamico de lanes.
 *
 *   ORACULO de invariantes (a cada round):
 *     - execucao EXATAMENTE UMA VEZ por task (ran==1: sem perda, sem duplicacao)
 *     - submit nunca falha (submit_failures==0)
 *     - sem deadlock (todas concluem dentro do timeout)
 *
 *   Logs:
 *     - a cada 10s: 1 linha SOBRESCRITA (\r), indicadores basicos (leve)
 *     - ao parar (tempo, ESC ou ERRO): relatorio detalhado + seed p/ reproduzir
 *
 *   Para parar antes do tempo: tecla ESC.
 * ========================================================================= */

#include <time.h>
#ifdef _WIN32
#include <conio.h>
static int soak_check_esc(void) { while (_kbhit()) { if (_getch() == 27) return 1; } return 0; }
#else
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
static int soak_check_esc(void) {
    static int inited = 0;
    if (!inited) {
        struct termios t; tcgetattr(STDIN_FILENO, &t);
        t.c_lflag &= ~(ICANON | ECHO); tcsetattr(STDIN_FILENO, TCSANOW, &t);
        int fl = fcntl(STDIN_FILENO, F_GETFL, 0); fcntl(STDIN_FILENO, F_SETFL, fl | O_NONBLOCK);
        inited = 1;
    }
    char c; while (read(STDIN_FILENO, &c, 1) == 1) { if (c == 27) return 1; }
    return 0;
}
#endif

typedef struct {
    tp_counter_t  ran;          /* nº de execucoes (1=ok, 0=perdida, >1=duplicada) */
    int           sleep_ms;
    uint64_t      submit_tsc;   /* set pelo produtor antes do submit */
    uint64_t      exec_tsc;     /* set pela task ao iniciar          */
    tp_counter_t* done;
} SoakArg;

static void soak_task(void* a) {
    SoakArg* s = (SoakArg*)a;
    s->exec_tsc = tsc_now();
    tp_counter_inc(&s->ran);                 /* deteccao de dupla-execucao */
    if (s->sleep_ms > 0) tp_sleep_ms(s->sleep_ms);
    tp_counter_inc(s->done);
}

typedef struct { ShardedPool* pool; SoakArg* args; int count; } SoakProd;

#ifdef _WIN32
static DWORD WINAPI soak_prod_fn(void* raw)
#else
static void* soak_prod_fn(void* raw)
#endif
{
    SoakProd* p = (SoakProd*)raw;
    for (int i = 0; i < p->count; i++) {
        p->args[i].submit_tsc = tsc_now();
        pool_submit(p->pool, soak_task, &p->args[i]);  /* nunca falha */
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

#define SOAK_WINDOW_SEC   10        /* cada rodada de coleta de indicadores */
#define SOAK_SLOW_MS      100       /* latencia submit->exec acima disto = "fora do esperado" */
#define SOAK_ROUND_TO_MS  60000     /* timeout por ciclo (deteccao de deadlock) */
#define SOAK_STRESS_CORES 2         /* prende o processo em poucos cores p/ forcar corridas */

/* Acumulador de indicadores (todos crus, explicitos). Usado para a janela de
 * 10s (zerado a cada rodada) e para o total acumulado (final). */
typedef struct {
    unsigned long long iters;        /* ciclos pool create/flood/drain */
    unsigned long long submitted;    /* tasks entregues ao pool_submit  */
    unsigned long long executed;     /* tasks que rodaram (ran>=1)      */
    unsigned long long lost;         /* ran==0 (perda)                  */
    unsigned long long doubled;      /* ran>1 (dupla execucao)          */
    unsigned long long subfail;      /* submit_failures reportadas       */
    unsigned long long timeouts;     /* ciclos que nao concluiram (deadlock) */
    unsigned long long expansions;   /* lanes ativadas dinamicamente     */
    unsigned long long backpressure; /* esperas no submit (fila cheia)   */
    unsigned long long slow;         /* tasks com latencia > SOAK_SLOW_MS */
    int                max_lat_ms;   /* maior latencia submit->exec       */
    int                max_backlog;  /* maior acumulo de fila nos shards  */
} SoakAcc;

static void soak_acc_zero(SoakAcc* a) { memset(a, 0, sizeof(*a)); }
static void soak_acc_add(SoakAcc* d, const SoakAcc* s) {
    d->iters+=s->iters; d->submitted+=s->submitted; d->executed+=s->executed;
    d->lost+=s->lost; d->doubled+=s->doubled; d->subfail+=s->subfail;
    d->timeouts+=s->timeouts; d->expansions+=s->expansions;
    d->backpressure+=s->backpressure; d->slow+=s->slow;
    if (s->max_lat_ms  > d->max_lat_ms)  d->max_lat_ms  = s->max_lat_ms;
    if (s->max_backlog > d->max_backlog) d->max_backlog = s->max_backlog;
}

/* Habilita escape ANSI (cursor) no console e detecta se a saida e um terminal. */
static int soak_tty = 0;
static void soak_init_term(void)
{
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode)) {       /* falha se redirecionado p/ arquivo */
        SetConsoleMode(h, mode | 0x0004); /* ENABLE_VIRTUAL_TERMINAL_PROCESSING */
        soak_tty = 1;
    }
#else
    soak_tty = isatty(STDOUT_FILENO);
#endif
}

/* ── Limiares criticos p/ o gradiente de cor (verde→amarelo→vermelho) ── */
#define SOAK_CRIT_LAT_MS   1000
#define SOAK_CRIT_BACKLOG  12000
#define SOAK_CRIT_BP       3000000.0
#define SOAK_CRIT_SLOW     100000.0

/* Cor por proximidade do critico (ratio 0..1): verde<0.5, amarelo<0.8, vermelho. */
static const char* soak_col(double ratio) {
    if (!soak_tty) return "";
    if (ratio < 0.5) return "\033[32m";
    if (ratio < 0.8) return "\033[33m";
    return "\033[31m";
}
/* Binario p/ indicadores de correcao: 0=verde, >0=vermelho. */
static const char* soak_col_bin(unsigned long long v) {
    if (!soak_tty) return "";
    return v ? "\033[31m" : "\033[32m";
}
/* Cor por severidade do evento da biblioteca. */
static const char* soak_col_sev(int sev) {
    if (!soak_tty) return "";
    return sev >= POOL_LOG_CRIT ? "\033[31m" : (sev == POOL_LOG_WARN ? "\033[33m" : "\033[32m");
}
static const char* soak_rst(void) { return soak_tty ? "\033[0m" : ""; }

/* Ultima mensagem da biblioteca: o hook (chamado por threads do pool) escreve,
 * o render (main) le. Protegido por lock. */
#ifdef _WIN32
static CRITICAL_SECTION soak_log_cs;
#define SOAK_LOG_INIT()   InitializeCriticalSection(&soak_log_cs)
#define SOAK_LOG_LOCK()   EnterCriticalSection(&soak_log_cs)
#define SOAK_LOG_UNLOCK() LeaveCriticalSection(&soak_log_cs)
#else
static pthread_mutex_t soak_log_cs = PTHREAD_MUTEX_INITIALIZER;
#define SOAK_LOG_INIT()   ((void)0)
#define SOAK_LOG_LOCK()   pthread_mutex_lock(&soak_log_cs)
#define SOAK_LOG_UNLOCK() pthread_mutex_unlock(&soak_log_cs)
#endif

static char soak_last_msg[200] = "(sem eventos)";
static int  soak_last_sev = POOL_LOG_INFO;

static void soak_log_hook(int sev, const char* msg) {
    SOAK_LOG_LOCK();
    soak_last_sev = sev;
    strncpy(soak_last_msg, msg, sizeof soak_last_msg - 1);
    soak_last_msg[sizeof soak_last_msg - 1] = 0;
    SOAK_LOG_UNLOCK();
}

/* Nº exato de linhas da tabela (deve casar com soak_render_table). */
#define SOAK_TABLE_LINES 20

/* Desenha a tabela. Em atualizacoes (first==0) sobe o cursor N linhas e
 * reescreve no mesmo lugar — so os valores mudam na tela, sem nova linha.
 * Cores: verde→amarelo→vermelho conforme cada valor se aproxima do critico. */
static void soak_render_table(int first, double el, int total_sec, unsigned seed,
                              const SoakAcc* w, const SoakAcc* t,
                              int last_shard, int last_final)
{
    int hh = (int)el / 3600, mm = ((int)el % 3600) / 60, ss = (int)el % 60;
    int th = total_sec / 3600, tm = (total_sec % 3600) / 60, ts = total_sec % 60;
    int nc = xcpu_count(); if (nc < 1) nc = 1;
    const char* R = soak_rst();
    if (!first) printf("\033[%dA", SOAK_TABLE_LINES);   /* sobe ate o topo da tabela */

    /* snapshot do ultimo evento sob lock */
    char ev[200]; int evsev;
    SOAK_LOG_LOCK(); evsev = soak_last_sev;
    strncpy(ev, soak_last_msg, sizeof ev - 1); ev[sizeof ev - 1] = 0; SOAK_LOG_UNLOCK();
    char evline[80]; snprintf(evline, sizeof evline, "evento: %s", ev);

    printf("  +-----------------------+------------------+---------------------+\033[K\n");
    printf("  | decorrido %02d:%02d:%02d / total %02d:%02d:%02d   seed=%-10u  lanes=%s%d->%-4d%s |\033[K\n",
           hh, mm, ss, th, tm, ts, seed, soak_col((double)last_final / nc), last_shard, last_final, R);
    printf("  +-----------------------+------------------+---------------------+\033[K\n");
    printf("  | indicador             |    janela 10s    |      acumulado      |\033[K\n");
    printf("  +-----------------------+------------------+---------------------+\033[K\n");
    printf("  | ciclos                | %16llu | %19llu |\033[K\n", w->iters,     t->iters);
    printf("  | submetidas            | %16llu | %19llu |\033[K\n", w->submitted, t->submitted);
    printf("  | executadas            | %16llu | %19llu |\033[K\n", w->executed,  t->executed);
    printf("  | perdidas (ran=0)      | %s%16llu%s | %s%19llu%s |\033[K\n", soak_col_bin(w->lost),    w->lost,    R, soak_col_bin(t->lost),    t->lost,    R);
    printf("  | duplicadas (ran>1)    | %s%16llu%s | %s%19llu%s |\033[K\n", soak_col_bin(w->doubled), w->doubled, R, soak_col_bin(t->doubled), t->doubled, R);
    printf("  | falhas de submit      | %s%16llu%s | %s%19llu%s |\033[K\n", soak_col_bin(w->subfail), w->subfail, R, soak_col_bin(t->subfail), t->subfail, R);
    printf("  | deadlocks             | %s%16llu%s | %s%19llu%s |\033[K\n", soak_col_bin(w->timeouts), w->timeouts, R, soak_col_bin(t->timeouts), t->timeouts, R);
    printf("  | expansoes de lane     | %16llu | %19llu |\033[K\n", w->expansions, t->expansions);
    printf("  | esperas submit (bp)   | %s%16llu%s | %19llu |\033[K\n", soak_col((double)w->backpressure / SOAK_CRIT_BP), w->backpressure, R, t->backpressure);
    printf("  | lentas (>%4dms)       | %s%16llu%s | %19llu |\033[K\n", SOAK_SLOW_MS, soak_col((double)w->slow / SOAK_CRIT_SLOW), w->slow, R, t->slow);
    printf("  | latencia max (ms)     | %s%16d%s | %s%19d%s |\033[K\n", soak_col((double)w->max_lat_ms / SOAK_CRIT_LAT_MS), w->max_lat_ms, R, soak_col((double)t->max_lat_ms / SOAK_CRIT_LAT_MS), t->max_lat_ms, R);
    printf("  | fila max (backlog)    | %s%16d%s | %s%19d%s |\033[K\n", soak_col((double)w->max_backlog / SOAK_CRIT_BACKLOG), w->max_backlog, R, soak_col((double)t->max_backlog / SOAK_CRIT_BACKLOG), t->max_backlog, R);
    printf("  +-----------------------+------------------+---------------------+\033[K\n");
    printf("  | %s%-62.62s%s |\033[K\n", soak_col_sev(evsev), evline, R);
    printf("  +-----------------------+------------------+---------------------+\033[K\n");
    fflush(stdout);
}

/* ── Captura de crash: grava cenario atual + stack no proprio log do soak ──
 * Reusa print_stacktrace() da lib (event_handler.c). No Windows usa
 * SetUnhandledExceptionFilter (robusto p/ access violation em qualquer thread);
 * no POSIX, signal(SIGSEGV/SIGABRT). */
extern void print_stacktrace(FILE* f);
#ifdef _WIN32
#pragma comment(lib, "dbghelp.lib")
#endif

static FILE* g_soak_logf = NULL;                         /* mesmo arquivo do heartbeat */
static char  g_soak_scenario[256] = "(nenhum ciclo ainda)";

static void soak_crash_dump(const char* what)
{
    FILE* f = g_soak_logf ? g_soak_logf : stderr;
    fprintf(f, "\n=== CRASH DETECTADO (%s) ===\n", what);
    fprintf(f, "cenario no momento  : %s\n", g_soak_scenario);
    print_stacktrace(f);
    fflush(f);
}

#ifdef _WIN32
static LONG WINAPI soak_crash_filter(EXCEPTION_POINTERS* ep)
{
    char b[80];
    snprintf(b, sizeof b, "exc=0x%08lx addr=%p",
             (unsigned long)ep->ExceptionRecord->ExceptionCode,
             ep->ExceptionRecord->ExceptionAddress);
    soak_crash_dump(b);
    return EXCEPTION_EXECUTE_HANDLER;   /* encerra o processo apos o dump */
}
static void soak_install_crash_handler(void) { SetUnhandledExceptionFilter(soak_crash_filter); }
static void soak_remove_crash_handler(void)  { SetUnhandledExceptionFilter(NULL); }
#else
#include <signal.h>
static void soak_crash_sig(int sig) {
    char b[32]; snprintf(b, sizeof b, "signal %d", sig);
    soak_crash_dump(b); _exit(1);
}
static void soak_install_crash_handler(void) { signal(SIGSEGV, soak_crash_sig); signal(SIGABRT, soak_crash_sig); }
static void soak_remove_crash_handler(void)  { signal(SIGSEGV, SIG_DFL); signal(SIGABRT, SIG_DFL); }
#endif

static void test_soak_concurrency_random(int run_minutes)
{
    printf("\n================================================================================\n");
    printf("  TESTE 10: SOAK de concorrencia — cenarios aleatorios (%d min). ESC para parar.\n",
           run_minutes);
    printf("  Rodada de coleta = %d s. Indicadores explicitos (crus), tabela in-place.\n", SOAK_WINDOW_SEC);
    printf("================================================================================\n\n");

    soak_init_term();
    SOAK_LOG_INIT();
    pool_set_log_hook(soak_log_hook);   /* a biblioteca passa a entregar eventos aqui */

    unsigned seed = (unsigned)time(NULL);
    srand(seed);

    uint64_t start    = tp_get_ns();
    uint64_t deadline = start + (uint64_t)run_minutes * 60ULL * 1000000000ULL;
    uint64_t win_t0   = start;
    int      total_sec = run_minutes * 60;

    /* log de arquivo (heartbeat 10s + relatorio/erro + dump de crash, flushed).
     * Nome com PID p/ rodar varias instancias em paralelo (#6) sem colisao. */
    char logname[64];
#ifdef _WIN32
    snprintf(logname, sizeof logname, "soak_log_%lu.txt", (unsigned long)GetCurrentProcessId());
#else
    snprintf(logname, sizeof logname, "soak_log_%d.txt", (int)getpid());
#endif
    g_soak_logf = fopen(logname, "w");
    printf("  [log] %s\n", logname);
    if (g_soak_logf) { fprintf(g_soak_logf, "SOAK seed=%u total=%d min\n", seed, run_minutes); fflush(g_soak_logf); }
    soak_install_crash_handler();   /* crash → grava cenario + stack neste log */

    /* #1 (estresse): prende em poucos cores → preempcao agressiva, alarga as
     * janelas de corrida do escalonador SEM tocar o thread_pool.c. */
#ifdef _WIN32
    {
        int ncpu = xcpu_count();
        int usec = (ncpu >= 2) ? SOAK_STRESS_CORES : 1;
        if (usec > ncpu) usec = ncpu;
        DWORD_PTR mask = ((DWORD_PTR)1 << usec) - 1;
        SetProcessAffinityMask(GetCurrentProcess(), mask);
        printf("  [stress] afinidade limitada a %d core(s) p/ forcar corridas\n", usec);
        if (g_soak_logf) { fprintf(g_soak_logf, "[stress] afinidade=%d cores\n", usec); fflush(g_soak_logf); }
    }
#endif

    SoakAcc tot;  soak_acc_zero(&tot);
    SoakAcc win;  soak_acc_zero(&win);
    int  fk_base = pool_force_kill_count();   /* baseline p/ detectar force-kill deste run */
    int  errors = 0, stopped_esc = 0, table_drawn = 0;
    int  last_shard = 0, last_final = 0;
    char errmsg[256]; errmsg[0] = 0;

    while (tp_get_ns() < deadline) {
        if (soak_check_esc()) { stopped_esc = 1; break; }

        /* --- cenario aleatorio AGRESSIVO (forca lifecycle/resize) --- */
        int shard     = 1 + rand() % 4;
        int maxsh     = shard + 1 + rand() % 16;
        int ring      = 32 << (rand() % 5);     /* 32..512: ring pequeno → mais expansao/backpressure */
        int producers = 1 + rand() % 6;
        int per       = 10 + rand() % 300;      /* #3: ciclos curtos → MUITO churn de create/destroy */
        int total     = producers * per;
        int inflight  = (rand() % 5 == 0);                /* #5: ~20% destroi com tasks em voo */
        int do_idle   = (!inflight && rand() % 4 == 0);   /* #4: idle > histerese → contracao */

        snprintf(g_soak_scenario, sizeof g_soak_scenario,
                 "ciclo=%llu shard=%d max=%d ring=%d producers=%d total=%d inflight=%d idle=%d",
                 (unsigned long long)(tot.iters + 1), shard, maxsh, ring, producers, total, inflight, do_idle);

        PoolConfig cfg = pool_default_config();
        cfg.shard_count   = shard;
        cfg.max_shards    = maxsh;
        cfg.ring_capacity = ring;

        ShardedPool* pool = pool_create(&cfg);
        if (!pool) { snprintf(errmsg, sizeof errmsg, "pool_create falhou (shard=%d)", shard); errors++; break; }

        SoakArg*     args  = (SoakArg*)    calloc(total,     sizeof(SoakArg));
        SoakProd*    prods = (SoakProd*)   calloc(producers, sizeof(SoakProd));
        tp_thread_t* hs    = (tp_thread_t*)calloc(producers, sizeof(tp_thread_t));
        tp_counter_t done  = 0;

        for (int i = 0; i < total; i++) {
            int longish = (rand() % 50 == 0);   /* ~2% tasks longas */
            args[i].sleep_ms = longish ? (20 + rand() % 40) : (rand() % 3);
            args[i].done     = &done;
        }
        for (int p = 0; p < producers; p++) {
            prods[p].pool = pool; prods[p].args = &args[p * per]; prods[p].count = per;
            tp_thread_start(hs[p], soak_prod_fn, &prods[p]);
        }
        for (int p = 0; p < producers; p++) tp_thread_join(hs[p]);  /* todas submetidas */

        int round_maxq = 0, ok = 1;
        if (inflight) {
            /* #5: destrói SEM drenar → workers ocupados quando o shutdown chega
             * (estressa a corrida shutdown vs worker ativo). Nao checa perda. */
            PoolStats s; pool_stats(pool, &s); round_maxq = s.pending_tasks;
        } else {
            int elapsed = 0;
            while (tp_counter_read(&done) < total) {
                PoolStats s; pool_stats(pool, &s);
                if (s.pending_tasks > round_maxq) round_maxq = s.pending_tasks;
                if (elapsed >= SOAK_ROUND_TO_MS) { ok = 0; break; }
                tp_sleep_ms(1); elapsed += 1;
            }
            if (do_idle) tp_sleep_ms(550 + rand() % 500);  /* > histerese → expande→contrai */
        }

        PoolStats st; pool_stats(pool, &st);
        last_shard = shard; last_final = st.shard_count;

        pool_shutdown(pool);

        /* Force-kill no shutdown → pool envenenado: PARA DE RECICLAR (decisao b).
         * Continuar criaria pools sobre heap possivelmente corrompido. */
        if (pool_force_kill_count() > fk_base) {
            snprintf(errmsg, sizeof errmsg,
                     "FORCE-KILL: %d thread(s) terminada(s) a forca — parando de reciclar [%s]",
                     pool_force_kill_count() - fk_base, g_soak_scenario);
            errors++;
            break;
        }

        /* --- indicadores do ciclo (crus) --- */
        SoakAcc r; soak_acc_zero(&r);
        r.iters        = 1;
        r.submitted    = (unsigned long long)total;
        r.subfail      = st.submit_failures;
        r.expansions   = st.total_expansions;
        r.backpressure = st.submit_backpressure;
        r.max_backlog  = round_maxq;
        if (!ok) r.timeouts = 1;

        for (int i = 0; i < total; i++) {
            int n = (int)tp_counter_read(&args[i].ran);
            if (n >= 1) {
                r.executed++;
                if (n > 1) r.doubled++;                 /* dupla execucao = sempre bug */
                if (args[i].exec_tsc && args[i].submit_tsc) {
                    double ms = tsc_elapsed_ns(args[i].submit_tsc, args[i].exec_tsc) / 1e6;
                    if ((int)ms > r.max_lat_ms) r.max_lat_ms = (int)ms;
                    if (ms > SOAK_SLOW_MS) r.slow++;
                }
            } else if (!inflight) {
                r.lost++;                               /* perda só conta se o ciclo drenou */
            }
        }

        soak_acc_add(&tot, &r);
        soak_acc_add(&win, &r);

        /* --- oraculo (qualquer violacao para o soak); em inflight nao checa perda/deadlock --- */
        if (!inflight && r.timeouts) { snprintf(errmsg, sizeof errmsg,
            "DEADLOCK: done=%d/%d [shard=%d max=%d ring=%d prod=%d]",
            (int)tp_counter_read(&done), total, shard, maxsh, ring, producers); errors++; }
        else if (r.subfail) { snprintf(errmsg, sizeof errmsg,
            "submit_failures=%llu (deveria ser 0)", r.subfail); errors++; }
        else if (!inflight && r.lost) { snprintf(errmsg, sizeof errmsg,
            "PERDA: %llu tasks com ran==0 [shard=%d max=%d ring=%d prod=%d]", r.lost, shard, maxsh, ring, producers); errors++; }
        else if (r.doubled) { snprintf(errmsg, sizeof errmsg,
            "DUPLA EXEC: %llu tasks com ran>1 [shard=%d max=%d ring=%d prod=%d]", r.doubled, shard, maxsh, ring, producers); errors++; }

        free(args); free(prods); free(hs);

        if (errors) break;

        /* --- rodada de coleta a cada 10s: TABELA (console) + heartbeat (arquivo) --- */
        uint64_t now = tp_get_ns();
        if (now - win_t0 >= (uint64_t)SOAK_WINDOW_SEC * 1000000000ULL) {
            double wel = (double)(now - start) / 1e9;
            int eh = (int)wel / 3600, em = ((int)wel % 3600) / 60, es = (int)wel % 60;
            int th = total_sec / 3600, tm = (total_sec % 3600) / 60, ts = total_sec % 60;

            char ev[200]; SOAK_LOG_LOCK();
            strncpy(ev, soak_last_msg, sizeof ev - 1); ev[sizeof ev - 1] = 0; SOAK_LOG_UNLOCK();

            char wline[400];
            snprintf(wline, sizeof wline,
                "[%02d:%02d:%02d/%02d:%02d:%02d] iters=%llu sub=%llu exec=%llu lost=%llu dup=%llu "
                "subfail=%llu deadlock=%llu exp=%llu bp=%llu slow=%llu maxlat=%dms maxfila=%d lanes=%d->%d | evento: %s",
                eh, em, es, th, tm, ts, win.iters, win.submitted, win.executed, win.lost, win.doubled,
                win.subfail, win.timeouts, win.expansions, win.backpressure, win.slow,
                win.max_lat_ms, win.max_backlog, last_shard, last_final, ev);

            /* arquivo: grava e da flush a cada atualizacao (sobrevive a crash) */
            if (g_soak_logf) { fprintf(g_soak_logf, "%s\n", wline); fflush(g_soak_logf); }

            /* console: tabela colorida in-place (tty) ou a mesma linha (redirecionado) */
            if (soak_tty) {
                soak_render_table(!table_drawn, wel, total_sec, seed, &win, &tot, last_shard, last_final);
                table_drawn = 1;
            } else {
                printf("%s\n", wline); fflush(stdout);
            }

            soak_acc_zero(&win);
            win_t0 = now;
        }
    }
    if (soak_tty && table_drawn) printf("\n");  /* sai de baixo da tabela */
    pool_set_log_hook(NULL);                    /* remove o hook ao terminar */
#ifdef _WIN32
    /* restaura a afinidade (nao vazar o pin de 2 cores p/ os testes seguintes) */
    { DWORD_PTR pm, sm; if (GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm)) SetProcessAffinityMask(GetCurrentProcess(), sm); }
#endif

    /* --- relatorio detalhado ao parar: para tela E arquivo (mantem log de erro) --- */
    double el = (double)(tp_get_ns() - start) / 1e9;
    #define REP(...) do { printf(__VA_ARGS__); if (g_soak_logf) fprintf(g_soak_logf, __VA_ARGS__); } while (0)
    REP("\n  --- SOAK: relatorio detalhado (acumulado, indicadores crus) ---\n");
    REP("    parada               : %s\n", stopped_esc ? "ESC (usuario)" : (errors ? "ERRO de invariante" : "tempo esgotado"));
    REP("    seed RNG             : %u\n", seed);
    REP("    duracao (s)          : %.1f\n", el);
    REP("    ciclos (iters)       : %llu\n", tot.iters);
    REP("    tasks submetidas     : %llu\n", tot.submitted);
    REP("    tasks executadas     : %llu\n", tot.executed);
    REP("    tasks perdidas       : %llu   (esperado 0)\n", tot.lost);
    REP("    tasks duplicadas     : %llu   (esperado 0)\n", tot.doubled);
    REP("    falhas de submit     : %llu   (esperado 0)\n", tot.subfail);
    REP("    ciclos com deadlock  : %llu   (esperado 0)\n", tot.timeouts);
    REP("    expansoes de lane    : %llu\n", tot.expansions);
    REP("    esperas de submit(bp): %llu\n", tot.backpressure);
    REP("    tasks lentas(>%dms)  : %llu\n", SOAK_SLOW_MS, tot.slow);
    REP("    latencia maxima (ms) : %d\n", tot.max_lat_ms);
    REP("    fila maxima (backlog): %d\n", tot.max_backlog);
    if (errors) REP("    >>> FALHA: %s\n", errmsg);
    #undef REP
    soak_remove_crash_handler();
    if (g_soak_logf) { fflush(g_soak_logf); fclose(g_soak_logf); g_soak_logf = NULL; }

    TP_ASSERT(tot.lost == 0,     "soak: nenhuma task perdida (ran==0)");
    TP_ASSERT(tot.doubled == 0,  "soak: nenhuma task duplicada (ran>1)");
    TP_ASSERT(tot.subfail == 0,  "soak: nenhuma falha de submit");
    TP_ASSERT(tot.timeouts == 0, "soak: nenhum deadlock (todos os ciclos concluiram)");
}

/* =========================================================================
 * TESTE 11: Shutdown com tasks TRAVADAS (escalonamento → force-kill)
 *
 *   Submete tasks que ficam presas num busy-loop infinito (sem malloc/lock, p/
 *   o TerminateThread ser seguro). pool_shutdown deve: drenar (timeout) →
 *   flag (timeout) → terminar as threads a forca, e RETORNAR em tempo limitado.
 *
 *   Afericao de que "realmente pararam": cada task travada incrementa um
 *   heartbeat (prova de vida). Apos o shutdown, o heartbeat deve CONGELAR
 *   (thread morta nao incrementa mais). Tambem confere force_kill_count.
 * ========================================================================= */

#define T11_STUCK   3
#define T11_NORMAL  4

typedef struct { volatile long heartbeat; volatile int go; } StuckArg;

static void stuck_task(void* a) {
    StuckArg* s = (StuckArg*)a;
    while (s->go) { s->heartbeat++; }   /* preso: busy-loop, sem alocacao/lock */
}

static void test_shutdown_stuck(void)
{
    printf("\n================================================================================\n");
    printf("  TESTE 11: Shutdown com %d tasks travadas (escalonamento → force-kill)\n", T11_STUCK);
    printf("================================================================================\n\n");

    /* Garante TODOS os cores (o soak pode ter limitado a afinidade antes; com
     * busy-loops presos em poucos cores, a thread do shutdown ficaria starved). */
#ifdef _WIN32
    { DWORD_PTR pm, sm; if (GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm)) SetProcessAffinityMask(GetCurrentProcess(), sm); }
#endif

    PoolConfig cfg = pool_default_config();
    cfg.shard_count = 4;
    cfg.shutdown_drain_timeout_ms = 1000;   /* encurta p/ o teste ser rapido */
    cfg.shutdown_join_timeout_ms  = 500;

    ShardedPool* pool = pool_create(&cfg);
    TP_ASSERT(pool != NULL, "pool_create != NULL");
    if (!pool) return;

    int fk_before = pool_force_kill_count();

    /* 1) tasks normais — devem rodar e concluir antes do shutdown */
    tp_counter_t ndone = 0;
    TpTaskArg normal[T11_NORMAL]; memset(normal, 0, sizeof normal);
    for (int i = 0; i < T11_NORMAL; i++) {
        normal[i].done_counter = &ndone;
        normal[i].submit_tsc   = tsc_now();
        pool_submit(pool, task_cb_plain, &normal[i]);
    }
    TP_ASSERT(tp_wait_done(&ndone, T11_NORMAL, 3000), "tasks normais executaram (pool funcional)");

    /* 2) tasks travadas (busy-loop infinito) */
    StuckArg stuck[T11_STUCK];
    for (int i = 0; i < T11_STUCK; i++) { stuck[i].heartbeat = 0; stuck[i].go = 1; }
    for (int i = 0; i < T11_STUCK; i++) pool_submit(pool, stuck_task, &stuck[i]);
    tp_sleep_ms(200);   /* deixa pegarem e ficarem presas */

    int alive = 0;
    for (int i = 0; i < T11_STUCK; i++) if (stuck[i].heartbeat > 0) alive++;
    TP_ASSERT(alive == T11_STUCK, "tasks travadas estao rodando (heartbeat > 0)");

    /* 3) shutdown: deve forcar e RETORNAR em tempo limitado (nao travar) */
    uint64_t t0 = tp_get_ns();
    pool_shutdown(pool);
    double sh_ms = (double)(tp_get_ns() - t0) / 1e6;
    int killed = pool_force_kill_count() - fk_before;

    /* 4) aferir que pararam: heartbeat congela apos o shutdown */
    long hb1[T11_STUCK];
    for (int i = 0; i < T11_STUCK; i++) hb1[i] = stuck[i].heartbeat;
    tp_sleep_ms(300);
    int frozen = 0;
    for (int i = 0; i < T11_STUCK; i++) if (stuck[i].heartbeat == hb1[i]) frozen++;

    double limit = (double)(cfg.shutdown_drain_timeout_ms + cfg.shutdown_join_timeout_ms + 2000);
    TP_ASSERT(sh_ms < limit,        "pool_shutdown retornou em tempo limitado (nao travou)");
    TP_ASSERT(killed >= T11_STUCK,  "as tasks travadas foram terminadas a forca (force-kill)");
    TP_ASSERT(frozen == T11_STUCK,  "tasks travadas REALMENTE pararam (heartbeat congelado)");

    printf("  shutdown=%.0f ms (limite %.0f)  force_killed=%d  congeladas=%d/%d\n",
           sh_ms, limit, killed, frozen, T11_STUCK);
    for (int i = 0; i < T11_STUCK; i++)
        printf("    stuck[%d] heartbeat=%ld  congelou=%s\n",
               i, stuck[i].heartbeat, (stuck[i].heartbeat == hb1[i]) ? "sim" : "NAO");

    /* Pool envenenado (force-kill): NAO criar mais pools nem liberar nada apos. */
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

    CpuMonitor cpu_mon;
    cpu_monitor_start(&cpu_mon, 10);

    //test_basic();
    //test_single_thread_perf();
    //test_multi_thread_perf(T3_THREAD_COUNT);
    test_handoff();
    //test_rescue();
    //test_rescue_no_overfire();
    //test_rescue_engage_latency();
    //test_pressure();
    //test_contraction();
    //test_soak_concurrency_random(1);
    //test_shutdown_stuck();   /* por ultimo: force-kill deixa o processo "envenenado" */

    cpu_monitor_stop(&cpu_mon);

    printf("\n================================================================================\n");
    printf("  RESULTADO FINAL: %d / %d asserts passaram\n", g_tests_passed, g_tests_run);
    cpu_monitor_print(&cpu_mon);
    printf("================================================================================\n\n");

    //int vc = getch();
}


#ifdef POOL_TEST_HOOKS
extern int thread_pool_bug_test_dispatch(int argc, char** argv);
#endif

/* main DESABILITADO: o entrypoint ativo do projeto agora e o de
 * pool_compare_bench.cpp (benchmark comparativo). Para voltar a rodar a suite de
 * testes, troque este #if 0 por #if 1 e comente/remova o main de
 * pool_compare_bench.cpp (ou tire o arquivo do projeto). */
#if 0
int main(int argc, char** argv)
{
#ifdef POOL_TEST_HOOKS
    if (argc > 1) return thread_pool_bug_test_dispatch(argc, argv);
#else
    (void)argc; (void)argv;
#endif
    thread_pool_test_run();
    return 0;
}
#endif