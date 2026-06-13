/*
 * pool_compare_bench.cpp — comparacao de LATENCIA submit->inicio-de-execucao
 * entre: WSPool (thread_pool.c), Windows Thread Pool (PTP) e Intel TBB.
 *
 * Metrica principal (espelha test_handoff): para cada task capturo o TSC ANTES
 * do submit (submit_tsc) e o TSC no PRIMEIRO instante dentro do callback
 * (exec_tsc). latencia = exec_tsc - submit_tsc, convertida p/ ns (TSC calibrado,
 * descontando o overhead do proprio rdtsc). Mede "quanto tempo a task leva p/
 * comecar a rodar" sob diferentes pressoes e complexidades.
 *
 * Cenarios:
 *   Pressao:     baixa / media / alta / ultra-alta   (produtores x volume)
 *   Complexidade: rapidas (0us) / medias (~5us) / mistas (rapidas + longas 50ms)
 *                 -> o cenario MISTO testa o que cada pool faz com task longa
 *                    (handoff no WSPool; crescimento de threads no PTP; TBB sem
 *                    handoff tende a enfileirar as rapidas atras das longas).
 *   Cada cenario roda N_REPS vezes; latencias agregadas (p50/p99/max), throughput
 *   = mediana das repeticoes.
 *
 * TBB: compilado so com -DBENCH_TBB (o build detecta). Sem TBB, roda WSPool+PTP.
 *
 * Build: ver build_compare.bat
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <intrin.h>

#include <atomic>
#include <thread>
#include <chrono>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>

#ifdef _WIN32
#pragma comment(lib, "psapi.lib")
#endif

/* O Tester.vcxproj define TBB_AVAILABLE; este arquivo usa BENCH_TBB — mapeia. */
#if defined(TBB_AVAILABLE) && !defined(BENCH_TBB)
#define BENCH_TBB 1
#endif

/* thread_pool.h ja tem guarda extern "C" propria — nao aninhar. */
#include "../Xplatbase/Xplatbase/src/thread_pool.h"

#ifdef BENCH_TBB
#include <tbb/task_arena.h>
#endif

/* ───────────────────────── TSC calibrado (reuso do exemplo) ───────────────── */
static double   g_tsc_ns_ratio = 0.0;   /* ns por ciclo */
static uint64_t g_tsc_overhead = 0;

static inline uint64_t tsc_now(void) { return __rdtsc(); }

static void tsc_calibrate(void)
{
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    uint64_t c0 = __rdtsc();
    QueryPerformanceCounter(&t0);
    Sleep(50);
    uint64_t c1 = __rdtsc();
    QueryPerformanceCounter(&t1);
    double ns  = (double)(t1.QuadPart - t0.QuadPart) * 1e9 / (double)freq.QuadPart;
    g_tsc_ns_ratio = ns / (double)(c1 - c0);

    uint64_t min_oh = UINT64_MAX;
    for (int i = 0; i < 1000; i++) {
        uint64_t a = __rdtsc(), b = __rdtsc();
        if (b - a < min_oh) min_oh = b - a;
    }
    g_tsc_overhead = min_oh;
}

static inline double tsc_to_ns(uint64_t start, uint64_t end)
{
    uint64_t c = end - start;
    if (c > g_tsc_overhead) c -= g_tsc_overhead; else c = 0;
    return (double)c * g_tsc_ns_ratio;
}

static uint64_t now_ns(void)
{
    static LARGE_INTEGER f = { 0 };
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)c.QuadPart * 1000000000ULL / (uint64_t)f.QuadPart;
}

/* ───────────────────────── Task e trabalho simulado ───────────────────────── */
struct BTask {
    uint64_t submit_tsc;
    uint64_t exec_tsc;
    uint32_t work_us;      /* duracao do trabalho (0 = rapida) */
};

static std::atomic<long long> g_done{0};

static inline void busy_us(uint32_t us)
{
    if (!us) return;
    uint64_t end = __rdtsc() + (uint64_t)((double)us * 1000.0 / g_tsc_ns_ratio);
    volatile uint64_t x = 0;
    while (__rdtsc() < end) { x += 1; }
    (void)x;
}

/* Captura exec_tsc (latencia) ANTES de gastar o tempo de trabalho. */
static inline void run_task(BTask* t)
{
    t->exec_tsc = tsc_now();
    busy_us(t->work_us);
    g_done.fetch_add(1, std::memory_order_relaxed);
}

/* ───────────────────────── Adapters ───────────────────────────────────────── */
typedef void* (*pool_create_fn)(int nthreads);
typedef void  (*pool_submit_fn)(void* ctx, BTask* t);
typedef void  (*pool_destroy_fn)(void* ctx);

struct Adapter {
    const char*     name;
    pool_create_fn  create;
    pool_submit_fn  submit;
    pool_destroy_fn destroy;
    const char*     budget;   /* descricao do orcamento de threads */
};

/* ---- WSPool ---- */
static void ws_cb(void* a) { run_task((BTask*)a); }
static void* ws_create(int) {
    PoolConfig cfg = pool_default_config();   /* shard=ncores, reserva+handoff */
    return pool_create(&cfg);
}
static void ws_submit(void* ctx, BTask* t) {
    t->submit_tsc = tsc_now();
    pool_submit((ShardedPool*)ctx, ws_cb, t);   /* externo: aplica backpressure, nao falha */
}
static void ws_destroy(void* ctx) { pool_shutdown((ShardedPool*)ctx); pool_destroy((ShardedPool*)ctx); }

/* ---- WSPool BALANCEADO: tenta cortar o MAX sem perder o p50 ----
 * Combina os dois levers:
 *   - folga de cores: shard_count = cores-4 → produtores/monitores rodam sem
 *     preemptar workers spinando (ataca a causa do MAX em baixa pressao);
 *   - menos spin / park cedo: libera core quando ocioso de verdade;
 *   - park_deep_sleep capado: teto do pior-caso de despertar. */
static void* ws_bal_create(int) {
    PoolConfig cfg = pool_default_config();
    int nc = xcpu_count();
    cfg.shard_count            = (nc > 5) ? nc - 4 : 1;
    cfg.max_auto_expand_lanes  = cfg.shard_count;   /* nao re-satura expandindo */
    cfg.spin_budget_us         = 250;               /* spin curto: menos saturacao */
    cfg.park_idle_threshold_ms = 20;                /* parquear cedo libera cores */
    //cfg.park_deep_sleep_us     = 5000;              /* teto do wake = 5ms */
    return pool_create(&cfg);
}

/* ---- Windows Thread Pool (PTP) ---- */
struct PtpCtx { PTP_POOL pool; TP_CALLBACK_ENVIRON env; };
static void CALLBACK ptp_cb(PTP_CALLBACK_INSTANCE, PVOID ctx) { run_task((BTask*)ctx); }
static void* ptp_create(int n) {
    PtpCtx* c = new PtpCtx();
    c->pool = CreateThreadpool(NULL);
    SetThreadpoolThreadMinimum(c->pool, n);
    SetThreadpoolThreadMaximum(c->pool, n * 2);   /* PTP pode crescer (design dele) */
    InitializeThreadpoolEnvironment(&c->env);
    SetThreadpoolCallbackPool(&c->env, c->pool);
    return c;
}
static void ptp_submit(void* ctx, BTask* t) {
    PtpCtx* c = (PtpCtx*)ctx;
    t->submit_tsc = tsc_now();
    while (!TrySubmitThreadpoolCallback(ptp_cb, t, &c->env)) Sleep(0);
}
static void ptp_destroy(void* ctx) {
    PtpCtx* c = (PtpCtx*)ctx;
    DestroyThreadpoolEnvironment(&c->env);
    CloseThreadpool(c->pool);
    delete c;
}

/* ---- Intel TBB ---- */
#ifdef BENCH_TBB
static void* tbb_create(int n) { auto* a = new tbb::task_arena(n); a->initialize(); return a; }
static void  tbb_submit(void* ctx, BTask* t) {
    auto* a = (tbb::task_arena*)ctx;
    t->submit_tsc = tsc_now();
    a->enqueue([t] { run_task(t); });
}
static void  tbb_destroy(void* ctx) { delete (tbb::task_arena*)ctx; }
#endif

/* ───────────────────────── Cenarios ───────────────────────────────────────── */
struct Scenario {
    const char* pressure;
    const char* complexity;
    int         producers;
    int         total;
    uint32_t    work_us;       /* trabalho da task base */
    int         long_every;    /* >0: 1 a cada N e longa */
    uint32_t    long_work_us;  /* duracao da task longa */
    int         bursts;        /* >1: envia em N rajadas (ondas) */
    int         gap_ms;        /* pausa ociosa entre rajadas */
};

#define N_REPS 5
#define LONG_BUCKET_US 1000   /* >= isso -> bucket "longa" no agregado */

static double pct(std::vector<double>& v, double p)
{
    if (v.empty()) return 0.0;
    size_t i = (size_t)(p * (double)(v.size() - 1) + 0.5);
    if (i >= v.size()) i = v.size() - 1;
    return v[i];
}
static double median(std::vector<double>& v)
{
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    return (n & 1) ? v[n/2] : (v[n/2-1] + v[n/2]) / 2.0;
}

struct ProducerArg {
    Adapter*           ad;
    void*              ctx;
    BTask*             tasks;
    int                from, to;
    std::atomic<int>*  gate;
};

static void producer_run(ProducerArg a)
{
    while (a.gate->load(std::memory_order_acquire) == 0) _mm_pause();
    for (int i = a.from; i < a.to; i++)
        a.ad->submit(a.ctx, &a.tasks[i]);
}

/* Roda UM cenario com UM adapter, N_REPS vezes. Acumula latencias (fast/long) e
 * a mediana do throughput. */
static void run_scenario(Adapter* ad, const Scenario& s,
                         std::vector<double>& fast_lat,
                         std::vector<double>& long_lat,
                         double& thr_mtps_median)
{
    std::vector<BTask> tasks(s.total);
    std::vector<double> thr_samples;
    int ncores = (int)std::thread::hardware_concurrency();

    /* Pool criado UMA vez e reusado entre reps — reflete o uso real (pool de vida
     * longa). Antes o create/destroy por rep media cold-start (criacao de ~30
     * threads), nao o regime estavel. */
    void* ctx = ad->create(ncores);
    if (!ctx) { printf("    %s: create FALHOU\n", ad->name); return; }

    /* Warmup descartado: agenda todos os workers e aquece caches antes de medir. */
    {
        int wn = s.total < 4000 ? s.total : 4000;
        for (int i = 0; i < wn; i++) { tasks[i].submit_tsc = 0; tasks[i].exec_tsc = 0; tasks[i].work_us = 0; }
        g_done.store(0, std::memory_order_relaxed);
        std::atomic<int> gate{0};
        std::vector<std::thread> wp;
        int per = wn / s.producers; if (per < 1) per = 1;
        for (int p = 0; p < s.producers; p++) {
            int from = p * per; if (from >= wn) break;
            int to = (p == s.producers - 1) ? wn : from + per;
            wp.emplace_back(producer_run, ProducerArg{ ad, ctx, tasks.data(), from, to, &gate });
        }
        gate.store(1, std::memory_order_release);
        while (g_done.load(std::memory_order_relaxed) < wn) _mm_pause();
        for (auto& th : wp) th.join();
    }

    for (int rep = 0; rep < N_REPS; rep++) {
        for (int i = 0; i < s.total; i++) {
            tasks[i].submit_tsc = 0;
            tasks[i].exec_tsc   = 0;
            tasks[i].work_us    = (s.long_every > 0 && (i % s.long_every) == 0)
                                ? s.long_work_us : s.work_us;
        }
        g_done.store(0, std::memory_order_relaxed);

        int waves    = (s.bursts > 1) ? s.bursts : 1;
        int per_wave = s.total / waves;
        uint64_t busy_ns = 0;   /* tempo ativo (exclui os gaps das rajadas) */

        for (int w = 0; w < waves; w++) {
            if (w > 0 && s.gap_ms > 0)                    /* gap ocioso: workers parqueiam */
                std::this_thread::sleep_for(std::chrono::milliseconds(s.gap_ms));

            int wfrom = w * per_wave;
            int wto   = (w == waves - 1) ? s.total : wfrom + per_wave;

            std::atomic<int> gate{0};
            std::vector<std::thread> prod;
            int span = wto - wfrom;
            int per  = span / s.producers; if (per < 1) per = 1;
            for (int p = 0; p < s.producers; p++) {
                int from = wfrom + p * per;
                if (from >= wto) break;
                int to = (p == s.producers - 1) ? wto : from + per;
                ProducerArg pa{ ad, ctx, tasks.data(), from, to, &gate };
                prod.emplace_back(producer_run, pa);
            }

            uint64_t wb = now_ns();
            gate.store(1, std::memory_order_release);
            while (g_done.load(std::memory_order_relaxed) < wto) _mm_pause();
            busy_ns += now_ns() - wb;

            for (auto& th : prod) th.join();
        }

        double wall_s = (double)busy_ns / 1e9;
        thr_samples.push_back((double)s.total / wall_s / 1e6);

        for (int i = 0; i < s.total; i++) {
            if (!tasks[i].exec_tsc) continue;
            double lat = tsc_to_ns(tasks[i].submit_tsc, tasks[i].exec_tsc);
            if (tasks[i].work_us >= LONG_BUCKET_US) long_lat.push_back(lat);
            else                                     fast_lat.push_back(lat);
        }
    }

    ad->destroy(ctx);
    thr_mtps_median = median(thr_samples);
}

/* Indicador rapido de memoria: working set delta ao criar+aquecer o pool. */
static void memory_probe(Adapter* ad)
{
    int ncores = (int)std::thread::hardware_concurrency();
    PROCESS_MEMORY_COUNTERS m0, m1;
    GetProcessMemoryInfo(GetCurrentProcess(), &m0, sizeof(m0));
    void* ctx = ad->create(ncores);
    g_done.store(0, std::memory_order_relaxed);
    std::vector<BTask> warm(2000);
    for (int i = 0; i < 2000; i++) { warm[i].work_us = 0; ad->submit(ctx, &warm[i]); }
    while (g_done.load() < 2000) _mm_pause();
    GetProcessMemoryInfo(GetCurrentProcess(), &m1, sizeof(m1));
    double mb = (double)((long long)m1.WorkingSetSize - (long long)m0.WorkingSetSize) / (1024.0*1024.0);
    printf("  %-10s working-set delta ~ %.2f MB (orcamento: %s)\n", ad->name, mb, ad->budget);
    ad->destroy(ctx);
}

int main(void)
{
    tsc_calibrate();
    int ncores = (int)std::thread::hardware_concurrency();

    printf("################################################################\n");
    printf("  COMPARACAO de latencia submit->exec  (ncpu=%d, reps=%d)\n", ncores, N_REPS);
    printf("  metrica: tempo do submit ate o 1o instante dentro do callback (tabela em ms)\n");
    printf("################################################################\n");

    static Adapter ws    = { "WSPool", ws_create,     ws_submit, ws_destroy, "shard=ncores + reserva + handoff/expand" };
    static Adapter wsbal = { "WSbal",  ws_bal_create, ws_submit, ws_destroy, "shard=cores-4, spin=250us, park=20ms" };
    static Adapter ptp   = { "WinTP",  ptp_create,    ptp_submit, ptp_destroy, "min=ncores, max=2*ncores (cresce)" };
    std::vector<Adapter*> ads = { &ws, &wsbal, &ptp };
#ifdef BENCH_TBB
    static Adapter tbb = { "TBB", tbb_create, tbb_submit, tbb_destroy, "arena=ncores (sem crescimento)" };
    ads.push_back(&tbb);
#else
    printf("  [TBB ausente: compile com -DBENCH_TBB p/ incluir]\n");
#endif

    printf("\n--- memoria (indicador) ---\n");
    for (auto* ad : ads) memory_probe(ad);

    const int NC = ncores;
    std::vector<Scenario> scns = {
        /* pressao  complexidade      prod   total    work long_every long_us  bursts gap_ms */
        { "baixa", "rapida",        1,      30000,   0,    0,     0,       1,   0 },
        { "media", "rapida",        4,      200000,  0,    0,     0,       1,   0 },
        { "alta",  "rapida",        NC,     500000,  0,    0,     0,       1,   0 },
        { "ultra", "rapida",        NC*2,   1000000, 0,    0,     0,       1,   0 },

        { "baixa", "media",         1,      15000,   5,    0,     0,       1,   0 },
        { "media", "media",         4,      60000,   5,    0,     0,       1,   0 },
        { "alta",  "media",         NC,     200000,  5,    0,     0,       1,   0 },
        { "ultra", "media",         NC*2,   400000,  5,    0,     0,       1,   0 },

        { "baixa", "mista",         1,      15000,   0,    500,   50000,   1,   0 },
        { "media", "mista",         4,      40000,   0,    500,   50000,   1,   0 },
        { "alta",  "mista",         NC,     80000,   0,    500,   50000,   1,   0 },
        { "ultra", "mista",         NC*2,   150000,  0,    500,   50000,   1,   0 },

        /* saturacao: produtores >> cores, volume alto (forca ring cheio/backpressure) */
        { "alta",  "satur",         NC*4,   600000,  0,    0,     0,       1,   0 },
        { "ultra", "satur",         NC*8,   1000000, 0,    0,     0,       1,   0 },

        /* rajadas: N ondas com gap ocioso (exercita park/wake e handoff em transicao) */
        { "media", "rajada-curta",  4,      200000,  0,    0,     0,       10,  30 },
        { "media", "rajada-mista",  4,      100000,  0,    500,   50000,   10,  30 },

        /* mista massiva: milhoes de curtas + milhares de longas de 50ms */
        { "alta",  "mista-massiva", NC,     1000000, 0,    2000,  50000,   1,   0 },
        { "ultra", "mista-massiva", NC*2,   1500000, 0,    2000,  50000,   1,   0 },
    };

    bool has_tbb = false;
    for (auto* ad : ads) if (std::strcmp(ad->name, "TBB") == 0) has_tbb = true;

    /* Roda cada cenario UMA vez por lib e guarda p50/p99/max (ms). Depois imprime
     * 3 tabelas no mesmo formato (libs em colunas + R1/R2 por metrica). */
    struct LibMetrics { double p50, p99, p999, mx; };
    struct ScenResult { LibMetrics ws, wsbal, ptp, tbb; };
    std::vector<ScenResult> results(scns.size());
    for (auto& r : results) {
        LibMetrics none = { -1.0, -1.0, -1.0, -1.0 };
        r.ws = r.wsbal = r.ptp = r.tbb = none;
    }

    for (size_t si = 0; si < scns.size(); si++) {
        for (auto* ad : ads) {
            std::vector<double> fast, lng;
            double thr = 0;
            run_scenario(ad, scns[si], fast, lng, thr);
            std::sort(fast.begin(), fast.end());
            LibMetrics m;
            m.p50  = pct(fast, 0.50)  / 1e6;            /* ns -> ms */
            m.p99  = pct(fast, 0.99)  / 1e6;
            m.p999 = pct(fast, 0.999) / 1e6;
            m.mx   = (fast.empty() ? 0.0 : fast.back()) / 1e6;
            if      (std::strcmp(ad->name, "WSPool") == 0) results[si].ws    = m;
            else if (std::strcmp(ad->name, "WSbal")  == 0) results[si].wsbal = m;
            else if (std::strcmp(ad->name, "WinTP")  == 0) results[si].ptp   = m;
            else if (std::strcmp(ad->name, "TBB")    == 0) results[si].tbb   = m;
        }
    }

    /* Imprime uma tabela para a metrica escolhida (ponteiro-para-membro).
     * Colunas: WSPool (default) | WSbal (tunado) | WinTP | TBB.
     * Ratios relativos ao WSPool default:
     *   Rbal = WSbal/WSPool  (<1 = balanceado melhorou),  Rwin = WinTP/WSPool,
     *   Rtbb = TBB/WSPool. */
    auto print_table = [&](const char* title, double LibMetrics::* field) {
        printf("\n=== %s (latencia submit->exec das tasks rapidas, ms) ===\n", title);
        printf("  Rbal = WSbal/WSPool (<1 = melhor)   Rwin = WinTP/WSPool   Rtbb = TBB/WSPool\n\n");
        printf("%-22s %15s %15s %15s %15s %11s %11s %11s\n",
               "TESTE", "WSPool", "WSbal", "WinTP", "TBB", "Rbal", "Rwin", "Rtbb");
        const char* prev = nullptr;
        for (size_t si = 0; si < scns.size(); si++) {
            if (prev && std::strcmp(prev, scns[si].complexity) != 0) printf("\n");
            prev = scns[si].complexity;

            double ws    = results[si].ws.*field;
            double wsbal = results[si].wsbal.*field;
            double ptp   = results[si].ptp.*field;
            double tbb   = results[si].tbb.*field;

            char label[40];
            snprintf(label, sizeof(label), "%s/%s", scns[si].complexity, scns[si].pressure);

            printf("%-22s %15.6f %15.6f %15.6f", label, ws, wsbal, ptp);
            if (has_tbb) printf(" %15.6f", tbb); else printf(" %15s", "-");

            double rbal = (ws > 0) ? wsbal / ws : 0.0;
            double rwin = (ws > 0) ? ptp   / ws : 0.0;
            printf(" %11.3f %11.3f", rbal, rwin);
            if (has_tbb) { double rtbb = (ws > 0) ? tbb / ws : 0.0; printf(" %11.3f\n", rtbb); }
            else         { printf(" %11s\n", "-"); }
        }
        fflush(stdout);
    };

    print_table("p50 (mediana)",  &LibMetrics::p50);
    print_table("p99",            &LibMetrics::p99);
    print_table("p999",           &LibMetrics::p999);
    print_table("MAX (pior caso)", &LibMetrics::mx);

    printf("\nFim.\n");
    return 0;
}
