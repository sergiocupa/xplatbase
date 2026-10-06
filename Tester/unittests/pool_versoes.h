//  As versoes do pool de tarefas, lado a lado no mesmo executavel.
//
//  AJUSTE = o mesmo src/thread_pool.c do DEPOIS com 1 vigia ligado (pool_vigias_relative):
//           a opcao de prontidao, que vem desligada por padrao (pool_ajuste.c).
//
//  DEPOIS = src/thread_pool.c como esta no disco (o que vai ser commitado).
//  ANTES  = referencia/thread_pool_antes.c (o thread_pool.c de antes da ultima alteracao
//           commitada) ou, com PoolAntesRef, o arquivo daquela revisao do git; compilado
//           com os simbolos publicos renomeados
//           (pool_antes.c). As duas usam as MESMAS camadas de baixo (thread_handler,
//           thread_wait, memory_pool) e as MESMAS opcoes de compilacao: a diferenca medida
//           e so a do thread_pool.c.

#ifndef POOL_VERSOES_H
#define POOL_VERSOES_H

#include "thread_pool.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    const char*  Nome;
    ThreadPool*  (*Criar)(int cores);
    void         (*Destruir)(ThreadPool* p);
    boolean      (*Submeter)(ThreadPool* p, pool_task_fn fn, void* arg);
    void         (*Dims)(ThreadPool* p, int* workers, int* core);
}
PoolVersao;

extern const PoolVersao POOL_DEPOIS;
extern const PoolVersao POOL_ANTES;
extern const PoolVersao POOL_AJUSTE;     // src/thread_pool.c com 1 vigia (pool_vigias_relative)

// 0 quando a versao nao pode ser montada (sem git, revisao inexistente, arquivo ausente...).
int         pool_antes_disponivel(void);
int         pool_ajuste_disponivel(void);

// XPB_BENCH_PERFIL=economia: ANTES e DEPOIS criados com pool_perfil_relative(ECONOMIA), para
// comparar o perfil economia antes x depois de uma alteracao (o AJUSTE segue a propria regra).
// pool_antes_tem_perfil: 0 quando a revisao ANTES e anterior aos perfis (roda no padrao).
int         pool_bench_perfil_economia(void);
int         pool_antes_tem_perfil(void);

// De onde veio cada versao, para o relatorio (revisao, sha, se ha alteracao local).
const char* pool_antes_descricao(void);
const char* pool_depois_descricao(void);
const char* pool_ajuste_descricao(void);

#ifdef __cplusplus
}
#endif

#endif
