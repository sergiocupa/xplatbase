/*
 * thread_pool_v2.h  — pool OFICIAL (arquitetura nova).
 *
 *   Work-stealing com deque Chase-Lev por worker + inbox MPMC, e concorrencia
 *   dinamica por "core + reserva":
 *
 *     - n_core = cores * 7/10 workers "core": recebem o submit EXTERNO (round-robin
 *       so neles) e fazem spin progressivo quando ociosos.
 *     - os demais (reserva) NAO recebem submit externo e, quando ociosos, NAO fazem
 *       busy-spin: dao uma varredura de steal e parqueiam. So engajam quando ha
 *       backlog para roubar (spawn / rajada).
 *
 *   Resultado:
 *     - flat (produtor externo): ~n_core workers quentes -> ~75% CPU, cauda baixa,
 *       throughput cheio; a reserva fica parqueada (sem oversubscription).
 *     - spawn (task dentro de task): trabalho LOCAL em qualquer worker (push no
 *       deque do proprio worker) -> a reserva ve backlog e engaja -> usa os cores.
 *
 *   Submit reentrante (task dentro de task) e detectado e empurra no deque LOCAL.
 *
 *   Tunables (override -D): V2_CORE_NUM/V2_CORE_DEN (default 7/10), V2_DEQUE_CAP,
 *   V2_INBOX_CAP, V2_SPIN_PAUSE/_YIELD/_SLEEP0.
 */

#ifndef THREAD_POOL_V2_H
#define THREAD_POOL_V2_H

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

/* Submit. Externo: round-robin nos workers core (backpressure, nunca falha exceto
 * shutdown). Reentrante (de dentro de uma task): push no deque local. */
bool      v2_pool_submit (WSPoolV2* p, v2_task_fn fn, void* arg);

/* Bloqueia ate todas as tasks submetidas terminarem. */
void      v2_pool_wait_idle(WSPoolV2* p);

/* out_workers = total de workers; out_core = workers que recebem submit externo. */
void      v2_pool_dims(WSPoolV2* p, int* out_workers, int* out_core);

#ifdef __cplusplus
}
#endif

#endif /* THREAD_POOL_V2_H */
