//  Perfil do pool de tarefas (pool_perfil_relative): performance (padrao) x economia.
//
//  Verifica que:
//    - o padrao e PERFORMANCE e o perfil troca na hora, nos dois sentidos;
//    - em ECONOMIA nenhuma tarefa se perde nem atrasa por causa do monitor dormindo: depois
//      de um tempo parado, a primeira tarefa roda logo (o submit acorda quem precisa);
//    - em ECONOMIA, com o pool parado, a CPU do processo nao e maior que em PERFORMANCE
//      (o monitor deixa de acordar a cada POOL_MON_MS).
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
