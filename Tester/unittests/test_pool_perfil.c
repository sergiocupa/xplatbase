//  Perfil do pool de tarefas (pool_perfil_relative): performance (padrao) x economia.
//
//  Verifica que:
//    - o padrao e PERFORMANCE e o perfil troca na hora, nos dois sentidos;
//    - em ECONOMIA nenhuma tarefa se perde nem atrasa por causa do monitor dormindo: depois
//      de um tempo parado, a primeira tarefa roda logo (o submit acorda quem precisa);
//    - em ECONOMIA, com o pool parado, a CPU do processo nao e maior que em PERFORMANCE
//      (o monitor deixa de acordar a cada POOL_MON_MS);
//    - em ECONOMIA, com o pool parado, as threads acordam pouco (trocas de contexto/s);
//    - rajadas intermitentes nao deixam os elasticos girando, nos dois perfis.
//  CPU em ciclos (QueryProcessCycleTime), como no teste do vigia.

#include "ctest_core.h"
#include "testes.h"
#include "bench_pool.h"          // bench_tsc_hz
#include "thread_pool.h"
#include "atomics.h"

#include <windows.h>

static double nucleos_em(int ms)
{
    ULONG64 c0 = 0, c1 = 0;
    QueryProcessCycleTime(GetCurrentProcess(), &c0);
    Sleep((DWORD)ms);
    QueryProcessCycleTime(GetCurrentProcess(), &c1);
    return (double)(c1 - c0) / bench_tsc_hz() / (ms / 1000.0);
}

static xatomic_int g_feitas;
static void conta(void* a) { (void)a; atomic_add_inline(&g_feitas, 1); }

static int espera_feitas(int alvo, int ms)
{
    for (int i = 0; i < ms; i++) { if (atomic_get_inline(&g_feitas) >= alvo) return 1; Sleep(1); }
    return atomic_get_inline(&g_feitas) >= alvo;
}

void teste_pool_perfil_economia_e_performance(TestResult* r)
{
    t_start(r);
    bench_tsc_hz();
    ThreadPool* p = pool_create_relative(0);
    T_ASSERT(r, p != 0, "pool_create_relative falhou");
    T_ASSERT(r, pool_perfil_atual_relative(p) == POOL_PERFIL_PERFORMANCE, "padrao deveria ser PERFORMANCE");

    Sleep(300);
    double cpu_perf = nucleos_em(1000);

    pool_perfil_relative(p, POOL_PERFIL_ECONOMIA);
    T_ASSERT(r, pool_perfil_atual_relative(p) == POOL_PERFIL_ECONOMIA, "perfil nao mudou para ECONOMIA");

    // rajada: tudo executa
    atomic_set_inline(&g_feitas, 0);
    for (int i = 0; i < 20000; i++) pool_submit_relative(p, conta, 0);
    T_ASSERT(r, espera_feitas(20000, 5000), "economia: so %d de 20000 tarefas", atomic_get_inline(&g_feitas));

    // parado: o monitor dorme; mede a CPU
    Sleep(300);
    double cpu_eco = nucleos_em(1000);

    // depois de parado, a primeira tarefa roda logo
    atomic_set_inline(&g_feitas, 0);
    LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    pool_submit_relative(p, conta, 0);
    T_ASSERT(r, espera_feitas(1, 1000), "economia: tarefa depois de parado nao rodou");
    QueryPerformanceCounter(&t1);
    double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;

    // volta para performance, na hora
    pool_perfil_relative(p, POOL_PERFIL_PERFORMANCE);
    T_ASSERT(r, pool_perfil_atual_relative(p) == POOL_PERFIL_PERFORMANCE, "perfil nao voltou para PERFORMANCE");
    atomic_set_inline(&g_feitas, 0);
    for (int i = 0; i < 20000; i++) pool_submit_relative(p, conta, 0);
    T_ASSERT(r, espera_feitas(20000, 5000), "performance: so %d de 20000 tarefas", atomic_get_inline(&g_feitas));

    pool_destroy_relative(p);

    t_logf("  pool parado: performance %.3f nucleo | economia %.3f nucleo | 1a tarefa depois de parado em economia: %.2f ms\n",
           cpu_perf, cpu_eco, ms);
    T_ASSERT(r, ms < 50.0, "economia: 1a tarefa depois de parado levou %.2f ms", ms);
    // margem para o ruido de 1 s de medida: economia nao pode gastar mais que performance parado
    T_ASSERT(r, cpu_eco <= cpu_perf + 0.02, "economia parado gastou mais CPU que performance (%.3f x %.3f)", cpu_eco, cpu_perf);
}

// Rajadas intermitentes (8 tarefas de ~300 us a cada 20 ms, como um evento dividido em pistas):
// o trabalho de verdade e ~0.12 nucleo. O monitor julgava "travado" quem acabara de PEGAR
// tarefa depois de parado (done_count sem mudar); dois assim acordavam os elasticos, que giravam
// ate POOL_ELASTIC_RETIRE_SPINS, pegavam a rajada seguinte e nao dormiam mais -- 3 a 6 nucleos
// ocupados. Nos dois perfis o pool tem de ficar perto do trabalho real.
static void trabalho_300us(void* a)
{
    (void)a;
    LARGE_INTEGER f, t0, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    do QueryPerformanceCounter(&t); while ((t.QuadPart - t0.QuadPart) * 1000000 / f.QuadPart < 300);
    atomic_add_inline(&g_feitas, 1);
}

static double rajadas(ThreadPool* p, int ms)
{
    ULONG64 c0 = 0, c1 = 0;
    int voltas = ms / 20, aquece = 25;
    for (int v = 0; v < aquece + voltas; v++)
    {
        if (v == aquece) QueryProcessCycleTime(GetCurrentProcess(), &c0);
        for (int i = 0; i < 8; i++) pool_submit_relative(p, trabalho_300us, 0);
        Sleep(20);
    }
    QueryProcessCycleTime(GetCurrentProcess(), &c1);
    return (double)(c1 - c0) / bench_tsc_hz() / (voltas * 20 / 1000.0);
}

void teste_pool_rajadas_intermitentes_nao_giram(TestResult* r)
{
    t_start(r);
    bench_tsc_hz();
    double cpu[2];
    int perfis[2] = { POOL_PERFIL_ECONOMIA, POOL_PERFIL_PERFORMANCE };
    for (int k = 0; k < 2; k++)
    {
        ThreadPool* p = pool_create_relative(0);
        T_ASSERT(r, p != 0, "pool_create_relative falhou");
        pool_perfil_relative(p, perfis[k]);
        atomic_set_inline(&g_feitas, 0);
        cpu[k] = rajadas(p, 2000);
        pool_wait_idle_relative(p);
        pool_destroy_relative(p);
    }
    t_logf("  rajadas de 8 x 300 us a cada 20 ms (trabalho ~0.12 nucleo): economia %.2f nucleo | performance %.2f nucleo\n", cpu[0], cpu[1]);
    T_ASSERT(r, cpu[0] < 0.5, "economia: %.2f nucleo para ~0.12 de trabalho (elasticos girando?)", cpu[0]);
    T_ASSERT(r, cpu[1] < 1.0, "performance: %.2f nucleo para ~0.12 de trabalho (elasticos girando?)", cpu[1]);
}

// Pool PARADO em economia: quantas vezes por segundo as threads do processo acordam (trocas
// de contexto, somadas). Antes, os workers nao-core dormiam 1 ms fixo em qualquer perfil:
// ~650 acordadas/s cada, ~3.800/s num pool de 16 -- o que impede o processador de descansar,
// mesmo com pouca CPU. Em economia o pool parado tem de acordar pouco; em performance segue
// como antes (prontidao). Mede a diferenca para o processo SEM este pool (outras threads do
// teste entram nas duas medidas igual).
#include <winternl.h>
#pragma comment(lib, "ntdll.lib")

static double acordadas_por_s(int ms)
{
    static BYTE buf[8 << 20];
    double soma[2] = { 0, 0 };
    for (int k = 0; k < 2; k++)
    {
        if (k == 1) Sleep((DWORD)ms);
        ULONG n = 0;
        if (NtQuerySystemInformation(SystemProcessInformation, buf, sizeof(buf), &n) != 0) return -1;
        for (BYTE* p = buf;;)
        {
            SYSTEM_PROCESS_INFORMATION* sp = (SYSTEM_PROCESS_INFORMATION*)p;
            if ((DWORD)(ULONG_PTR)sp->UniqueProcessId == GetCurrentProcessId())
            {
                SYSTEM_THREAD_INFORMATION* t = (SYSTEM_THREAD_INFORMATION*)(sp + 1);
                for (ULONG i = 0; i < sp->NumberOfThreads; i++) soma[k] += (double)t[i].Reserved3;   // ContextSwitches
                break;
            }
            if (!sp->NextEntryOffset) return -1;
            p += sp->NextEntryOffset;
        }
    }
    return (soma[1] - soma[0]) / (ms / 1000.0);
}

void teste_pool_parado_economia_acorda_pouco(TestResult* r)
{
    t_start(r);
    double base = acordadas_por_s(1000);
    ThreadPool* p = pool_create_relative(0);
    T_ASSERT(r, p != 0, "pool_create_relative falhou");
    int workers = 0, core = 0; pool_dims_relative(p, &workers, &core);

    Sleep(500);
    double perf = acordadas_por_s(1000) - base;

    pool_perfil_relative(p, POOL_PERFIL_ECONOMIA);
    Sleep(1500);   // recuo: os sonos dobram ate o teto
    double eco = acordadas_por_s(2000) - base;

    // parado nao quer dizer surdo: rajada depois da pausa, tudo executa
    atomic_set_inline(&g_feitas, 0);
    for (int i = 0; i < 20000; i++) pool_submit_relative(p, conta, 0);
    int ok = espera_feitas(20000, 5000);
    pool_destroy_relative(p);

    t_logf("  pool parado (%d workers, %d core), acordadas/s alem do processo: performance %.0f | economia %.0f\n",
           workers, core, perf, eco);
    T_ASSERT(r, ok, "economia: rajada depois de parado executou so %d de 20000", atomic_get_inline(&g_feitas));
    T_ASSERT(r, eco < 500, "economia parado: %.0f acordadas/s (performance: %.0f)", eco, perf);
    T_ASSERT(r, eco <= 0.25 * perf + 50, "economia parado acorda quase tanto quanto performance (%.0f x %.0f)", eco, perf);
}
