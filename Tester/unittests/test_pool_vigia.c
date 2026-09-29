//  Vigia do pool de tarefas (pool_vigias_relative): liga e desliga NA HORA.
//
//  Mede a CPU do processo com o pool parado em tres momentos: padrao (vigia desligado),
//  depois de ligar 1 vigia e depois de desligar de novo. Esperado: ~0 nucleo, ~1 nucleo,
//  ~0 nucleo. CPU em ciclos (QueryProcessCycleTime), nao o GetProcessTimes: esse cobra por
//  tique de relogio e mede 0 para um pool que acorda alinhado com o tique.

#include "ctest_core.h"
#include "testes.h"
#include "bench_pool.h"          // bench_tsc_hz
#include "thread_pool.h"

#include <windows.h>

static double nucleos_em(int ms)
{
    ULONG64 c0 = 0, c1 = 0;
    QueryProcessCycleTime(GetCurrentProcess(), &c0);
    Sleep((DWORD)ms);
    QueryProcessCycleTime(GetCurrentProcess(), &c1);
    return (double)(c1 - c0) / bench_tsc_hz() / (ms / 1000.0);
}

void teste_pool_vigia_liga_e_desliga(TestResult* r)
{
    ThreadPool* p;
    double padrao, com_vigia, desligado;

    t_start(r);
    bench_tsc_hz();                              // calibra antes de medir
    p = pool_create_relative(0);
    T_ASSERT(r, p != 0, "pool_create_relative falhou");

    Sleep(300);                                  // o sono ocioso chega ao teto em ~60 ms
    padrao = nucleos_em(1000);

    pool_vigias_relative(p, 1);
    Sleep(100);
    com_vigia = nucleos_em(1000);

    pool_vigias_relative(p, 0);
    Sleep(300);
    desligado = nucleos_em(1000);

    pool_destroy_relative(p);

    t_logf("  pool parado: padrao %.3f nucleo | com 1 vigia %.3f | desligado de novo %.3f\n",
           padrao, com_vigia, desligado);
    T_ASSERT(r, padrao < 0.3,    "padrao deveria ser ~0 nucleo (vigia desligado), mediu %.3f", padrao);
    T_ASSERT(r, com_vigia > 0.7 && com_vigia < 1.6, "com 1 vigia deveria ser ~1 nucleo, mediu %.3f", com_vigia);
    T_ASSERT(r, desligado < 0.3, "depois de desligar deveria voltar a ~0 nucleo, mediu %.3f", desligado);
}
