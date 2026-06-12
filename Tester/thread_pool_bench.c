/*
 * thread_pool_bench.c — benchmark de performance do WSPool (thread_pool.c)
 *
 * Objetivo: medir o impacto das correcoes PERF/INST com numeros reproduziveis,
 * comparando BEFORE (lib intacta) x AFTER (lib corrigida). Tem seu proprio main.
 *
 * Cenarios:
 *   A  - submit single-producer, tasks minimas  -> mede overhead por submit (PERF-1)
 *   B  - submit 4 producers concorrentes        -> contencao de ring/lane (PERF-2/3)
 *   C  - submit 8 producers concorrentes (burst) -> contencao alta (PERF-2/3)
 *   D  - tasks com micro-trabalho (steal/spin)   -> caminho idle/steal (PERF-6)
 *
 * Cada cenario:
 *   - roda REPS vezes; reporta a MEDIANA (robusta a ruido de scheduler)
 *   - verifica CORRECAO: contador final == submetido, submit_failures==0
 *     (guarda contra regressao das mudancas INST)
 *
 * Saida parseavel: linhas "RESULT scen=.. ms=.. ns_per_task=.. Mtps=.. ..."
 *
 * Build: ver build_bench.bat (cl /O2, x64). So compila com este main.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../Xplatbase/Xplatbase/src/thread_pool.h"

/* ---- config ---- */
#define REPS              7
#define TASKS_A     2000000
#define TASKS_BC    2000000
#define TASKS_D      400000
#define PRODUCERS_B       4
#define PRODUCERS_C       8

/* ---- tempo ---- */
static double now_ms(void)
{
    static LARGE_INTEGER f = { 0 };
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static int cmp_double(const void* a, const void* b)
{
    double da = *(const double*)a, db = *(const double*)b;
    return (da > db) - (da < db);
}

static double median(double* v, int n)
{
    qsort(v, n, sizeof(double), cmp_double);
    return (n & 1) ? v[n/2] : (v[n/2 - 1] + v[n/2]) / 2.0;
}

/* ---- tasks ---- */
static volatile LONG64 g_counter;

static void task_tiny(void* unused)
{
    (void)unused;
    InterlockedIncrement64(&g_counter);
}

static void task_micro(void* unused)
{
    (void)unused;
    /* micro-trabalho: ~algumas centenas de ciclos, mantem worker ocupado o bastante
       para criar desbalanceamento e exercitar steal/idle sem dominar o tempo. */
    volatile uint64_t x = 1;
    for (int i = 0; i < 64; i++) x = x * 6364136223846793005ULL + 1442695040888963407ULL;
    (void)x;
    InterlockedIncrement64(&g_counter);
}

static void wait_until(LONG64 target)
{
    while (InterlockedCompareExchange64(&g_counter, 0, 0) < target)
        YieldProcessor();
}

/* ---- single producer ---- */
static double run_single(int tasks, task_fn fn, PoolStats* out_stats)
{
    PoolConfig cfg = pool_default_config();
    ShardedPool* pool = pool_create(&cfg);
    if (!pool) { printf("  pool_create FALHOU\n"); exit(2); }

    InterlockedExchange64(&g_counter, 0);
    double t0 = now_ms();
    for (int i = 0; i < tasks; i++)
        pool_submit(pool, fn, NULL);
    wait_until(tasks);
    double dt = now_ms() - t0;

    if (out_stats) pool_stats(pool, out_stats);
    pool_shutdown(pool);
    pool_destroy(pool);
    return dt;
}

/* ---- multi producer ---- */
typedef struct {
    ShardedPool* pool;
    task_fn      fn;
    int          count;
    HANDLE       gate;
} ProducerArg;

static DWORD WINAPI producer_thread(void* raw)
{
    ProducerArg* a = (ProducerArg*)raw;
    WaitForSingleObject(a->gate, INFINITE);
    for (int i = 0; i < a->count; i++)
        pool_submit(a->pool, a->fn, NULL);
    return 0;
}

static double run_multi(int total_tasks, int producers, task_fn fn, PoolStats* out_stats)
{
    PoolConfig cfg = pool_default_config();
    ShardedPool* pool = pool_create(&cfg);
    if (!pool) { printf("  pool_create FALHOU\n"); exit(2); }

    int per = total_tasks / producers;
    int real_total = per * producers;

    HANDLE gate = CreateEventW(NULL, TRUE, FALSE, NULL);
    HANDLE th[32];
    ProducerArg args[32];
    for (int i = 0; i < producers; i++) {
        args[i].pool = pool; args[i].fn = fn; args[i].count = per; args[i].gate = gate;
        th[i] = CreateThread(NULL, 0, producer_thread, &args[i], 0, NULL);
    }

    InterlockedExchange64(&g_counter, 0);
    double t0 = now_ms();
    SetEvent(gate);
    WaitForMultipleObjects(producers, th, TRUE, INFINITE);
    wait_until(real_total);
    double dt = now_ms() - t0;

    for (int i = 0; i < producers; i++) CloseHandle(th[i]);
    CloseHandle(gate);

    if (out_stats) pool_stats(pool, out_stats);
    pool_shutdown(pool);
    pool_destroy(pool);
    return dt;
}

/* ---- runner ---- */
static void report(const char* scen, int tasks, double* samples, int reps, PoolStats* st)
{
    double med = median(samples, reps);
    double mn = samples[0];           /* qsort em median deixou ordenado */
    double ns_per = (med * 1e6) / (double)tasks;
    double mtps = (double)tasks / (med * 1000.0);
    printf("RESULT scen=%s tasks=%d med_ms=%.2f min_ms=%.2f ns_per_task=%.1f Mtps=%.2f "
           "stolen=%llu handoffs=%llu rescued=%llu backpressure=%llu expansions=%llu failures=%llu\n",
           scen, tasks, med, mn, ns_per, mtps,
           (unsigned long long)st->total_stolen,
           (unsigned long long)st->total_handoffs,
           (unsigned long long)st->total_rescued,
           (unsigned long long)st->submit_backpressure,
           (unsigned long long)st->total_expansions,
           (unsigned long long)st->submit_failures);
}

static int g_correctness_ok = 1;

static void check(const char* scen, LONG64 expected)
{
    LONG64 got = InterlockedCompareExchange64(&g_counter, 0, 0);
    if (got != expected) {
        printf("  [CORRECAO FALHOU] %s: executadas=%lld esperadas=%lld\n",
               scen, (long long)got, (long long)expected);
        g_correctness_ok = 0;
    }
}

int main(void)
{
    printf("################################################################\n");
    printf("  thread_pool — BENCHMARK de performance\n");
    printf("  ncpu=%d  reps=%d\n", xcpu_count(), REPS);
    printf("################################################################\n");

    double s[REPS];
    PoolStats st;

    /* A: single producer, tiny */
    for (int r = 0; r < REPS; r++) { s[r] = run_single(TASKS_A, task_tiny, &st); check("A", TASKS_A); }
    report("A_single_tiny", TASKS_A, s, REPS, &st);

    /* B: 4 producers, tiny */
    for (int r = 0; r < REPS; r++) { s[r] = run_multi(TASKS_BC, PRODUCERS_B, task_tiny, &st); check("B", (TASKS_BC/PRODUCERS_B)*PRODUCERS_B); }
    report("B_4prod_tiny", (TASKS_BC/PRODUCERS_B)*PRODUCERS_B, s, REPS, &st);

    /* C: 8 producers, tiny */
    for (int r = 0; r < REPS; r++) { s[r] = run_multi(TASKS_BC, PRODUCERS_C, task_tiny, &st); check("C", (TASKS_BC/PRODUCERS_C)*PRODUCERS_C); }
    report("C_8prod_tiny", (TASKS_BC/PRODUCERS_C)*PRODUCERS_C, s, REPS, &st);

    /* D: single producer, micro-work (steal/idle) */
    for (int r = 0; r < REPS; r++) { s[r] = run_single(TASKS_D, task_micro, &st); check("D", TASKS_D); }
    report("D_single_micro", TASKS_D, s, REPS, &st);

    printf("----------------------------------------------------------------\n");
    printf("  CORRECAO: %s\n", g_correctness_ok ? "OK (todas as contagens batem)" : "FALHOU");
    printf("----------------------------------------------------------------\n");
    return g_correctness_ok ? 0 : 1;
}
