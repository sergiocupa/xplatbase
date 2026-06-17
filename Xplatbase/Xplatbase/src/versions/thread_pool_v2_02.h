/*
 * thread_pool_v2_02.h  — pool OFICIAL (V2.02).
 *
 *   Arquitetura ARENA + core/reserva (consolidacao do antigo "V4"):
 *
 *     - Submit EXTERNO vai para G filas MPMC COMPARTILHADAS (shards = cores/4),
 *       round-robin. Qualquer worker puxa de qualquer shard (task nao fica presa
 *       a um dono). Em regime quente os workers se auto-servem e o produtor so
 *       ENFILEIRA (acorda alguem apenas se houver core parqueado) -> submit barato.
 *     - Spawn (submit reentrante, de dentro de uma task) usa o deque Chase-Lev
 *       LOCAL do worker (push/take sem CAS). Steal entre deques.
 *     - core/reserva: n_core = cores*7/10 workers "core" spinam ociosos e sao
 *       acordados pelo submit; os demais (reserva) sao park-first e so engajam
 *       quando ha backlog (spawn/rajada) -> flat ~75% CPU + cauda baixa; spawn
 *       usa todos os cores.
 *
 *   Resultados (16 cores): flat ~2.5 Mtask/s @ ~75% CPU (supera TBB), cauda no
 *   nivel do TBB; spawn no nivel do TBB.
 *
 *   Tunables (override -D): V2_CORE_NUM/V2_CORE_DEN (7/10), V2_SHARD_DIV (4),
 *   V2_SHARD_CAP, V2_DEQUE_CAP, V2_SPIN_PAUSE/_YIELD/_SLEEP0.
 */

#ifndef THREAD_POOL_V2_02_H
#define THREAD_POOL_V2_02_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WSPoolV2 WSPoolV2;
typedef void (*v2_task_fn)(void*);

/* cores_override: 0 -> usa xcpu_count(). Retorna NULL em falha. */
WSPoolV2* v2_pool_create (int cores_override);

/* Espera as tasks em voo drenarem, para e junta os workers, libera tudo. */
void      v2_pool_destroy(WSPoolV2* p);

/* Submit. Externo: round-robin nas shards compartilhadas (backpressure, nunca
 * falha exceto shutdown). Reentrante (de dentro de uma task): push no deque local. */
bool      v2_pool_submit (WSPoolV2* p, v2_task_fn fn, void* arg);

/* Bloqueia ate todas as tasks submetidas terminarem. */
void      v2_pool_wait_idle(WSPoolV2* p);

/* out_workers = total de workers; out_core = workers core (recebem submit externo). */
void      v2_pool_dims(WSPoolV2* p, int* out_workers, int* out_core);

#ifdef __cplusplus
}
#endif

#endif /* THREAD_POOL_V2_02_H */
