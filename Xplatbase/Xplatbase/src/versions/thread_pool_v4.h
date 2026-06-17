/*
 * thread_pool_v4.h
 *
 *   V4 = V3 (deque Chase-Lev + inbox) com otimizacoes no hot path do spawn:
 *     - SEM rdtscp no submit (nao timestampa a task).
 *     - steal no inbox so se nao-vazio (evita pop_mc desperdicado).
 *     - contador 'pending' POR WORKER (sem ping-pong de cache no contador global).
 *     - funcoes quentes com __forceinline.
 *
 *   Mesma API/semantica do V3. Defaults iguais (override -D V4_*).
 */

#ifndef THREAD_POOL_V4_H
#define THREAD_POOL_V4_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WSPoolV4 WSPoolV4;
typedef void (*v4_task_fn)(void*);

WSPoolV4* v4_pool_create (int cores_override);
void      v4_pool_destroy(WSPoolV4* p);
bool      v4_pool_submit (WSPoolV4* p, v4_task_fn fn, void* arg);
void      v4_pool_wait_idle(WSPoolV4* p);
void      v4_pool_dims(WSPoolV4* p, int* out_workers, int* out_lanes);

#ifdef __cplusplus
}
#endif

#endif /* THREAD_POOL_V4_H */
