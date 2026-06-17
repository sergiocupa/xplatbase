/*
 * thread_pool_v3.h
 *
 *   Arquitetura experimental com DEQUE Chase-Lev por worker (estilo TBB).
 *
 *   Como o submit aqui e EXTERNO (thread produtora != worker), e o deque
 *   Chase-Lev so admite UM dono empurrando no bottom, usa-se uma ponte:
 *
 *     submit  -> inbox MPMC do worker (round-robin) + wake do dono
 *     worker  -> 1) take() do PROPRIO deque   (LIFO, sem CAS no caso comum)
 *                2) drena o inbox -> empurra no proprio deque (push, so o dono)
 *                3) steal() de outros deques   (FIFO, CAS) e, se vazio, pop do
 *                   inbox alheio (para nao reter trabalho de um dono ocupado)
 *                4) spin progressivo -> park (WaitOnAddress)
 *
 *   Defaults (override -D):
 *     V3_WORKER_NUM/DEN  workers = cores * NUM/DEN     (default 1/1 = cores)
 *     V3_DEQUE_CAP       slots do deque (pot. de 2)    (default 4096)
 *     V3_INBOX_CAP       slots do inbox MPMC (pot. 2)  (default 1024)
 *     V3_DRAIN_BATCH     itens movidos inbox->deque    (default 64)
 *     V3_SPIN_PAUSE/_YIELD/_SLEEP0                      iteracoes do spin
 */

#ifndef THREAD_POOL_V3_H
#define THREAD_POOL_V3_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WSPoolV3 WSPoolV3;
typedef void (*v3_task_fn)(void*);

WSPoolV3* v3_pool_create (int cores_override);
void      v3_pool_destroy(WSPoolV3* p);
bool      v3_pool_submit (WSPoolV3* p, v3_task_fn fn, void* arg);
void      v3_pool_wait_idle(WSPoolV3* p);
void      v3_pool_dims(WSPoolV3* p, int* out_workers, int* out_lanes);

#ifdef __cplusplus
}
#endif

#endif /* THREAD_POOL_V3_H */
