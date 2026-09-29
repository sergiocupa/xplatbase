/*
 * thread_pool.h  — pool OFICIAL (design consolidado V2.05).
 *
 *   Work-stealing estilo ARENA + core/reserva + worker elastico:
 *     - Submit EXTERNO vai para G filas MPMC COMPARTILHADAS (shards = cores/4),
 *       round-robin; qualquer worker puxa de qualquer shard (task nao fica presa
 *       a um dono). Ring Vyukov tipado inline (V2.05); consumidores reservam ate
 *       POOL_BATCH tasks por CAS. Em regime quente os workers se auto-servem e o
 *       produtor so enfileira (acorda alguem so se houver core parqueado).
 *     - Spawn (submit reentrante, de dentro de uma task) vai para o LIFO slot
 *       nao-roubavel do worker (V2.05, cache quente) com overflow para o deque
 *       Chase-Lev LOCAL (push/take sem CAS). Steal entre deques.
 *     - core/reserva: n_core = cores*7/10 spinam e sao acordados pelo submit;
 *       os demais (reserva) sao park-first e so engajam sob backlog -> flat
 *       ~75% CPU + cauda baixa; spawn usa todos os cores.
 *     - worker ELASTICO: um monitor detecta workers presos em tasks longas e,
 *       havendo backlog, acorda workers extras (alem dos cores) para drenar as
 *       curtas; eles se aposentam quando a carga passa. Protege a latencia das
 *       curtas no perfil "rapida pode virar lenta".
 *
 *   API minima (run-to-completion). Para tasks bloqueantes/longas em massa,
 *   prefira isolar num pool separado / reactor.
 *
 *   Tunables (-D): POOL_CORE_NUM/DEN (7/10), POOL_ELASTIC_NUM/DEN (1/1),
 *   POOL_SHARD_DIV (4), POOL_SHARD_CAP, POOL_DEQUE_CAP, POOL_MON_MS (5),
 *   POOL_STUCK_MIN (2), POOL_SPIN_PAUSE/_YIELD/_SLEEP0,
 *   POOL_BATCH (2, V2.05), POOL_LIFO_CAP (8, V2.05).
 */

#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <stdbool.h>
#include "../include/xplatbase.h"   /* XPLATBASE_API */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ThreadPool ThreadPool;
typedef void (*pool_task_fn)(void*);


// public
XPLATBASE_API ThreadPool* pool_create_relative(int cores_override);
XPLATBASE_API void        pool_destroy_relative(ThreadPool* p);
XPLATBASE_API boolean     pool_submit_relative(ThreadPool* p, pool_task_fn fn, void* arg);
XPLATBASE_API void        pool_wait_idle_relative(ThreadPool* p);
XPLATBASE_API void        pool_dims_relative(ThreadPool* p, int* out_workers, int* out_core);


// internal
void pool_create();
void pool_destroy();

// public
XPLATBASE_API boolean pool_submit(pool_task_fn fn, void* arg);
XPLATBASE_API void pool_wait_idle();
XPLATBASE_API void pool_dims(int* w, int* c);


/* ---- VIGIAS: prontidao x energia ------------------------------------------------------
 *
 * PADRAO: DESLIGADO (0 vigias) -- prioriza ECONOMIA DE ENERGIA. Com o pool ocioso todos os
 * workers dormem de verdade: ~0 CPU parado. Custo: a 1a tarefa depois de uma pausa (mais de
 * ~0,1 ms sem trabalho) espera o sistema acordar um worker, ~15-20 us no Windows, e um 1o
 * trabalho pesado encontra os nucleos com o clock baixo. Sob carga continua, nao muda nada.
 *
 * pool_vigias(n): mantem os primeiros n workers core ACORDADOS, girando, com o pool ocioso.
 * A 1a tarefa depois de uma pausa comeca em ~2-3 us (como antes da versao economica), em
 * troca de ~1 NUCLEO OCUPADO POR VIGIA, o tempo todo, enquanto o pool existir (~6% da CPU
 * numa maquina de 16 processadores logicos). Em dispositivo com bateria, deixe 0.
 *
 *   n = 0            desliga (padrao)
 *   n = 1            recomendado quando a prontidao importa mais que a energia
 *   n > 1            mais nucleos quentes; limitado ao numero de workers core
 *
 * Vale NA HORA e pode ser chamado a qualquer momento depois da inicializacao (o pool global
 * e criado pelo platform_init, que roda sozinho na carga da lib) e quantas vezes quiser.
 * Medido no Tester/unittests (bench ANTES x DEPOIS x AJUSTE): com 1 vigia, a carga fica no
 * mesmo patamar; o despertar volta de ~17 us para ~3 us.
 */
XPLATBASE_API void pool_vigias(int vigias);                                 /* pool global */
XPLATBASE_API void pool_vigias_relative(ThreadPool* p, int vigias);        /* pool proprio */



#ifdef __cplusplus
}
#endif

#endif /* THREAD_POOL_H */
