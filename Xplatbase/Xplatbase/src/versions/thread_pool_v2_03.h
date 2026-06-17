/*
 * thread_pool_v2_03.h — V2.03 = V2.02 (arena + core/reserva) + WORKER ELASTICO.
 *
 *   Acrescenta protecao de latencia das tasks curtas quando workers ficam presos
 *   em tasks longas (perfil "rapida pode virar lenta"):
 *
 *     - cada worker mantem um contador de PROGRESSO (done_count) e um flag in_task,
 *       ambos single-writer (custo ~zero no hot-path);
 *     - uma thread MONITOR (a cada V203_MON_MS) detecta workers "presos" (in_task e
 *       done_count sem mudar entre ticks => task > ~MON_MS sem terminar);
 *     - se ha >= V203_STUCK_MIN presos E backlog nas shards, acorda 1 worker
 *       ELASTICO (alem dos cores) para drenar as curtas;
 *     - workers elasticos se APOSENTAM (re-parqueiam) quando nao acham trabalho.
 *
 *   Para tasks longas BLOQUEANTES recupera o core liberado; para CPU-bound,
 *   sobre-subscreve (time-slice) mantendo as curtas responsivas.
 *
 *   Tunables (-D): V203_ELASTIC_NUM/DEN (elasticos = cores*N/D, default 1/1),
 *   V203_MON_MS (5), V203_STUCK_MIN (2). Demais herdados (V203_CORE_*, shards...).
 */

#ifndef THREAD_POOL_V2_03_H
#define THREAD_POOL_V2_03_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WSPoolV203 WSPoolV203;
typedef void (*v203_task_fn)(void*);

WSPoolV203* v203_pool_create (int cores_override);
void        v203_pool_destroy(WSPoolV203* p);
bool        v203_pool_submit (WSPoolV203* p, v203_task_fn fn, void* arg);
void        v203_pool_wait_idle(WSPoolV203* p);
void        v203_pool_dims(WSPoolV203* p, int* out_workers, int* out_core);

#ifdef __cplusplus
}
#endif

#endif /* THREAD_POOL_V2_03_H */
