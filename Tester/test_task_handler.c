/*
 * test_task_handler.c
 * Testes de massa para ShardedPool (task_handler.h)
 *
 * Estrutura:
 *   Loop externo  - configuracoes de workers (threads paralelas)
 *   Loop interno  - submissao serial de tasks
 *
 * Metricas por task:
 *   - flag executed (0=pendente, 1=ok, -1=submit falhou)
 *   - start_ns / end_ns / exec_duration_ns
 *
 * Totalizadores:
 *   - total_exec_ns  : soma do tempo de execucao de todas as tasks
 *   - wall_ns        : tempo de parede (submit + execucao paralela)
 *   - media, min, max de duracao por task
 *
 * Nota: o arquivo de teste usa APIs de plataforma diretamente para o
 * contador atomico compartilhado (done_count), evitando dependencia das
 * funcoes __forceinline de atomics.h que nao geram simbolo externo na lib.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "../Xplatbase/Xplatbase/src/task_handler.h"

/* ─── contador atomico local (sem depender de atomics.h) ─── */

#ifdef _WIN32
    typedef volatile LONG tth_counter_t;
    #define tth_counter_inc(p)      InterlockedIncrement(p)
    #define tth_counter_read(p)     (*(p))
#else
    #include <stdatomic.h>
    typedef volatile int tth_counter_t;
    #define tth_counter_inc(p)      __sync_add_and_fetch((p), 1)
    #define tth_counter_read(p)     (*(p))
#endif

/* ────────────────────────────── timing ────────────────────────────── */

    static double    tsc_ns_ratio = 1.0;   /* cycles → ns */
    static uint64_t  tsc_overhead = 0;     /* custo de uma chamada __rdtsc */

    static void tsc_calibrate()
    {
        /* ── Calibrar frequência: rdtsc vs QPC ── */
        LARGE_INTEGER freq, t0, t1;
        QueryPerformanceFrequency(&freq);

        uint64_t c0 = __rdtsc();
        QueryPerformanceCounter(&t0);

        /* Espera 50ms para ter amostra boa */
        Sleep(50);

        uint64_t c1 = __rdtsc();
        QueryPerformanceCounter(&t1);

        double elapsed_ns = (double)(t1.QuadPart - t0.QuadPart) * 1e9 / (double)freq.QuadPart;
        double elapsed_cycles = (double)(c1 - c0);

        tsc_ns_ratio = elapsed_ns / elapsed_cycles;   /* ns por ciclo */

        /* ── Calibrar overhead: menor de N amostras ── */
        uint64_t min_overhead = UINT64_MAX;
        for (int i = 0; i < 1000; i++)
        {
            uint64_t a = __rdtsc();
            uint64_t b = __rdtsc();
            uint64_t diff = b - a;
            if (diff < min_overhead)
                min_overhead = diff;
        }
        tsc_overhead = min_overhead;
    }

    /* Leitura compensada: subtrai overhead e converte para ns */
    static inline uint64_t tsc_now()
    {
        return __rdtsc();
    }

    static inline double tsc_elapsed_ns(uint64_t start, uint64_t end)
    {
        uint64_t cycles = (end - start);
        if (cycles > tsc_overhead)
            cycles -= tsc_overhead;
        else
            cycles = 0;
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

/* ────────────────────────── argumento de task ──────────────────────── */

typedef struct
{
    int              task_id;
    int              sleep_ms;          /* duracao simulada da task      */
    volatile int     executed;          /* 0=pendente, 1=ok, -1=submit fail */
    //uint64_t         submit_ns;
    uint64_t         start_ns;
    uint64_t         end_ns;
    uint64_t         exec_duration_ns;
    tth_counter_t*   done_counter;      /* contador compartilhado        */
} TaskArg;


static void task_fn(void* arg)
{
    TaskArg* t = (TaskArg*)arg;
    t->end_ns           = tsc_now();
    t->exec_duration_ns = tsc_elapsed_ns(t->start_ns, t->end_ns);
    t->executed         = 1;
    tth_counter_inc(t->done_counter);
}

void CALLBACK my_task(PTP_CALLBACK_INSTANCE instance, PVOID context, PTP_WORK work)
{
    TaskArg* t = (TaskArg*)context;
    t->end_ns = tsc_now();
    t->exec_duration_ns = tsc_elapsed_ns(t->start_ns, t->end_ns);
    t->executed = 1;
    tth_counter_inc(t->done_counter);
}

/* ──────────────────────────── configuracoes ────────────────────────── */

#define TEST_TASK_COUNT  10000

static int worker_configs[]     = { 8 };// { 2, 4, 8, 16, 32, 64, 128 };
static int worker_configs_count = (int)(sizeof(worker_configs) / sizeof(worker_configs[0]));

/* ───────────────────────────── mass test ───────────────────────────── */

static void test_mass_tasks(void)
{
    tsc_calibrate();

    printf("\n");
    printf("================================================================================\n");
    printf("  MASS TASK TEST  -  %d tasks por execucao\n", TEST_TASK_COUNT);
    printf("================================================================================\n\n");

    TaskArg* args = (TaskArg*)malloc(TEST_TASK_COUNT * sizeof(TaskArg));
    if (!args)
    {
        printf("[ERROR] malloc falhou para args\n");
        return;
    }

    srand(42);  /* seed fixo para reprodutibilidade */

    /* ── Loop externo: diferentes contagens de workers (threads paralelas) ── */
    for (int w = 0; w < worker_configs_count; w++)
    {
        int workers = worker_configs[w];

        ShardedPool pool;
        if (!pool_init(&pool, workers))
        {
            printf("[ERROR] pool_init falhou com %d workers\n", workers);
            continue;
        }

        tth_counter_t done_count = 0;
        int           submit_failed = 0;

        memset(args, 0, TEST_TASK_COUNT * sizeof(TaskArg));

        uint64_t wall_start = tth_get_ns();

        /* ── Loop interno: submissao serial de tasks ── */
        for (int i = 0; i < TEST_TASK_COUNT; i++)
        {
            args[i].task_id      = i;
            args[i].sleep_ms     = rand() % 10;  /* 0-9 ms aleatorio */
            args[i].executed     = 0;
            args[i].done_counter = &done_count;
   
            tth_sleep_ms(args[i].sleep_ms);

            args[i].start_ns = tsc_now();

            if (!pool_submit(&pool, task_fn, &args[i]))
            {
                args[i].executed = -1;
                submit_failed++;
                tth_counter_inc(&done_count);
            }
        }

        /* ── Aguarda conclusao de todas as tasks (timeout 30s) ── */
        uint64_t deadline = tth_get_ns() + 30ULL * 1000000000ULL;
        while (tth_counter_read(&done_count) < TEST_TASK_COUNT)
        {
            if (tth_get_ns() > deadline)
            {
                printf("[TIMEOUT] workers=%d  done=%ld/%d\n", workers, (long)tth_counter_read(&done_count), TEST_TASK_COUNT);
                break;
            }
            tth_sleep_ms(1);
        }

        uint64_t wall_end = tth_get_ns();

        pool_shutdown(&pool);

        /* ── Afericao dos resultados ── */
        int      exec_ok    = 0;
        int      exec_fail  = 0;
        uint64_t total_exec = 0;
        uint64_t min_exec   = UINT64_MAX;
        uint64_t max_exec   = 0;

        for (int i = 0; i < TEST_TASK_COUNT; i++)
        {
            if (args[i].executed == 1)
            {
                exec_ok++;
                total_exec += args[i].exec_duration_ns;
                if (args[i].exec_duration_ns < min_exec) min_exec = args[i].exec_duration_ns;
                if (args[i].exec_duration_ns > max_exec) max_exec = args[i].exec_duration_ns;
            }
            else
            {
                exec_fail++;
            }
        }

        double wall_ms  = (double)(wall_end - wall_start) / 1e6;
        double total_ms = (double)total_exec               / 1e6;
        double avg_us   = exec_ok ? ((double)total_exec / (double)exec_ok) / 1000.0 : 0.0;
        double min_us   = (min_exec != UINT64_MAX) ? ((double)min_exec / 1000.0) : 0.0;
        double max_us   = (double)max_exec / 1000.0;

        char status = (exec_fail == 0 && submit_failed == 0) ? ' ' : '!';

        printf("[%c] Workers: %2d | Submetidas: %4d | OK: %4d | Falhas exec: %d | Falhas submit: %d\n", status, workers, TEST_TASK_COUNT, exec_ok, exec_fail, submit_failed);
        printf("     Tempo parede : %8.1f ms\n",   wall_ms);
        printf("     Total exec   : %8.1f ms  (soma de todas as tasks)\n", total_ms);
        if (exec_ok > 0)
        {
            printf("     Media / task : %8.1f us  |  Min: %8.4f us  |  Max: %8.4f us\n", avg_us, min_us, max_us);
        }

        if (exec_fail > 0 || submit_failed > 0)
        {
            printf("  *** ATENCAO: %d tasks nao executadas, %d falhas de submit ***\n", exec_fail, submit_failed);
        }

        printf("\n");
    }

    free(args);
}

/* ────────────────────────────── entrypoint ─────────────────────────── */

void test_task_handler_run(void)
{
    test_mass_tasks();
}
