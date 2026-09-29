//  Cenarios de carga do pool de tarefas, medidos numa versao (pool_versoes.h) de cada vez.
//
//  Os cenarios "flat" e o "spawn" sao os do Tester/thread_pool/thread_pool_bench.cpp (mesmos
//  nomes, tamanhos, trabalho por tarefa e alocacoes), com latencia POR TAREFA: do submit ao
//  inicio da execucao. Os de pool parado medem o que o bench antigo nao via: CPU gasta
//  sem trabalho e o custo de acordar.

#ifndef BENCH_POOL_H
#define BENCH_POOL_H

#include "pool_versoes.h"

#ifdef __cplusplus
extern "C" {
#endif

// Metricas de uma medida. Latencias em microssegundos, wall em milissegundos.
enum { MET_WALL, MET_P50, MET_P90, MET_P99, MET_P999, MET_MAX10, MET_MAX, MET_NUCLEOS, N_METRICAS };

typedef enum
{
    CEN_FLAT,               // N tarefas submetidas de fora
    CEN_SPAWN,              // arvore: cada tarefa submete as filhas (caminho reentrante)
    CEN_CPU_OCIOSO,         // nucleos gastos com o pool parado
    CEN_DESPERTAR,          // submit externo -> inicio, depois de 200 ms parado
    CEN_FJ_OCIOSO_CPU,      // fork-join de 64 apos 1 s parado, filhas com trabalho de CPU
    CEN_FJ_OCIOSO_TEMPO,    // idem, filhas com 500 us de relogio (so agendamento)
    CEN_FJ_REGIME           // fork-join de 64, 20 seguidos, sem pausa
}
TipoCenario;

typedef struct
{
    const char* Nome;
    TipoCenario Tipo;
    int         Tarefas;
    int         WMin, WMax;          // trabalho por tarefa, us (espera ativa)
    int         LongaCada, LongaUs;  // a cada N tarefas, uma longa de LongaUs
    int         Alocs;               // alocacoes por tarefa (64..2048 bytes)
    int         AlocPool;            // 1 = memory_pool, 0 = malloc do CRT
}
CenarioPool;

typedef struct { double V[N_METRICAS]; } Medida;

// Aloca os buffers de latencia e monta a arvore do spawn (uma vez).
void   bench_pool_prepara(int max_tarefas, int nos_arvore);

// Cria o pool da versao, mede o cenario e destroi o pool. 0 = TRAVOU (tarefa que nao
// terminou no limite); nesse caso o pool e abandonado, nao destruido.
int    bench_pool_mede(const CenarioPool* c, const PoolVersao* v, Medida* m);

// Quais metricas o tipo de cenario produz.
void   bench_pool_metricas(TipoCenario t, int aplica[N_METRICAS]);

// Frequencia do TSC (para converter ciclos de CPU em nucleos).
double bench_tsc_hz(void);

#ifdef __cplusplus
}
#endif

#endif
