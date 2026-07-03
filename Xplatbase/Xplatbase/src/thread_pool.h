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



#ifdef __cplusplus
}
#endif

#endif /* THREAD_POOL_H */
