/*
 * bench_alocadores.c - comparativo de alocadores, no Gerenciador de Testes
 * (PoolDeMemoria.BenchAlocadores; convertido de bench/bench.c -- os cenarios e as medidas
 * sao os mesmos, so a moldura mudou).
 *
 * Diferencas para o programa original:
 *   - rpmalloc e mimalloc sao OPCIONAIS: entram so se bench/rpmalloc e bench/mimalloc
 *     existirem (nao sao versionados; o .vcxproj define XPB_TEM_RPMALLOC/XPB_TEM_MIMALLOC).
 *   - o memop medido e o da lib como ela e distribuida (COM estatisticas); o bench/ original
 *     compilava o memory_pool com MEMOP_NO_STATS, entao os numeros do memop saem um pouco
 *     mais lentos aqui.
 *   - CRT dinamica (/MD), como a DLL de testes; o original usava a estatica (/MT).
 *   - o log do mem_leak_watch vai para a pasta temporaria.
 *   - como o bench desliga os ganchos de thread e reinicia o pool, no fim eles sao
 *     restaurados para os testes que vierem depois.
 *   - so mede em Release (pulado em Debug); e PULADO por padrao no Gerenciador (ver
 *     registro.cpp). Parametros: XPB_BENCH_THREADS (padrao min(CPUs, 8)),
 *     XPB_BENCH_OPS (ops por thread no cenario A, padrao 4.000.000).
 *
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

#include "ctest_core.h"
#include "testes.h"
#include "memory_pool.h"
#include "mem_leak_watch.h"
#ifdef XPB_TEM_RPMALLOC
#include "rpmalloc/rpmalloc.h"
#endif
#ifdef XPB_TEM_MIMALLOC
#include "mimalloc.h"
#endif

/* log do mem_leak_watch dos cenarios "memop-lw": pasta temporaria */
static char g_lw_log[MAX_PATH];
static const char* lw_log(void)
{
    if (!g_lw_log[0])
    {
        GetTempPathA(MAX_PATH, g_lw_log);
        strncat(g_lw_log, "xplatbase_tests_bench_alocadores_leak.log", MAX_PATH - strlen(g_lw_log) - 1);
    }
    return g_lw_log;
}

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
    mem_leak_watch_default_config(&cfg);
    cfg.log_path = lw_log();
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
    mem_leak_watch_default_config(&cfg);
    cfg.log_path = lw_log();
    cfg.interval_ms = 60000;
    mem_leak_watch_start(&cfg);
}
static void memop_lw_prod_g_fini(void)
{
    mem_leak_watch_scan_now();
    mem_leak_watch_stop();
    memop_shutdown();
}

#ifdef XPB_TEM_RPMALLOC
/* --- rpmalloc --- */
static void rp_g_init(void) { rpmalloc_initialize(0); }
static void rp_g_fini(void) { rpmalloc_finalize(); }
static void rp_t_init(void) { rpmalloc_thread_initialize(); }
static void rp_t_fini(void) { rpmalloc_thread_finalize(); }
static void* rp_a(size_t n) { return rpmalloc(n); }
static void  rp_f(void* p, size_t n) { (void)n; rpfree(p); }
#endif

#ifdef XPB_TEM_MIMALLOC
/* --- mimalloc --- */
static void* mi_a(size_t n) { return mi_malloc(n); }
static void  mi_f(void* p, size_t n) { (void)n; mi_free(p); }
#endif

static Alloc ALLOCS[] = {
    { "sys-malloc",     noop,             noop,              noop,      noop,      sys_alloc, sys_free },
    { "memop-pool",     memop_g_init,     memop_g_fini,      noop,      noop,      memop_a,   memop_f  },
    { "memop-lw-dbg",   memop_lw_dbg_g_init,  memop_lw_dbg_g_fini,  noop,  noop,  memop_a,   memop_f  },
    { "memop-lw-prod",  memop_lw_prod_g_init, memop_lw_prod_g_fini, noop,  noop,  memop_a,   memop_f  },
#ifdef XPB_TEM_RPMALLOC
    { "rpmalloc",       rp_g_init,        rp_g_fini,         rp_t_init, rp_t_fini, rp_a,      rp_f     },
#endif
#ifdef XPB_TEM_MIMALLOC
    { "mimalloc",       noop,             noop,              noop,      noop,      mi_a,      mi_f     },
#endif
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

    t_logf("  %-12s %10.3f %12.2f\n",
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
        t_logf("  %-12s %9.2f %7.1f %9.2f %7.1f %9.2f\n",
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

    t_logf("  %-12s %12.1f %12.1f %12.1f %12.1f\n",
           a->name, warm_alloc_ns, warm_free_ns, grow_ns, maxns);
    free(ptr); free(sz); free(ws); free(wsz);
}

static int env_int(const char* nome, int padrao)
{
    char* v = 0;
    size_t n = 0;
    int r = padrao;
    if (_dupenv_s(&v, &n, nome) == 0 && v) { if (v[0]) r = atoi(v); free(v); }
    return r;
}

void teste_memoria_bench_alocadores(TestResult* r)
{
    SYSTEM_INFO si; GetSystemInfo(&si);
    int hw = (int)si.dwNumberOfProcessors;
    int threads = hw < 8 ? hw : 8;
    int slots = 2000;
    long ops = 4000000;       /* por thread */
    long smallops = 30000000; /* 1 thread */
    int i;

    t_start(r);
#ifdef _DEBUG
    if (env_int("XPB_BENCH_DEBUG", 0) != 1)
        T_SKIP(r, "build Debug nao mede desempenho real (sem otimizacao, heap de Debug). Compile em Release. XPB_BENCH_DEBUG=1 forca.");
#endif
    threads = env_int("XPB_BENCH_THREADS", threads);
    ops     = (long)env_int("XPB_BENCH_OPS", (int)ops);

    t_logf("CPUs=%d  threads=%d  slots/thread=%d  ops/thread=%ld\n", hw, threads, slots, ops);
#ifndef XPB_TEM_RPMALLOC
    t_logf("(sem rpmalloc: bench/rpmalloc nao existe nesta copia)\n");
#endif
#ifndef XPB_TEM_MIMALLOC
    t_logf("(sem mimalloc: bench/mimalloc nao existe nesta copia)\n");
#endif

    t_logf("\n== Cenario A: Larson churn (free+alloc intercalado, multi-thread) ==\n");
    t_logf("  %-12s %10s %12s\n", "allocator", "tempo(s)", "vazao Mop/s");
    for (i = 0; i < NALLOC; i++) run_larson(&ALLOCS[i], threads, slots, ops);

    t_logf("\n== Cenario B: small fixed 64B (1 thread, %ld ops) ==\n", smallops);
    t_logf("  %-12s %9s %7s %9s %7s %9s\n",
           "allocator", "aloc Mop", "ns", "free Mop", "ns", "par Mop");
    for (i = 0; i < NALLOC; i++) run_smallfixed(&ALLOCS[i], smallops);

    t_logf("\n== Cenario C: latencia por chamada, em ns (1 thread, 2M blocos 16-256B) ==\n");
    t_logf("  %-12s %12s %12s %12s %12s\n",
           "allocator", "aloc quente", "free quente", "aloc cresc", "aloc pior");
    for (i = 0; i < NALLOC; i++) run_latency(&ALLOCS[i], 2000000);

    /* o bench desliga os ganchos de thread (thread_init(NULL, NULL)) e termina com o pool
     * desligado (memop_shutdown): restaura para os testes seguintes */
    memop_init();
    thread_init(memop_on_created_thread, memop_on_ended_thread);
}
