/*
 * bench.c - comparativo de alocadores (Windows / MSVC)
 *
 *   - CRT malloc/free
 *   - memop  (nossa pool; >16KB usa fallback malloc, design da pendencia "large")
 *   - rpmalloc
 *   - mimalloc
 *
 * Cenarios:
 *   A) Larson-style: working-set fixo por thread + churn (free+alloc), tamanhos
 *      aleatorios pequeno/medio/grande, multi-thread. Mede vazao sob rotatividade.
 *   B) Small fixed: alloc/free 64B em rajada, 1 thread (melhor caso de pool).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "../Xplatbase/Xplatbase/src/memory_pool.h"
#include "../Xplatbase/Xplatbase/src/mem_leak_watch.h"
#include "rpmalloc/rpmalloc/rpmalloc.h"
#include "mimalloc/include/mimalloc.h"

/* ------------------------------------------------------------------ */
/*  RNG rapido por-thread (xorshift64), sem lock                       */
/* ------------------------------------------------------------------ */
static uint64_t rng_next(uint64_t* s)
{
    uint64_t x = *s;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    *s = x;
    return x;
}

/* Distribuicao: 75% pequeno [8,128], 20% medio [129,2048], 5% grande [2049,65536] */
static size_t rand_size(uint64_t* s)
{
    uint32_t r = (uint32_t)(rng_next(s) % 100u);
    if (r < 75)  return 8    + (size_t)(rng_next(s) % 121u);
    if (r < 95)  return 129  + (size_t)(rng_next(s) % 1920u);
    return            2049 + (size_t)(rng_next(s) % 63488u);
}

/* ------------------------------------------------------------------ */
/*  Interface de alocador                                              */
/* ------------------------------------------------------------------ */
typedef struct
{
    const char* name;
    void  (*g_init)(void);
    void  (*g_fini)(void);
    void  (*t_init)(void);
    void  (*t_fini)(void);
    void* (*alloc)(size_t);
    void  (*free_)(void*, size_t);
} Alloc;

/* --- CRT malloc --- */
static void  noop(void) {}
static void* sys_alloc(size_t n) { return malloc(n); }
static void  sys_free(void* p, size_t n) { (void)n; free(p); }

/* --- memop (pool) --- */
static void memop_g_init(void) { thread_init(NULL, NULL); memop_init(); }
static void memop_g_fini(void) { memop_shutdown(); }
static void* memop_a(size_t n) { return memop_alloc_raw(n); }     /* void* em registrador */
static void  memop_f(void* p, size_t n) { (void)n; memop_free_raw(p); }

/* --- memop + mem_leak_watch --- *
 * Mesmo alocador (memop_a/memop_f), com o monitor de vazamento ligado por
 * cima -- mede o custo real de ter o site (CaptureStackBackTrace por
 * span_create) e o timer barato (memop_get_stats() sob G.lock/G.cache_lock)
 * ativos durante o benchmark, em duas configuracoes de intervalo:
 *   - "dbg":  intervalo curto (200ms) -- varredura de raizes frequente,
 *     pior caso de contencao pelo timer durante o benchmark.
 *   - "prod": intervalo longo (60s) -- o timer praticamente nao dispara
 *     durante a corrida (cenarios duram segundos), custo esperado ~= memop puro.
 * default_config() usa _DEBUG p/ escolher o intervalo, mas este bench
 * compila com /DNDEBUG -- por isso o interval_ms e sobrescrito explicitamente
 * abaixo em vez de confiar no default. */
static void memop_lw_dbg_g_init(void)
{
    MemLeakWatchConfig cfg;
    thread_init(NULL, NULL);
    memop_init();
    mem_leak_watch_default_config(&cfg);   /* log_path fica no default: absoluto, ao lado do .exe */
    cfg.interval_ms = 200;
    mem_leak_watch_start(&cfg);
}
static void memop_lw_dbg_g_fini(void)
{
    mem_leak_watch_scan_now();   /* forca 1 varredura real (limiar de RAM nunca e cruzado aqui) */
    mem_leak_watch_stop();
    memop_shutdown();
}

static void memop_lw_prod_g_init(void)
{
    MemLeakWatchConfig cfg;
    thread_init(NULL, NULL);
    memop_init();
    mem_leak_watch_default_config(&cfg);   /* log_path fica no default: absoluto, ao lado do .exe */
    cfg.interval_ms = 60000;
    mem_leak_watch_start(&cfg);
}
static void memop_lw_prod_g_fini(void)
{
    mem_leak_watch_scan_now();
    mem_leak_watch_stop();
    memop_shutdown();
}

/* --- rpmalloc --- */
static void rp_g_init(void) { rpmalloc_initialize(0); }
static void rp_g_fini(void) { rpmalloc_finalize(); }
static void rp_t_init(void) { rpmalloc_thread_initialize(); }
static void rp_t_fini(void) { rpmalloc_thread_finalize(); }
static void* rp_a(size_t n) { return rpmalloc(n); }
static void  rp_f(void* p, size_t n) { (void)n; rpfree(p); }

/* --- mimalloc --- */
static void* mi_a(size_t n) { return mi_malloc(n); }
static void  mi_f(void* p, size_t n) { (void)n; mi_free(p); }

static Alloc ALLOCS[] = {
    { "sys-malloc",     noop,             noop,              noop,      noop,      sys_alloc, sys_free },
    { "memop-pool",     memop_g_init,     memop_g_fini,      noop,      noop,      memop_a,   memop_f  },
    { "memop-lw-dbg",   memop_lw_dbg_g_init,  memop_lw_dbg_g_fini,  noop,  noop,  memop_a,   memop_f  },
    { "memop-lw-prod",  memop_lw_prod_g_init, memop_lw_prod_g_fini, noop,  noop,  memop_a,   memop_f  },
    { "rpmalloc",       rp_g_init,        rp_g_fini,         rp_t_init, rp_t_fini, rp_a,      rp_f     },
    { "mimalloc",       noop,             noop,              noop,      noop,      mi_a,      mi_f     },
};
#define NALLOC (int)(sizeof(ALLOCS)/sizeof(ALLOCS[0]))

/* ------------------------------------------------------------------ */
/*  Cenario A: Larson churn                                            */
/* ------------------------------------------------------------------ */
typedef struct
{
    Alloc*   a;
    int      slots;
    long     ops;
    uint64_t seed;
    double   elapsed;   /* saida: segundos do laco cronometrado */
} Work;

static double qpc_now(LARGE_INTEGER freq)
{
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)freq.QuadPart;
}

static DWORD WINAPI larson_worker(void* arg)
{
    Work* w = (Work*)arg;
    Alloc* a = w->a;
    LARGE_INTEGER freq; QueryPerformanceFrequency(&freq);
    uint64_t rng = w->seed;
    int i;
    long k;
    double t0, t1;

    void**  ptr = (void**)malloc((size_t)w->slots * sizeof(void*));
    size_t* sz  = (size_t*)malloc((size_t)w->slots * sizeof(size_t));

    a->t_init();

    /* pre-enche o working-set */
    for (i = 0; i < w->slots; i++)
    {
        size_t s = rand_size(&rng);
        ptr[i] = a->alloc(s);
        sz[i]  = s;
        if (ptr[i]) { ((char*)ptr[i])[0] = 1; ((char*)ptr[i])[s - 1] = 1; }
    }

    /* laco cronometrado: free + alloc (1 op = 1 par) */
    t0 = qpc_now(freq);
    for (k = 0; k < w->ops; k++)
    {
        int idx = (int)(rng_next(&rng) % (uint64_t)w->slots);
        a->free_(ptr[idx], sz[idx]);
        {
            size_t s = rand_size(&rng);
            void* p = a->alloc(s);
            ptr[idx] = p;
            sz[idx]  = s;
            if (p) { ((char*)p)[0] = (char)k; ((char*)p)[s - 1] = (char)k; }
        }
    }
    t1 = qpc_now(freq);

    for (i = 0; i < w->slots; i++) a->free_(ptr[i], sz[i]);
    a->t_fini();

    free(ptr); free(sz);
    w->elapsed = t1 - t0;
    return 0;
}

static void run_larson(Alloc* a, int threads, int slots, long ops_per_thread)
{
    HANDLE* h = (HANDLE*)malloc((size_t)threads * sizeof(HANDLE));
    Work*   w = (Work*)malloc((size_t)threads * sizeof(Work));
    int i;
    double maxel = 0.0;
    double total_ops = (double)ops_per_thread * threads;

    a->g_init();

    for (i = 0; i < threads; i++)
    {
        w[i].a = a; w[i].slots = slots; w[i].ops = ops_per_thread;
        w[i].seed = 0x9E3779B97F4A7C15ull ^ ((uint64_t)(i + 1) * 0xD1B54A32D192ED03ull);
        w[i].elapsed = 0.0;
        h[i] = CreateThread(NULL, 0, larson_worker, &w[i], 0, NULL);
    }
    WaitForMultipleObjects(threads, h, TRUE, INFINITE);
    for (i = 0; i < threads; i++)
    {
        if (w[i].elapsed > maxel) maxel = w[i].elapsed;
        CloseHandle(h[i]);
    }

    a->g_fini();

    printf("  %-12s %10.3f %12.2f\n",
           a->name, maxel, (total_ops / maxel) / 1e6);

    free(h); free(w);
}

/* ------------------------------------------------------------------ */
/*  Cenario B: small fixed 64B, 1 thread                              */
/* ------------------------------------------------------------------ */
static void run_smallfixed(Alloc* a, long ops)
{
    LARGE_INTEGER freq; QueryPerformanceFrequency(&freq);
    double t0, t1, t_pair, t_alloc = 0.0, t_free = 0.0;
    long k, b, batches;
    enum { SL = 256 };
    void* ptr[SL];
    int i;

    a->g_init();
    a->t_init();
    for (i = 0; i < SL; i++) { ptr[i] = a->alloc(64); if (ptr[i]) ((char*)ptr[i])[0]=1; }

    /* (1) par free+alloc intercalado: vazao combinada do hot path */
    t0 = qpc_now(freq);
    for (k = 0; k < ops; k++)
    {
        int idx = (int)(k & (SL - 1));
        a->free_(ptr[idx], 64);
        ptr[idx] = a->alloc(64);
        if (ptr[idx]) ((char*)ptr[idx])[0] = (char)k;
    }
    t1 = qpc_now(freq);
    t_pair = t1 - t0;

    /* (2) alloc e free cronometrados em FASES separadas (working-set quente SL):
     *     cada lote libera todos os SL e depois realoca, isolando o custo de
     *     cada operacao sem o efeito de intercalacao. */
    batches = ops / SL;
    for (b = 0; b < batches; b++)
    {
        t0 = qpc_now(freq);
        for (i = 0; i < SL; i++) a->free_(ptr[i], 64);
        t1 = qpc_now(freq);
        t_free += t1 - t0;

        t0 = qpc_now(freq);
        for (i = 0; i < SL; i++) ptr[i] = a->alloc(64);
        t1 = qpc_now(freq);
        t_alloc += t1 - t0;
    }

    for (i = 0; i < SL; i++) a->free_(ptr[i], 64);
    a->t_fini();
    a->g_fini();

    {
        double nsep = (double)(batches * SL);
        printf("  %-12s %9.2f %7.1f %9.2f %7.1f %9.2f\n",
               a->name,
               (nsep / t_alloc) / 1e6, t_alloc * 1e9 / nsep,
               (nsep / t_free)  / 1e6, t_free  * 1e9 / nsep,
               ((double)ops / t_pair) / 1e6);
    }
}

/* ------------------------------------------------------------------ */
/*  Cenario C: latencia por chamada (ns/alloc, ns/free, pior caso)     */
/* ------------------------------------------------------------------ */
static void run_latency(Alloc* a, long grow_k)
{
    enum { WS = 100000 };          /* working-set quente */
    LARGE_INTEGER freq; QueryPerformanceFrequency(&freq);
    void**   ptr = (void**)malloc((size_t)grow_k * sizeof(void*));
    size_t*  sz  = (size_t*)malloc((size_t)grow_k * sizeof(size_t));
    void**   ws  = (void**)malloc((size_t)WS * sizeof(void*));
    size_t*  wsz = (size_t*)malloc((size_t)WS * sizeof(size_t));
    uint64_t rng = 0xABCDEF1234567ull;
    long i, m;
    long M = 4000000;              /* iteracoes do churn quente */
    double t0, t1, grow_ns, warm_alloc_ns = 0.0, warm_free_ns = 0.0, maxns = 0.0;

    a->g_init(); a->t_init();

    /* (1) CRESCIMENTO: 2M allocs novas, com toque (inclui commit de pagina).
     *     Tempo de uma alloc quando o pool esta crescendo. */
    for (i = 0; i < grow_k; i++) sz[i] = 16 + (size_t)(rng_next(&rng) % 241);
    t0 = qpc_now(freq);
    for (i = 0; i < grow_k; i++) { ptr[i] = a->alloc(sz[i]); if (ptr[i]) ((char*)ptr[i])[0] = 1; }
    t1 = qpc_now(freq);
    grow_ns = (t1 - t0) * 1e9 / (double)grow_k;
    for (i = 0; i < grow_k; i++) a->free_(ptr[i], sz[i]);

    /* (2) QUENTE: working-set residente; alloc e free cronometrados em FASES
     *     separadas, por janelas pequenas (BW) para nao esvaziar spans inteiros
     *     -> a maior parte do working-set permanece residente (quente). */
    for (i = 0; i < WS; i++) { wsz[i] = 16 + (size_t)(rng_next(&rng) % 241); ws[i] = a->alloc(wsz[i]); if (ws[i]) ((char*)ws[i])[0]=1; }
    {
        enum { BW = 256 };           /* janela << WS: spans seguem residentes */
        long base = 0, nb = M / BW, b, j;
        double ta = 0.0, tf = 0.0;
        for (b = 0; b < nb; b++)
        {
            t0 = qpc_now(freq);
            for (j = 0; j < BW; j++) a->free_(ws[base + j], wsz[base + j]);
            t1 = qpc_now(freq);
            tf += t1 - t0;

            t0 = qpc_now(freq);
            for (j = 0; j < BW; j++) ws[base + j] = a->alloc(wsz[base + j]);
            t1 = qpc_now(freq);
            ta += t1 - t0;

            base += BW;
            if (base + BW > WS) base = 0;
        }
        warm_alloc_ns = ta * 1e9 / (double)(nb * BW);
        warm_free_ns  = tf * 1e9 / (double)(nb * BW);
    }

    /* (3) PIOR CASO de alloc: timer por-chamada no churn (max captura o pico
     *     de criar/recarregar span). */
    for (m = 0; m < 1000000; m++)
    {
        long idx = m % WS;
        double s, e, d;
        a->free_(ws[idx], wsz[idx]);
        s = qpc_now(freq);
        ws[idx] = a->alloc(wsz[idx]);
        e = qpc_now(freq);
        d = (e - s) * 1e9;
        if (d > maxns) maxns = d;
    }

    for (i = 0; i < WS; i++) a->free_(ws[i], wsz[i]);
    a->t_fini(); a->g_fini();

    printf("  %-12s %12.1f %12.1f %12.1f %12.1f\n",
           a->name, warm_alloc_ns, warm_free_ns, grow_ns, maxns);
    free(ptr); free(sz); free(ws); free(wsz);
}

int main(int argc, char** argv)
{
    SYSTEM_INFO si; GetSystemInfo(&si);
    int hw = (int)si.dwNumberOfProcessors;
    int threads = hw < 8 ? hw : 8;
    int slots = 2000;
    long ops = 4000000;       /* por thread */
    long smallops = 30000000; /* 1 thread */
    int i;

    if (argc > 1) threads = atoi(argv[1]);
    if (argc > 2) ops = atol(argv[2]);

    printf("CPUs=%d  threads=%d  slots/thread=%d  ops/thread=%ld\n", hw, threads, slots, ops);

    printf("\n== Cenario A: Larson churn (free+alloc intercalado, multi-thread) ==\n");
    printf("  %-12s %10s %12s\n", "allocator", "tempo(s)", "vazao Mop/s");
    for (i = 0; i < NALLOC; i++) run_larson(&ALLOCS[i], threads, slots, ops);

    printf("\n== Cenario B: small fixed 64B (1 thread, %ld ops) ==\n", smallops);
    printf("  %-12s %9s %7s %9s %7s %9s\n",
           "allocator", "aloc Mop", "ns", "free Mop", "ns", "par Mop");
    for (i = 0; i < NALLOC; i++) run_smallfixed(&ALLOCS[i], smallops);

    printf("\n== Cenario C: latencia por chamada, em ns (1 thread, 2M blocos 16-256B) ==\n");
    printf("  %-12s %12s %12s %12s %12s\n",
           "allocator", "aloc quente", "free quente", "aloc cresc", "aloc pior");
    for (i = 0; i < NALLOC; i++) run_latency(&ALLOCS[i], 2000000);

    return 0;
}
