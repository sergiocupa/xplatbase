#include "bench_pool.h"
#include "bench_estatistica.h"
#include "memory_pool.h"

#include <windows.h>
#include <intrin.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define LIMITE_ESPERA_MS 60000.0     // uma tarefa que nao termina em 60 s = pool travado

// ---- relogio e CPU ----------------------------------------------------------------------

static double g_qpc_por_ms;
static double g_tsc_hz;

static LONG64 qpc(void)            { LARGE_INTEGER c; QueryPerformanceCounter(&c); return c.QuadPart; }
static double ms_entre(LONG64 a, LONG64 b) { return (double)(b - a) / g_qpc_por_ms; }

static void espera_us(int us)
{
    LONG64 fim;
    if (us <= 0) return;
    fim = qpc() + (LONG64)((double)us * g_qpc_por_ms / 1000.0);
    while (qpc() < fim) YieldProcessor();
}

double bench_tsc_hz(void)
{
    if (g_tsc_hz == 0.0)
    {
        LARGE_INTEGER f, a, b;
        unsigned __int64 t0, t1;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&a); t0 = __rdtsc();
        Sleep(200);
        QueryPerformanceCounter(&b); t1 = __rdtsc();
        g_tsc_hz = (double)(t1 - t0) / ((double)(b.QuadPart - a.QuadPart) / (double)f.QuadPart);
    }
    return g_tsc_hz;
}

// CPU do processo em CICLOS, nao o GetProcessTimes: aquele cobra por tique de relogio, e
// com o timer a 1 ms (o pool eleva a resolucao) um worker que acorda no tique, gira e volta
// a dormir antes do proximo nunca e cobrado -- media 0,000 nucleo para um pool que gastava
// 2,5. QueryProcessCycleTime conta os ciclos de verdade.
static double cpu_s(void)
{
    ULONG64 ciclos = 0;
    QueryProcessCycleTime(GetCurrentProcess(), &ciclos);
    return (double)ciclos / bench_tsc_hz();
}

// ---- pool da medida corrente (as medidas sao sequenciais) ---------------------------------

static const PoolVersao* g_v;
static ThreadPool*       g_p;

static void submete(pool_task_fn fn, void* arg)
{
    while (!g_v->Submeter(g_p, fn, arg)) YieldProcessor();
}

static int aguarda_contador(volatile LONG* c, LONG alvo)
{
    LONG64 t0 = qpc();
    while (*c < alvo)
    {
        if (ms_entre(t0, qpc()) > LIMITE_ESPERA_MS) return 0;
        Sleep(0);
    }
    return 1;
}

static int aguarda_zero(volatile LONG64* c)
{
    LONG64 t0 = qpc();
    while (*c > 0)
    {
        if (ms_entre(t0, qpc()) > LIMITE_ESPERA_MS) return 0;
        Sleep(0);
    }
    return 1;
}

static uint32_t lcg(uint32_t* s) { *s = *s * 1664525u + 1013904223u; return *s; }

// ---- latencias --------------------------------------------------------------------------

static double* g_lat;
static int     g_lat_cap;

static int compara_double(const void* a, const void* b)
{
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

// Mesmas definicoes do thread_pool_bench: percentil = v[n*q] da lista ordenada,
// max10 = media das 10 maiores.
static void percentis(double* lat, int n, Medida* m)
{
    int k, i;
    double s = 0;
    if (n <= 0) return;
    qsort(lat, (size_t)n, sizeof(double), compara_double);
    m->V[MET_P50]  = lat[(size_t)((double)n * 0.5)];
    m->V[MET_P90]  = lat[(size_t)((double)n * 0.9)];
    m->V[MET_P99]  = lat[(size_t)((double)n * 0.99)];
    m->V[MET_P999] = lat[(size_t)((double)n * 0.999)];
    m->V[MET_MAX]  = lat[n - 1];
    k = n < 10 ? n : 10;
    for (i = n - k; i < n; i++) s += lat[i];
    m->V[MET_MAX10] = s / k;
}

// ---- flat -------------------------------------------------------------------------------

typedef struct { LONG64 Enq, Ini; int WorkUs, AlocN, AlocPool; uint32_t Semente; } Tarefa;

static Tarefa*       g_tar;
static volatile LONG g_feitas;

static void aloca_malloc(int n, uint32_t semente)
{
    void* p[16];
    int k = n > 16 ? 16 : n, i;
    for (i = 0; i < k; i++)
    {
        uint32_t sz = 64u + (lcg(&semente) % 1985u);
        p[i] = malloc(sz);
        if (p[i]) { ((volatile char*)p[i])[0] = (char)i; ((volatile char*)p[i])[sz - 1] = (char)sz; }
    }
    for (i = 0; i < k; i++) free(p[i]);
}

static void aloca_pool(int n, uint32_t semente)
{
    void* p[16];
    int k = n > 16 ? 16 : n, i;
    for (i = 0; i < k; i++)
    {
        uint32_t sz = 64u + (lcg(&semente) % 1985u);
        p[i] = memop_alloc_raw(sz);
        if (p[i]) { ((volatile char*)p[i])[0] = (char)i; ((volatile char*)p[i])[sz - 1] = (char)sz; }
    }
    for (i = 0; i < k; i++) memop_free_raw(p[i]);
}

static void t_flat(void* a)
{
    Tarefa* t = (Tarefa*)a;
    t->Ini = qpc();
    if      (t->AlocN && t->AlocPool) aloca_pool(t->AlocN, t->Semente);
    else if (t->AlocN)                aloca_malloc(t->AlocN, t->Semente);
    else                              espera_us(t->WorkUs);
    InterlockedIncrement(&g_feitas);
}

static int mede_flat(const CenarioPool* c, Medida* m)
{
    int n = c->Tarefas, i, ok;
    uint32_t rnd = 0x12345678u;
    double c0, c1, wall;
    LONG64 t0, t1;

    // mesma geracao de carga do thread_pool_bench (fill_work)
    for (i = 0; i < n; i++)
    {
        int w = c->WMin;
        if (c->WMax > c->WMin) { int span = c->WMax - c->WMin + 1; w = c->WMin + (int)(lcg(&rnd) % (uint32_t)span); }
        if (c->LongaCada > 0 && (i % c->LongaCada) == 0) w = c->LongaUs;
        g_tar[i].WorkUs   = w;
        g_tar[i].AlocN    = c->Alocs;
        g_tar[i].AlocPool = c->AlocPool;
        g_tar[i].Semente  = 0x9E3779B9u * (uint32_t)(i + 1);
        g_tar[i].Ini      = 0;
    }
    InterlockedExchange(&g_feitas, 0);

    c0 = cpu_s(); t0 = qpc();
    for (i = 0; i < n; i++) { g_tar[i].Enq = qpc(); submete(t_flat, &g_tar[i]); }
    ok = aguarda_contador(&g_feitas, n);
    t1 = qpc(); c1 = cpu_s();
    if (!ok) return 0;

    wall = ms_entre(t0, t1);
    for (i = 0; i < n; i++) g_lat[i] = ms_entre(g_tar[i].Enq, g_tar[i].Ini) * 1000.0;
    m->V[MET_WALL]    = wall;
    m->V[MET_NUCLEOS] = (c1 - c0) / (wall / 1000.0);
    percentis(g_lat, n, m);
    return 1;
}

// ---- spawn (arvore) ---------------------------------------------------------------------

static int*            g_arv_ini;
static int*            g_arv_qtd;
static int             g_arv_n;
static LONG64*         g_no_enq;
static LONG64*         g_no_ini;
static volatile LONG64 g_pendentes;

static void t_no(void* a);

static void submete_no(int idx)
{
    InterlockedIncrement64(&g_pendentes);
    g_no_enq[idx] = qpc();
    submete(t_no, (void*)(intptr_t)idx);
}

static void t_no(void* a)
{
    int i = (int)(intptr_t)a, j;
    int s = g_arv_ini[i], k = g_arv_qtd[i];
    g_no_ini[i] = qpc();
    for (j = 0; j < k; j++) submete_no(s + j);
    InterlockedDecrement64(&g_pendentes);
}

// Mesma forma do build_tree do thread_pool_bench: largura primeiro, 1..4 filhos por no.
static void monta_arvore(int alvo)
{
    uint32_t rnd = 12345u;
    int total = 1, i;
    g_arv_ini = (int*)calloc((size_t)alvo, sizeof(int));
    g_arv_qtd = (int*)calloc((size_t)alvo, sizeof(int));
    g_no_enq  = (LONG64*)calloc((size_t)alvo, sizeof(LONG64));
    g_no_ini  = (LONG64*)calloc((size_t)alvo, sizeof(LONG64));
    for (i = 0; i < total && total < alvo; i++)
    {
        int k = 1 + (int)(lcg(&rnd) % 4u);
        if (k > alvo - total) k = alvo - total;
        g_arv_ini[i] = total;
        g_arv_qtd[i] = k;
        total += k;
    }
    g_arv_n = total;
}

static int mede_spawn(Medida* m)
{
    int i, n = 0, ok;
    double c0, c1, wall;
    LONG64 t0, t1;

    memset(g_no_ini, 0, sizeof(LONG64) * (size_t)g_arv_n);
    InterlockedExchange64(&g_pendentes, 0);
    c0 = cpu_s(); t0 = qpc();
    submete_no(0);
    ok = aguarda_zero(&g_pendentes);
    t1 = qpc(); c1 = cpu_s();
    if (!ok) return 0;

    wall = ms_entre(t0, t1);
    for (i = 0; i < g_arv_n; i++)
        if (g_no_ini[i]) g_lat[n++] = ms_entre(g_no_enq[i], g_no_ini[i]) * 1000.0;
    m->V[MET_WALL]    = wall;
    m->V[MET_NUCLEOS] = (c1 - c0) / (wall / 1000.0);
    percentis(g_lat, n, m);
    return 1;
}

// ---- pool parado ------------------------------------------------------------------------

static volatile LONG   g_marca;
static volatile LONG64 g_t_inicio;
static pool_task_fn    g_filha;

static void t_marca(void* a) { (void)a; g_t_inicio = qpc(); InterlockedExchange(&g_marca, 1); }

static void t_filha_cpu(void* a)
{
    volatile double x = 0;
    int i;
    (void)a;
    for (i = 0; i < 400000; i++) x += i * 0.5;
    InterlockedDecrement64(&g_pendentes);
}

static void t_filha_tempo(void* a) { (void)a; espera_us(500); InterlockedDecrement64(&g_pendentes); }

static void t_mae(void* a)
{
    int n = (int)(intptr_t)a, i;
    for (i = 0; i < n; i++) submete(g_filha, 0);
}

static int mede_cpu_ocioso(Medida* m)
{
    double c0, c1;
    Sleep(700);                          // deixa o pool assentar (o recuo leva ~60 ms)
    c0 = cpu_s();
    Sleep(2000);
    c1 = cpu_s();
    m->V[MET_NUCLEOS] = (c1 - c0) / 2.0;
    return 1;
}

static int mede_despertar(Medida* m)
{
    double amostra[6], mx = 0;
    int i;
    Sleep(300);
    for (i = 0; i < 6; i++)
    {
        LONG64 t0;
        Sleep(200);
        InterlockedExchange(&g_marca, 0);
        t0 = qpc();
        submete(t_marca, 0);
        while (!g_marca)
        {
            if (ms_entre(t0, qpc()) > LIMITE_ESPERA_MS) return 0;
            SwitchToThread();
        }
        amostra[i] = ms_entre(t0, g_t_inicio) * 1000.0;
        if (amostra[i] > mx) mx = amostra[i];
    }
    m->V[MET_P50] = est_mediana(amostra, 6);
    m->V[MET_MAX] = mx;
    return 1;
}

static double fork_join(pool_task_fn filha, int* ok)
{
    LONG64 t0;
    g_filha = filha;
    InterlockedExchange64(&g_pendentes, 64);
    t0 = qpc();
    submete(t_mae, (void*)(intptr_t)64);
    *ok = aguarda_zero(&g_pendentes);
    return ms_entre(t0, qpc());
}

static int mede_fj_ocioso(pool_task_fn filha, Medida* m)
{
    int ok;
    Sleep(1000);
    m->V[MET_WALL] = fork_join(filha, &ok);
    return ok;
}

static int mede_fj_regime(Medida* m)
{
    int ok, i;
    double soma = 0;
    Sleep(100);
    fork_join(t_filha_cpu, &ok);                          // aquecimento
    for (i = 0; ok && i < 20; i++) soma += fork_join(t_filha_cpu, &ok);
    m->V[MET_WALL] = soma / 20.0;
    return ok;
}

// ---- entrada ----------------------------------------------------------------------------

void bench_pool_prepara(int max_tarefas, int nos_arvore)
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpc_por_ms = (double)f.QuadPart / 1000.0;
    bench_tsc_hz();

    if (!g_tar) g_tar = (Tarefa*)calloc((size_t)max_tarefas, sizeof(Tarefa));
    if (!g_arv_ini) monta_arvore(nos_arvore);
    if (!g_lat)
    {
        g_lat_cap = max_tarefas > nos_arvore ? max_tarefas : nos_arvore;
        g_lat = (double*)malloc(sizeof(double) * (size_t)g_lat_cap);
    }
}

void bench_pool_metricas(TipoCenario t, int aplica[N_METRICAS])
{
    int i;
    for (i = 0; i < N_METRICAS; i++) aplica[i] = 0;
    switch (t)
    {
    case CEN_FLAT:
    case CEN_SPAWN:
        for (i = 0; i < N_METRICAS; i++) aplica[i] = 1;
        break;
    case CEN_CPU_OCIOSO:
        aplica[MET_NUCLEOS] = 1;
        break;
    case CEN_DESPERTAR:
        aplica[MET_P50] = aplica[MET_MAX] = 1;
        break;
    default:                             // fork-join
        aplica[MET_WALL] = 1;
        break;
    }
}

int bench_pool_mede(const CenarioPool* c, const PoolVersao* v, Medida* m)
{
    int ok = 0, i;
    for (i = 0; i < N_METRICAS; i++) m->V[i] = 0.0;
    if (c->Tipo == CEN_FLAT && c->Tarefas > g_lat_cap) return 0;

    g_v = v;
    g_p = v->Criar(0);
    switch (c->Tipo)
    {
    case CEN_FLAT:            ok = mede_flat(c, m);                  break;
    case CEN_SPAWN:           ok = mede_spawn(m);                    break;
    case CEN_CPU_OCIOSO:      ok = mede_cpu_ocioso(m);               break;
    case CEN_DESPERTAR:       ok = mede_despertar(m);                break;
    case CEN_FJ_OCIOSO_CPU:   ok = mede_fj_ocioso(t_filha_cpu, m);   break;
    case CEN_FJ_OCIOSO_TEMPO: ok = mede_fj_ocioso(t_filha_tempo, m); break;
    case CEN_FJ_REGIME:       ok = mede_fj_regime(m);                break;
    }
    // Pool travado nao e destruido: o destroy esperaria para sempre pela tarefa presa.
    if (ok) v->Destruir(g_p);
    g_p = 0;
    return ok;
}
