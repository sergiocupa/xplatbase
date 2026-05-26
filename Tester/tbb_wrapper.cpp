/*
 * tbb_wrapper.cpp
 * Teste de performance usando Intel TBB (oneTBB).
 *
 * Pre-requisitos:
 *   Instalar TBB via vcpkg  : vcpkg install tbb:x64-windows
 *   ou via Intel oneAPI     : https://www.intel.com/content/www/us/en/developer/tools/oneapi/onetbb.html
 *
 *   No vcxproj, definir a propriedade TBB_DIR apontando para a raiz do TBB.
 *   Exemplos:
 *     vcpkg   : $(VCPKG_ROOT)\installed\x64-windows
 *     oneAPI  : $(ONEAPI_ROOT)tbb\latest
 *
 *   Adicionar TBB_AVAILABLE ao pre-processador para habilitar.
 */

#ifdef TBB_AVAILABLE

#include <Windows.h>
#include <tbb/task_arena.h>
#include <tbb/task_group.h>
#include <tbb/info.h>
#include <tbb/global_control.h>
#include <chrono>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

/* ── constantes (devem coincidir com test_task_handler.c) ── */
#define TBB_TASK_COUNT  1000
#define TBB_THREADS     100

/* Percentual de hw_concurrency usado como workers TBB.
   Workers ociosos fazem spin-wait consumindo CPU.
   Reduza para diminuir CPU; aumente para menor latencia maxima.
   Intervalo valido: 1..100 */
#ifndef TBB_WORKER_PCT
#define TBB_WORKER_PCT  50
#endif

/* ── tipos locais (layout identico ao de test_task_handler.c) ── */

#ifdef _WIN32
    typedef volatile LONG tth_counter_t;
    #define tth_counter_inc(p)   InterlockedIncrement(p)
    #define tth_counter_read(p)  (*(p))
    typedef HANDLE tth_thread_t;
    #define tth_thread_start(h, fn, arg) ((h) = CreateThread(NULL, 0, (fn), (arg), 0, NULL))
    #define tth_thread_join(h)           do { WaitForSingleObject((h), INFINITE); CloseHandle(h); } while (0)
#else
    typedef volatile int tth_counter_t;
    #define tth_counter_inc(p)   __sync_add_and_fetch((p), 1)
    #define tth_counter_read(p)  (*(p))
    #include <pthread.h>
    typedef pthread_t tth_thread_t;
    #define tth_thread_start(h, fn, arg) pthread_create(&(h), NULL, (fn), (arg))
    #define tth_thread_join(h)           pthread_join((h), NULL)
#endif

typedef struct
{
    int             task_id;
    int             sleep_ms;
    volatile int    executed;
    uint64_t        start_ns;
    uint64_t        end_ns;
    uint64_t        exec_duration_ns;
    tth_counter_t*  done_counter;
#ifdef _WIN32
    PTP_WORK        work_handle;
#endif
} TaskArg;


/* ── timing ── */

static uint64_t tbb_now_ns(void)
{
    using namespace std::chrono;
    return (uint64_t)duration_cast<nanoseconds>(
        high_resolution_clock::now().time_since_epoch()).count();
}

static void tbb_sleep_us(int64_t us)
{
#ifdef _WIN32
    HANDLE timer = CreateWaitableTimer(NULL, TRUE, NULL);
    if (timer)
    {
        LARGE_INTEGER li;
        li.QuadPart = -(us * 10);
        SetWaitableTimer(timer, &li, 0, NULL, NULL, FALSE);
        WaitForSingleObject(timer, INFINITE);
        CloseHandle(timer);
    }
#else
    struct timespec ts;
    ts.tv_sec  = us / 1000000;
    ts.tv_nsec = (us % 1000000) * 1000;
    nanosleep(&ts, NULL);
#endif
}


/* ── metricas ── */

typedef struct
{
    double avg_us;
    double min_us;
    double max_us;
    double stddev_us;
    double boundary_low_us;
    double boundary_high_us;
    int    count_min;    double avg_min_us;    double pct_min;
    int    count_stable; double avg_stable_us; double pct_stable;
    int    count_max;    double avg_max_us;    double pct_max;
} TBBPerfInd;

static TBBPerfInd tbb_compute_ind(TaskArg* args, int total)
{
    TBBPerfInd ind;
    memset(&ind, 0, sizeof(ind));

    int n = 0;
    double sum = 0.0, gmin = 1e18, gmax = 0.0;

    for (int i = 0; i < total; i++)
    {
        if (args[i].executed == 1)
        {
            double v = (double)args[i].exec_duration_ns;
            sum += v; n++;
            if (v < gmin) gmin = v;
            if (v > gmax) gmax = v;
        }
    }
    if (n == 0) return ind;

    double mean = sum / n;
    double var  = 0.0;
    for (int i = 0; i < total; i++)
        if (args[i].executed == 1) { double d = (double)args[i].exec_duration_ns - mean; var += d * d; }

    double sigma = n > 1 ? sqrt(var / n) : 0.0;
    double lo = mean - sigma, hi = mean + sigma;
    double s_min = 0, s_mid = 0, s_max = 0;
    int    c_min = 0, c_mid = 0, c_max = 0;

    for (int i = 0; i < total; i++)
    {
        if (args[i].executed == 1)
        {
            double v = (double)args[i].exec_duration_ns;
            if      (v < lo) { s_min += v; c_min++; }
            else if (v > hi) { s_max += v; c_max++; }
            else             { s_mid += v; c_mid++; }
        }
    }

    ind.avg_us           = mean / 1000.0;
    ind.min_us           = gmin / 1000.0;
    ind.max_us           = gmax / 1000.0;
    ind.stddev_us        = sigma / 1000.0;
    ind.boundary_low_us  = lo / 1000.0;
    ind.boundary_high_us = hi / 1000.0;

    ind.count_min    = c_min; ind.avg_min_us    = c_min > 0 ? (s_min / c_min) / 1000.0 : 0; ind.pct_min    = (double)c_min * 100.0 / n;
    ind.count_stable = c_mid; ind.avg_stable_us = c_mid > 0 ? (s_mid / c_mid) / 1000.0 : 0; ind.pct_stable = (double)c_mid * 100.0 / n;
    ind.count_max    = c_max; ind.avg_max_us    = c_max > 0 ? (s_max / c_max) / 1000.0 : 0; ind.pct_max    = (double)c_max * 100.0 / n;

    return ind;
}


/* ── sampler thread ── */

typedef struct
{
    TaskArg*       args;
    int            total_tasks;
    tth_counter_t* done_counter;
    FILE*          log_file;
    uint64_t       test_start_ns;
    volatile int   stop;
} TBBSamplerCtx;

static void tbb_print_sample_row(FILE* log_file, uint64_t ts_ms, int done,
                                 const TBBPerfInd* ind)
{
    printf("%llu\tTBB\tsample         \t%d\t%.3f\t%.4f\t%.4f\t%.3f\t%.1f%%\t%.3f\t%.1f%%\t%.3f\t%.1f%%\n",
           (unsigned long long)ts_ms, done,
           ind->avg_us, ind->min_us, ind->max_us,
           ind->avg_min_us, ind->pct_min,
           ind->avg_stable_us, ind->pct_stable,
           ind->avg_max_us, ind->pct_max);
    fflush(stdout);

    if (log_file)
    {
        fprintf(log_file,
                "%llu\tTBB\tsample         \t%d\t%.3f\t%.4f\t%.4f\t%.3f\t%.1f%%\t%.3f\t%.1f%%\t%.3f\t%.1f%%\n",
                (unsigned long long)ts_ms, done,
                ind->avg_us, ind->min_us, ind->max_us,
                ind->avg_min_us, ind->pct_min,
                ind->avg_stable_us, ind->pct_stable,
                ind->avg_max_us, ind->pct_max);
        fflush(log_file);
    }
}

static DWORD WINAPI tbb_sampler_fn(void* raw)
{
    TBBSamplerCtx* ctx = (TBBSamplerCtx*)raw;

    while (!ctx->stop)
    {
        Sleep(1000);
        if (ctx->stop) break;

        uint64_t ts_ms = (tbb_now_ns() - ctx->test_start_ns) / 1000000ULL;
        int      done  = (int)tth_counter_read(ctx->done_counter);
        TBBPerfInd ind = tbb_compute_ind(ctx->args, ctx->total_tasks);

        tbb_print_sample_row(ctx->log_file, ts_ms, done, &ind);
    }
    return 0;
}


/* ── submit thread ── */

struct TBBSubmitArg
{
    tbb::task_arena* arena;
    tbb::task_group* tg;
    TaskArg*         args;
    tth_counter_t*   done_counter;
};

static DWORD WINAPI tbb_submit_fn(void* raw)
{
    TBBSubmitArg* sta = (TBBSubmitArg*)raw;
    for (int i = 0; i < TBB_TASK_COUNT; i++)
    {
        TaskArg* t  = &sta->args[i];
        t->sleep_ms = rand() % 10;
        tbb_sleep_us(t->sleep_ms);
        sta->arena->execute([sta, t]()
        {
            sta->tg->run([t]()
            {
                /* start_ns marcado aqui: mede apenas o tempo de execucao,
                   nao o tempo de fila */
                t->start_ns         = tbb_now_ns();
                t->end_ns           = tbb_now_ns();
                t->exec_duration_ns = t->end_ns - t->start_ns;
                t->executed         = 1;
                tth_counter_inc(t->done_counter);
            });
        });
    }
    return 0;
}


/* ── entrypoint (extern "C" para chamar do .c) ── */

extern "C" void run_tbb_test(void* args_raw, void* handles_raw, int workers, int total_tasks,
                             void* log_file_raw, uint64_t test_start_ns)
{
    TaskArg*      args    = (TaskArg*)args_raw;
    tth_thread_t* handles = (tth_thread_t*)handles_raw;
    FILE*         log_file = (FILE*)log_file_raw;

    /* TBB workers fazem spin-wait: mais workers que CPUs logicas causam
       uso intenso de CPU sem ganho de throughput.
       Aplica TBB_WORKER_PCT sobre hw_concurrency; minimo 1. */
    int hw_concurrency   = tbb::info::default_concurrency();
    int pct_cap          = (hw_concurrency * TBB_WORKER_PCT + 99) / 100;
    if (pct_cap < 1) pct_cap = 1;
    int effective_workers = (workers > pct_cap) ? pct_cap : workers;

    /* global_control restringe o pool interno do TBB ao mesmo limite,
       fazendo threads excedentes dormirem em vez de spinarem */
    tbb::global_control gc(tbb::global_control::max_allowed_parallelism,
                           (size_t)effective_workers);

    tbb::task_arena arena(effective_workers);
    printf("[TBB] workers solicitados=%d  efetivos=%d  (hw=%d  pct=%d%%)\n",
           workers, effective_workers, hw_concurrency, TBB_WORKER_PCT);
    fflush(stdout);

    tth_counter_t done_count = 0;

    memset(args, 0, (size_t)total_tasks * sizeof(TaskArg));
    for (int i = 0; i < total_tasks; i++)
    {
        args[i].task_id      = i;
        args[i].done_counter = &done_count;
    }

    /* inicia sampler antes das submit threads */
    TBBSamplerCtx sctx;
    memset(&sctx, 0, sizeof(sctx));
    sctx.args          = args;
    sctx.total_tasks   = total_tasks;
    sctx.done_counter  = &done_count;
    sctx.log_file      = log_file;
    sctx.test_start_ns = test_start_ns;
    sctx.stop          = 0;

    tth_thread_t sampler_handle;
    tth_thread_start(sampler_handle, tbb_sampler_fn, &sctx);

    TBBSubmitArg sta[TBB_THREADS];
    uint64_t wall_start = tbb_now_ns();
    uint64_t wall_end   = wall_start;

    /*
     * task_group DEVE ser criado dentro de arena.execute() para ficar
     * associado a esta arena. Se criado fora, fica ligado ao scheduler
     * global do TBB e os workers desta arena nao executam as tasks.
     */
    arena.execute([&]()
    {
        tbb::task_group tg;

        for (int th = 0; th < TBB_THREADS; th++)
        {
            sta[th].arena        = &arena;
            sta[th].tg           = &tg;
            sta[th].args         = &args[th * TBB_TASK_COUNT];
            sta[th].done_counter = &done_count;
            tth_thread_start(handles[th], tbb_submit_fn, &sta[th]);
        }

        uint64_t deadline = tbb_now_ns() + 30ULL * 1000000000ULL;
        while (tth_counter_read(&done_count) < total_tasks)
        {
            if (tbb_now_ns() > deadline)
            {
                printf("  [TIMEOUT] TBB workers=%d  done=%ld/%d\n",
                       workers, (long)tth_counter_read(&done_count), total_tasks);
                break;
            }
            Sleep(1);
        }

        for (int th = 0; th < TBB_THREADS; th++)
            tth_thread_join(handles[th]);

        tg.wait();
        wall_end = tbb_now_ns();
    });

    sctx.stop = 1;
    tth_thread_join(sampler_handle);

    double wall_ms = (double)(wall_end - wall_start) / 1e6;

    TBBPerfInd ind      = tbb_compute_ind(args, total_tasks);
    int        exec_ok  = ind.count_min + ind.count_stable + ind.count_max;
    int        exec_fail= total_tasks - exec_ok;

    char status = exec_fail == 0 ? ' ' : '!';
    printf("  [%c] Intel TBB   | Workers: %2d | Threads submit: %3d | Tasks: %5d"
           " | OK: %5d | Falhas exec: %d\n",
           status, workers, TBB_THREADS, total_tasks, exec_ok, exec_fail);
    printf("       Tempo parede   : %8.1f ms\n", wall_ms);
    printf("       Media global   : %10.3f us\n", ind.avg_us);
    printf("       Min / Max      : %10.4f us  /  %10.4f us\n", ind.min_us, ind.max_us);
    printf("       StdDev (sigma) : %10.3f us  ->  limites: [%.4f .. %.4f] us\n",
           ind.stddev_us, ind.boundary_low_us, ind.boundary_high_us);
    printf("       Regiao minimos : %6d tasks (%5.1f%%)  avg = %10.3f us   (< avg-sigma)\n",
           ind.count_min, ind.pct_min, ind.avg_min_us);
    printf("       Regiao estavel : %6d tasks (%5.1f%%)  avg = %10.3f us   (avg +/- sigma)\n",
           ind.count_stable, ind.pct_stable, ind.avg_stable_us);
    printf("       Regiao maximos : %6d tasks (%5.1f%%)  avg = %10.3f us   (> avg+sigma)\n",
           ind.count_max, ind.pct_max, ind.avg_max_us);
    printf("\n");
}

#endif /* TBB_AVAILABLE */
