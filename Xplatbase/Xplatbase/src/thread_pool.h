/*
 * thread_pool.h  — pool OFICIAL (consolidacao do design V2.03).
 *
 *   Work-stealing estilo ARENA + core/reserva + worker elastico:
 *     - Submit EXTERNO vai para G filas MPMC COMPARTILHADAS (shards = cores/4),
 *       round-robin; qualquer worker puxa de qualquer shard (task nao fica presa
 *       a um dono). Em regime quente os workers se auto-servem e o produtor so
 *       enfileira (acorda alguem so se houver core parqueado) -> submit barato.
 *     - Spawn (submit reentrante, de dentro de uma task) usa o deque Chase-Lev
 *       LOCAL do worker (push/take sem CAS). Steal entre deques.
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
 *   POOL_STUCK_MIN (2), POOL_SPIN_PAUSE/_YIELD/_SLEEP0.
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

/* cores_override: 0 -> usa xcpu_count(). Retorna NULL em falha. */
XPLATBASE_API ThreadPool* pool_create(int cores_override);

/* Espera as tasks em voo drenarem, para e junta os workers, libera tudo. */
XPLATBASE_API void        pool_destroy(ThreadPool* p);

/* Submit. Externo: round-robin nas shards (backpressure, nunca falha exceto
 * shutdown). Reentrante (de dentro de uma task): push no deque local. */
XPLATBASE_API bool        pool_submit(ThreadPool* p, pool_task_fn fn, void* arg);

/* Bloqueia ate todas as tasks submetidas terminarem. */
XPLATBASE_API void        pool_wait_idle(ThreadPool* p);

/* out_workers = total de workers (core+reserva); out_core = workers core. */
XPLATBASE_API void        pool_dims(ThreadPool* p, int* out_workers, int* out_core);

#ifdef __cplusplus
}
#endif

#endif /* THREAD_POOL_H */
