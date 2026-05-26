/*
 * test_task_handler.c
 * Comparativo: ShardedPool (pool_submit) vs Windows Thread Pool API
 *
 * Metricas por execucao:
 *   avg_global     : media simples de todos os tempos de execucao
 *   min_us/max_us  : tempo minimo e maximo observados
 *   stddev_us      : desvio padrao
 *
 * Indicador de 3 regioes (cruzamento com curva normal -- limites +-1 sigma):
 *   Regiao minimos : tempos < avg-sigma  (cauda esquerda da normal)
 *   Regiao estavel : tempos em [avg-sigma, avg+sigma]  (~68% em dist. normal)
 *   Regiao maximos : tempos > avg+sigma  (cauda direita da normal)
 *   Para cada regiao: media + percentual do total de tasks.
 *
 * Historico unificado (1 entrada por segundo + eventos para todos os testes):
 *   - EV_SAMPLE         : captura periodica
 *   - EV_SHARD_ADDED    : novo shard adicionado ao pool
 *   - EV_ALL_QUEUES_FULL: todas as filas ficaram cheias simultaneamente
 *   Primeira coluna = momento (ms desde inicio do teste) da captura.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include "../Xplatbase/Xplatbase/src/task_handler.h"

#define MAX_SHARD_COUNT POOL_MAX_SHARDS

#ifdef TBB_AVAILABLE
extern void run_tbb_test(void* args, void* handles, int workers, int total_tasks, void* log_file, uint64_t test_start_ns);
#endif

/* --- contador atomico local --- */

#ifdef _WIN32
    typedef volatile LONG tth_counter_t;
    #define tth_counter_inc(p)   InterlockedIncrement(p)
    #define tth_counter_read(p)  (*(p))
#else
    #include <stdatomic.h>
    typedef volatile int tth_counter_t;
    #define tth_counter_inc(p)   __sync_add_and_fetch((p), 1)
    #define tth_counter_read(p)  (*(p))
#endif

/* --------------------------------- timing --------------------------------- */

static double   tsc_ns_ratio = 1.0;
static uint64_t tsc_overhead = 0;

static void tsc_calibrate(void)
{
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

    uint64_t min_oh = UINT64_MAX;
    for (int i = 0; i < 1000; i++)
    {
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

static uint64_t tth_get_ns(void)
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

static void tth_sleep_ms(int ms)
{
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
#endif
}

static void tth_sleep_us(int64_t us)
{
#ifdef _WIN32
    HANDLE timer = CreateWaitableTimer(NULL, TRUE, NULL);
    if (timer)
    {
        LARGE_INTEGER li;
        // WaitableTimer usa unidades de 100ns
        li.QuadPart = -(us * 10);

        SetWaitableTimer(timer, &li, 0, NULL, NULL, FALSE);
        WaitForSingleObject(timer, INFINITE);
        CloseHandle(timer);
    }
#else
    struct timespec ts;
    ts.tv_sec = us / 1000000;
    ts.tv_nsec = (us % 1000000) * 1000;
    nanosleep(&ts, NULL);
#endif
}

static void tth_sleep_ns(int64_t ns)
{
#ifdef _WIN32
    HANDLE timer = CreateWaitableTimer(NULL, TRUE, NULL);
    if (timer)
    {
        LARGE_INTEGER li;

        // Windows trabalha em 100ns, ent�o arredonda para cima
        int64_t units_100ns = (ns + 99) / 100;
        li.QuadPart = -units_100ns;

        SetWaitableTimer(timer, &li, 0, NULL, NULL, FALSE);
        WaitForSingleObject(timer, INFINITE);
        CloseHandle(timer);
    }
#else
    struct timespec ts;
    ts.tv_sec = ns / 1000000000LL;
    ts.tv_nsec = ns % 1000000000LL;
    nanosleep(&ts, NULL);
#endif
}


/* ----------------------------- argumento de task -------------------------- */

typedef struct
{
    int              task_id;
    int              sleep_ms;
    volatile int     executed;          /* 0=pendente, 1=ok, -1=submit fail */
    uint64_t         start_ns;
    uint64_t         end_ns;
    uint64_t         exec_duration_ns;
    tth_counter_t*   done_counter;
#ifdef _WIN32
    PTP_WORK         work_handle;
#endif
} TaskArg;

/* -------------------------------- callbacks ------------------------------- */

static void task_fn(void* arg)
{
    TaskArg* t = (TaskArg*)arg;
    t->end_ns           = tsc_now();
    t->exec_duration_ns = (uint64_t)tsc_elapsed_ns(t->start_ns, t->end_ns);
    t->executed         = 1;
    tth_counter_inc(t->done_counter);

    tth_sleep_ms(1);
}

#ifdef _WIN32
static void CALLBACK wintp_task_fn(PTP_CALLBACK_INSTANCE instance,
                                   PVOID context, PTP_WORK work)
{
    (void)instance; (void)work;
    TaskArg* t = (TaskArg*)context;
    t->end_ns           = tsc_now();
    t->exec_duration_ns = (uint64_t)tsc_elapsed_ns(t->start_ns, t->end_ns);
    t->executed         = 1;
    tth_counter_inc(t->done_counter);
}
#endif

/* ------------------------------- configuracoes ---------------------------- */

#define TEST_TASK_COUNT  1000
#define TEST_THREADS     100

static int worker_configs[]     = { 512 };
static int worker_configs_count = (int)(sizeof(worker_configs) / sizeof(worker_configs[0]));

/* -------------------- submit threads -- ShardedPool ----------------------- */

typedef struct
{
    ShardedPool*    pool;
    TaskArg*        args;
    tth_counter_t*  done_counter;
    tth_counter_t*  submit_failed;
    int             thread_idx;
} SubmitThreadArg;

#ifdef _WIN32
    static DWORD WINAPI submit_thread_fn(void* raw)
    {
        SubmitThreadArg* sta = (SubmitThreadArg*)raw;
        for (int i = 0; i < TEST_TASK_COUNT; i++)
        {
            TaskArg* t  = &sta->args[i];
            t->sleep_ms = rand() % 10;
            tth_sleep_us(t->sleep_ms);
            t->start_ns = tsc_now();
            if (!pool_submit(sta->pool, task_fn, t))
            {   
                t->executed = -1;
                tth_counter_inc(sta->submit_failed);
                tth_counter_inc(sta->done_counter);
            }
        }
        return 0;
    }

    typedef HANDLE tth_thread_t;
    #define tth_thread_start(h, fn, arg) ((h) = CreateThread(NULL, 0, (fn), (arg), 0, NULL))
    #define tth_thread_join(h)           do { WaitForSingleObject((h), INFINITE); CloseHandle(h); } while (0)
#else
    #include <pthread.h>

    static void* submit_thread_fn(void* raw)
    {
        SubmitThreadArg* sta = (SubmitThreadArg*)raw;
        unsigned seed = (unsigned)(uintptr_t)raw;
        for (int i = 0; i < TEST_TASK_COUNT; i++)
        {
            TaskArg* t  = &sta->args[i];
            t->sleep_ms = rand_r(&seed) % 10;
            tth_sleep_ms(t->sleep_ms);
            t->start_ns = tsc_now();
            if (!pool_submit(sta->pool, task_fn, t))
            {
                t->executed = -1;
                tth_counter_inc(sta->submit_failed);
                tth_counter_inc(sta->done_counter);
            }
        }
        return NULL;
    }

    typedef pthread_t tth_thread_t;
    #define tth_thread_start(h, fn, arg) pthread_create(&(h), NULL, (fn), (arg))
    #define tth_thread_join(h)           pthread_join((h), NULL)
#endif

/* -------------------- submit threads -- Windows Thread Pool --------------- */

#ifdef _WIN32

typedef struct
{
    TP_CALLBACK_ENVIRON* tpEnv;
    TaskArg*             args;
    tth_counter_t*       done_counter;
    tth_counter_t*       submit_failed;
} WinTPSubmitArg;

static DWORD WINAPI wintp_submit_thread_fn(void* raw)
{
    WinTPSubmitArg* sta = (WinTPSubmitArg*)raw;
    for (int i = 0; i < TEST_TASK_COUNT; i++)
    {
        TaskArg* t  = &sta->args[i];
        t->sleep_ms = rand() % 10;
        tth_sleep_ms(t->sleep_ms);
        t->start_ns = tsc_now();

        PTP_WORK work = CreateThreadpoolWork(wintp_task_fn, t, sta->tpEnv);
        if (!work)
        {
            t->executed = -1;
            tth_counter_inc(sta->submit_failed);
            tth_counter_inc(sta->done_counter);
        }
        else
        {
            t->work_handle = work;
            SubmitThreadpoolWork(work);
        }
    }
    return 0;
}

#endif /* _WIN32 */

/* ============================== INDICADORES =============================== */

/*
 * PerfIndicators
 *
 *   avg_global          : media de todos os tempos (us)
 *   min_us / max_us     : extremos
 *   stddev_us           : desvio padrao (sigma)
 *   boundary_low_us     : avg - sigma  (inicio da regiao estavel)
 *   boundary_high_us    : avg + sigma  (fim da regiao estavel)
 *
 *   Regioes (cruzamento com curva normal -- limites +-1 sigma):
 *     [minimos]  count_min  avg_min_us  pct_min   <- abaixo de avg-sigma
 *     [estavel]  count_stable  avg_stable_us  pct_stable  <- entre avg+-sigma
 *     [maximos]  count_max  avg_max_us  pct_max   <- acima de avg+sigma
 */
typedef struct
{
    double avg_global;
    double min_us;
    double max_us;
    double stddev_us;
    double boundary_low_us;
    double boundary_high_us;
    /* regiao minimos */
    int    count_min;
    double avg_min_us;
    double pct_min;
    /* regiao estavel */
    int    count_stable;
    double avg_stable_us;
    double pct_stable;
    /* regiao maximos */
    int    count_max;
    double avg_max_us;
    double pct_max;
} PerfIndicators;

static PerfIndicators compute_indicators(TaskArg* args, int total_tasks)
{
    PerfIndicators ind;
    memset(&ind, 0, sizeof(ind));

    int    n    = 0;
    double sum  = 0.0;
    double gmin = (double)UINT64_MAX, gmax = 0.0;

    for (int i = 0; i < total_tasks; i++)
    {
        if (args[i].executed == 1)
        {
            double v = (double)args[i].exec_duration_ns;
            sum += v;
            n++;
            if (v < gmin) gmin = v;
            if (v > gmax) gmax = v;
        }
    }
    if (n == 0) return ind;

    double mean = sum / (double)n;

    double var = 0.0;
    for (int i = 0; i < total_tasks; i++)
        if (args[i].executed == 1)
        {
            double d = (double)args[i].exec_duration_ns - mean;
            var += d * d;
        }
    double sigma = n > 1 ? sqrt(var / (double)n) : 0.0;

    double lo = mean - sigma;
    double hi = mean + sigma;

    double s_min = 0.0, s_mid = 0.0, s_max = 0.0;
    int    c_min = 0,   c_mid = 0,   c_max = 0;

    for (int i = 0; i < total_tasks; i++)
    {
        if (args[i].executed == 1)
        {
            double v = (double)args[i].exec_duration_ns;
            if      (v < lo) { s_min += v; c_min++; }
            else if (v > hi) { s_max += v; c_max++; }
            else             { s_mid += v; c_mid++; }
        }
    }

    ind.avg_global       = mean / 1000.0;
    ind.min_us           = gmin / 1000.0;
    ind.max_us           = gmax / 1000.0;
    ind.stddev_us        = sigma / 1000.0;
    ind.boundary_low_us  = lo / 1000.0;
    ind.boundary_high_us = hi / 1000.0;

    ind.count_min     = c_min;
    ind.avg_min_us    = c_min > 0 ? (s_min / (double)c_min) / 1000.0 : 0.0;
    ind.pct_min       = (double)c_min * 100.0 / (double)n;

    ind.count_stable  = c_mid;
    ind.avg_stable_us = c_mid > 0 ? (s_mid / (double)c_mid) / 1000.0 : 0.0;
    ind.pct_stable    = (double)c_mid * 100.0 / (double)n;

    ind.count_max     = c_max;
    ind.avg_max_us    = c_max > 0 ? (s_max / (double)c_max) / 1000.0 : 0.0;
    ind.pct_max       = (double)c_max * 100.0 / (double)n;

    return ind;
}

static void print_indicators(const PerfIndicators* ind)
{
    printf("       Media global   : %10.3f us\n", ind->avg_global);
    printf("       Min / Max      : %10.4f us  /  %10.4f us\n", ind->min_us, ind->max_us);
    printf("       StdDev (sigma) : %10.3f us  ->  limites: [%.4f .. %.4f] us\n",
           ind->stddev_us, ind->boundary_low_us, ind->boundary_high_us);
    printf("       Regiao minimos : %6d tasks (%5.1f%%)  avg = %10.3f us   (< avg-sigma)\n",
           ind->count_min, ind->pct_min, ind->avg_min_us);
    printf("       Regiao estavel : %6d tasks (%5.1f%%)  avg = %10.3f us   (avg +/- sigma)\n",
           ind->count_stable, ind->pct_stable, ind->avg_stable_us);
    printf("       Regiao maximos : %6d tasks (%5.1f%%)  avg = %10.3f us   (> avg+sigma)\n",
           ind->count_max, ind->pct_max, ind->avg_max_us);
}

/* ================================ HISTORICO ================================ */

/*
 * Historico unificado de todos os testes.
 * ts_ms e relativo ao inicio de test_task_handler_run.
 * Cada entrada identifica o impl (label) que a gerou.
 */

#define MAX_HISTORY 600

typedef enum
{
    EV_SAMPLE = 0,
    EV_SHARD_ADDED,
    EV_ALL_QUEUES_FULL
} EventKind;

static const char* ev_name[] = { "sample         ", "SHARD_ADDED    ", "ALL_QUEUES_FULL" };

typedef struct
{
    uint64_t       ts_ms;
    char           label[16];
    EventKind      kind;
    int            tasks_done;
    int            tasks_total;
    PerfIndicators perf;
    /* estado do pool (shard_count == 0 -> N/A) */
    int            shard_count;
    int            shard_occ[MAX_SHARD_COUNT];
    int            shard_cap;
} HistEntry;

static HistEntry    g_hist[MAX_HISTORY];
static int          g_hist_n        = 0;
static uint64_t     g_test_start_ns = 0;   /* definido em test_task_handler_run */
static FILE*        g_log_file      = NULL; /* arquivo TSV para importar no Excel */
static int          g_pool_verbose  = 0;   /* 0 = resumo "sc=N", 1 = completo "sc=N occ=[...]" */

static void hist_push(uint64_t ts_ms, const char* label, EventKind kind,
                      int done, int total, const PerfIndicators* ind,
                      int sc, int* occ, int cap)
{
    if (g_hist_n >= MAX_HISTORY) return;
    HistEntry* e  = &g_hist[g_hist_n++];
    e->ts_ms      = ts_ms;
    e->kind       = kind;
    e->tasks_done = done;
    e->tasks_total= total;
    e->perf       = *ind;
    e->shard_count= sc;
    e->shard_cap  = cap;
    strncpy(e->label, label ? label : "", sizeof(e->label) - 1);
    e->label[sizeof(e->label) - 1] = '\0';
    if (sc > 0 && occ)
        memcpy(e->shard_occ, occ, (size_t)sc * sizeof(int));
}

static void print_history_header(void)
{
    static const char* hdr =
        "t(ms)\timpl\tevento\tdone\tavg(us)\tmin(us)\tmax(us)"
        "\trg_min(us)\tmin%\tstable(us)\tstbl%\trg_max(us)\tmax%\tpool\n";

    printf("\n");
    printf("================================================================================\n");
    printf("  HISTORICO UNIFICADO (ao vivo)\n");
    printf("================================================================================\n\n");
    printf("%s", hdr);
    fflush(stdout);

    if (g_log_file)
    {
        fprintf(g_log_file, "%s", hdr);
        fflush(g_log_file);
    }
}

static void print_hist_row(const HistEntry* e)
{
    /* monta a parte fixa uma vez e envia para console e arquivo */
    char pool_str[MAX_SHARD_COUNT * 6 + 32];
    pool_str[0] = '\0';
    if (e->shard_count > 0)
    {
        if (g_pool_verbose)
        {
            int pos = 0;
            pos += sprintf(pool_str + pos, "sc=%d occ=[", e->shard_count);
            for (int j = 0; j < e->shard_count; j++)
            {
                if (j) pos += sprintf(pool_str + pos, ",");
                pos += sprintf(pool_str + pos, "%d", e->shard_occ[j]);
            }
            sprintf(pool_str + pos, "]/%d", e->shard_cap);
        }
        else
        {
            sprintf(pool_str, "sc=%d", e->shard_count);
        }
    }

    printf("%llu\t%s\t%s\t%d\t%.3f\t%.4f\t%.4f\t%.3f\t%.1f%%\t%.3f\t%.1f%%\t%.3f\t%.1f%%%s%s\n",
           (unsigned long long)e->ts_ms,
           e->label,
           ev_name[e->kind],
           e->tasks_done,
           e->perf.avg_global,
           e->perf.min_us,
           e->perf.max_us,
           e->perf.avg_min_us,    e->perf.pct_min,
           e->perf.avg_stable_us, e->perf.pct_stable,
           e->perf.avg_max_us,    e->perf.pct_max,
           pool_str[0] ? "\t" : "", pool_str);
    fflush(stdout);

    if (g_log_file)
    {
        fprintf(g_log_file,
                "%llu\t%s\t%s\t%d\t%.3f\t%.4f\t%.4f\t%.3f\t%.1f%%\t%.3f\t%.1f%%\t%.3f\t%.1f%%%s%s\n",
                (unsigned long long)e->ts_ms,
                e->label,
                ev_name[e->kind],
                e->tasks_done,
                e->perf.avg_global,
                e->perf.min_us,
                e->perf.max_us,
                e->perf.avg_min_us,    e->perf.pct_min,
                e->perf.avg_stable_us, e->perf.pct_stable,
                e->perf.avg_max_us,    e->perf.pct_max,
                pool_str[0] ? "\t" : "", pool_str);
        fflush(g_log_file);
    }
}

/* ============================= SAMPLER THREAD ============================= */

typedef struct
{
    ShardedPool*   pool;          /* NULL -> WinTP, sem monitoramento de shards */
    TaskArg*       args;
    int            total_tasks;
    tth_counter_t* done_counter;
    const char*    label;
    volatile int   stop;
} SamplerCtx;

#ifdef _WIN32
static DWORD WINAPI sampler_fn(void* raw)
#else
static void* sampler_fn(void* raw)
#endif
{
    SamplerCtx* ctx = (SamplerCtx*)raw;
    int prev_sc   = 0;
    int prev_full = 0;

    while (!ctx->stop)
    {
        tth_sleep_ms(1000);
        if (ctx->stop) break;

        uint64_t ts   = (tth_get_ns() - g_test_start_ns) / 1000000ULL;
        int      done = (int)tth_counter_read(ctx->done_counter);

        PerfIndicators ind = compute_indicators(ctx->args, ctx->total_tasks);

        /* le estado do pool */
        int sc   = 0, all_full = 0;
        int cap  = SHARD_CAPACITY;
        int occ[MAX_SHARD_COUNT];
        memset(occ, 0, sizeof(occ));

        if (ctx->pool)
        {
            sc = (int)atomic_get(&ctx->pool->shard_count);
            if (sc > MAX_SHARD_COUNT) sc = MAX_SHARD_COUNT;
            all_full = (sc > 0) ? 1 : 0;

            for (int i = 0; i < sc; i++)
            {
                if (ctx->pool->shards[i])
                {
                    occ[i] = ring_queue_count(&ctx->pool->shards[i]->ring);
                    if (!ring_queue_full(&ctx->pool->shards[i]->ring))
                        all_full = 0;
                }
            }

            /* evento: novo shard adicionado */
            if (sc > prev_sc && prev_sc > 0)
            {
                hist_push(ts, ctx->label, EV_SHARD_ADDED,
                          done, ctx->total_tasks, &ind, sc, occ, cap);
                print_hist_row(&g_hist[g_hist_n - 1]);
            }

            /* evento: todas as filas ficaram cheias (borda de subida) */
            if (all_full && !prev_full)
            {
                hist_push(ts, ctx->label, EV_ALL_QUEUES_FULL,
                          done, ctx->total_tasks, &ind, sc, occ, cap);
                print_hist_row(&g_hist[g_hist_n - 1]);
            }
        }

        /* amostra periodica */
        hist_push(ts, ctx->label, EV_SAMPLE,
                  done, ctx->total_tasks, &ind, sc, occ, cap);
        print_hist_row(&g_hist[g_hist_n - 1]);

        prev_sc   = sc;
        prev_full = all_full;
    }

#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* ------------------- run_pool_test -- ShardedPool ------------------------- */

static void run_pool_test(
    TaskArg*         args,
    SubmitThreadArg* sta,
    tth_thread_t*    handles,
    int              workers,
    int              total_tasks)
{
    ShardedPool pool;
    if (!pool_init(&pool))
    {
        printf("  [ERROR] pool_init falhou com %d workers\n", workers);
        return;
    }

    tth_counter_t done_count    = 0;
    tth_counter_t submit_failed = 0;

    memset(args, 0, (size_t)total_tasks * sizeof(TaskArg));
    for (int i = 0; i < total_tasks; i++)
    {
        args[i].task_id      = i;
        args[i].done_counter = &done_count;
    }

    /* inicia sampler antes das submit threads */
    SamplerCtx sctx;
    memset(&sctx, 0, sizeof(sctx));
    sctx.pool         = &pool;
    sctx.args         = args;
    sctx.total_tasks  = total_tasks;
    sctx.done_counter = &done_count;
    sctx.label        = "ShardedPool";
    sctx.stop         = 0;

    tth_thread_t sampler_handle;
    tth_thread_start(sampler_handle, sampler_fn, &sctx);

    uint64_t wall_start = tth_get_ns();

    for (int th = 0; th < TEST_THREADS; th++)
    {
        sta[th].pool          = &pool;
        sta[th].args          = &args[th * TEST_TASK_COUNT];
        sta[th].done_counter  = &done_count;
        sta[th].submit_failed = &submit_failed;
        sta[th].thread_idx    = th;
        tth_thread_start(handles[th], submit_thread_fn, &sta[th]);
    }

    uint64_t deadline = tth_get_ns() + 30ULL * 1000000000ULL;
    while (tth_counter_read(&done_count) < total_tasks)
    {
        if (tth_get_ns() > deadline)
        {
            printf("  [TIMEOUT] ShardedPool workers=%d  done=%ld/%d\n",
                   workers, (long)tth_counter_read(&done_count), total_tasks);
            break;
        }
        tth_sleep_ms(1);
    }

    for (int th = 0; th < TEST_THREADS; th++)
        tth_thread_join(handles[th]);

    uint64_t wall_end = tth_get_ns();

    /* para o sampler antes de destruir o pool */
    sctx.stop = 1;
    tth_thread_join(sampler_handle);

    pool_shutdown(&pool);

    /* resultados finais */
    double wall_ms = (double)(wall_end - wall_start) / 1e6;
    long   sf      = (long)tth_counter_read(&submit_failed);

    PerfIndicators ind = compute_indicators(args, total_tasks);
    int exec_ok   = ind.count_min + ind.count_stable + ind.count_max;
    int exec_fail = total_tasks - exec_ok;

    char status = (exec_fail == 0 && sf == 0) ? ' ' : '!';
    printf("  [%c] ShardedPool | Workers: %2d | Threads submit: %3d | Tasks: %5d"
           " | OK: %5d | Falhas exec: %d | Falhas submit: %ld\n",
           status, workers, TEST_THREADS, total_tasks, exec_ok, exec_fail, sf);
    printf("       Tempo parede   : %8.1f ms\n", wall_ms);
    print_indicators(&ind);
    if (exec_fail > 0 || sf > 0)
        printf("  *** ATENCAO: %d tasks nao executadas, %ld falhas de submit ***\n",
               exec_fail, sf);
    printf("\n");
}

/* -------------------- run_wintp_test -- Windows Thread Pool --------------- */

#ifdef _WIN32

static void run_wintp_test(
    TaskArg*        args,
    WinTPSubmitArg* wsta,
    tth_thread_t*   handles,
    int             workers,
    int             total_tasks)
{
    PTP_POOL tp_pool = CreateThreadpool(NULL);
    if (!tp_pool)
    {
        printf("  [ERROR] CreateThreadpool falhou\n");
        return;
    }
    SetThreadpoolThreadMinimum(tp_pool, workers);
    SetThreadpoolThreadMaximum(tp_pool, workers);

    TP_CALLBACK_ENVIRON tpEnv;
    InitializeThreadpoolEnvironment(&tpEnv);
    SetThreadpoolCallbackPool(&tpEnv, tp_pool);

    tth_counter_t done_count    = 0;
    tth_counter_t submit_failed = 0;

    memset(args, 0, (size_t)total_tasks * sizeof(TaskArg));
    for (int i = 0; i < total_tasks; i++)
    {
        args[i].task_id      = i;
        args[i].done_counter = &done_count;
    }

    /* sampler sem pool -- captura so metricas de tasks */
    SamplerCtx sctx;
    memset(&sctx, 0, sizeof(sctx));
    sctx.pool         = NULL;
    sctx.args         = args;
    sctx.total_tasks  = total_tasks;
    sctx.done_counter = &done_count;
    sctx.label        = "WinTP";
    sctx.stop         = 0;

    tth_thread_t sampler_handle;
    tth_thread_start(sampler_handle, sampler_fn, &sctx);

    uint64_t wall_start = tth_get_ns();

    for (int th = 0; th < TEST_THREADS; th++)
    {
        wsta[th].tpEnv         = &tpEnv;
        wsta[th].args          = &args[th * TEST_TASK_COUNT];
        wsta[th].done_counter  = &done_count;
        wsta[th].submit_failed = &submit_failed;
        handles[th] = CreateThread(NULL, 0, wintp_submit_thread_fn, &wsta[th], 0, NULL);
    }

    uint64_t deadline = tth_get_ns() + 30ULL * 1000000000ULL;
    while (tth_counter_read(&done_count) < total_tasks)
    {
        if (tth_get_ns() > deadline)
        {
            printf("  [TIMEOUT] WinTP workers=%d  done=%ld/%d\n",
                   workers, (long)tth_counter_read(&done_count), total_tasks);
            break;
        }
        tth_sleep_ms(1);
    }

    for (int th = 0; th < TEST_THREADS; th++)
    {
        WaitForSingleObject(handles[th], INFINITE);
        CloseHandle(handles[th]);
    }

    uint64_t wall_end = tth_get_ns();

    for (int i = 0; i < total_tasks; i++)
        if (args[i].work_handle)
        {
            WaitForThreadpoolWorkCallbacks(args[i].work_handle, FALSE);
            CloseThreadpoolWork(args[i].work_handle);
            args[i].work_handle = NULL;
        }

    DestroyThreadpoolEnvironment(&tpEnv);
    CloseThreadpool(tp_pool);

    sctx.stop = 1;
    tth_thread_join(sampler_handle);

    /* resultados finais */
    double wall_ms = (double)(wall_end - wall_start) / 1e6;
    long   sf      = (long)tth_counter_read(&submit_failed);

    PerfIndicators ind = compute_indicators(args, total_tasks);
    int exec_ok   = ind.count_min + ind.count_stable + ind.count_max;
    int exec_fail = total_tasks - exec_ok;

    char status = (exec_fail == 0 && sf == 0) ? ' ' : '!';
    printf("  [%c] Windows TP  | Workers: %2d | Threads submit: %3d | Tasks: %5d"
           " | OK: %5d | Falhas exec: %d | Falhas submit: %ld\n",
           status, workers, TEST_THREADS, total_tasks, exec_ok, exec_fail, sf);
    printf("       Tempo parede   : %8.1f ms\n", wall_ms);
    print_indicators(&ind);
    if (exec_fail > 0 || sf > 0)
        printf("  *** ATENCAO: %d tasks nao executadas, %ld falhas de submit ***\n",
               exec_fail, sf);
    printf("\n");
}

#endif /* _WIN32 */

/* -------------------------------- entrypoint ------------------------------ */

void test_task_handler_run(void)
{
    tsc_calibrate();

    int total_tasks = TEST_TASK_COUNT * TEST_THREADS;

    g_hist_n        = 0;
    g_test_start_ns = tth_get_ns();

    g_log_file = fopen("tth_history.tsv", "w");
    if (!g_log_file)
        printf("[AVISO] nao foi possivel criar tth_history.tsv\n");
    else
        printf("[LOG] gravando historico em: tth_history.tsv\n");

    printf("\n");
    printf("================================================================================\n");
    printf("  COMPARATIVO: ShardedPool vs Windows Thread Pool\n");
    printf("  %d threads x %d tasks = %d tasks por execucao\n",
           TEST_THREADS, TEST_TASK_COUNT, total_tasks);
    printf("================================================================================\n\n");

    print_history_header();

    TaskArg*         args    = (TaskArg*)        malloc((size_t)total_tasks  * sizeof(TaskArg));
    SubmitThreadArg* sta     = (SubmitThreadArg*)malloc((size_t)TEST_THREADS * sizeof(SubmitThreadArg));
    tth_thread_t*    handles = (tth_thread_t*)   malloc((size_t)TEST_THREADS * sizeof(tth_thread_t));
#ifdef _WIN32
    WinTPSubmitArg*  wsta    = (WinTPSubmitArg*) malloc((size_t)TEST_THREADS * sizeof(WinTPSubmitArg));
#endif

    if (!args || !sta || !handles
#ifdef _WIN32
        || !wsta
#endif
       )
    {
        printf("[ERROR] malloc falhou\n");
        free(args); free(sta); free(handles);
#ifdef _WIN32
        free(wsta);
#endif
        return;
    }

    for (int w = 0; w < worker_configs_count; w++)
    {
        int workers = worker_configs[w];

        printf("--------------------------------------------------------------------------------\n");
        printf("  workers = %d\n", workers);
        printf("--------------------------------------------------------------------------------\n\n");

        //srand(42);
        //run_pool_test(args, sta, handles, workers, total_tasks);

#ifdef _WIN32
        srand(42);
        run_wintp_test(args, wsta, handles, workers, total_tasks);
#endif

#ifdef TBB_AVAILABLE
        printf("[DIAG] TBB_AVAILABLE definido em compilacao -- chamando run_tbb_test\n");
        fflush(stdout);
        srand(42);
        run_tbb_test(args, handles, workers, total_tasks, g_log_file, g_test_start_ns);
        printf("[DIAG] run_tbb_test retornou\n");
        fflush(stdout);
#else
        printf("[DIAG] TBB_AVAILABLE NAO definido -- bloco TBB ignorado pelo pre-processador\n");
        fflush(stdout);
#endif
    }

    if (g_log_file)
    {
        fclose(g_log_file);
        g_log_file = NULL;
        printf("[LOG] tth_history.tsv salvo.\n");
    }

    free(args);
    free(sta);
    free(handles);
#ifdef _WIN32
    free(wsta);
#endif
}
