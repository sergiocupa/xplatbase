/*
 * thread_pool_v4.h — V4 = V3 (arena) + "core + reserva".
 *   Mesmo codigo do V3 recompilado com V3_CORE_RESERVE=1 e simbolos v3_*->v4_*.
 *   Reusa o tipo opaco WSPoolV3 e v3_task_fn.
 */
#ifndef THREAD_POOL_V4_H
#define THREAD_POOL_V4_H

#include "thread_pool_v3.h"

#ifdef __cplusplus
extern "C" {
#endif

WSPoolV3* v4_pool_create (int cores_override);
void      v4_pool_destroy(WSPoolV3* p);
bool      v4_pool_submit (WSPoolV3* p, v3_task_fn fn, void* arg);
void      v4_pool_wait_idle(WSPoolV3* p);
void      v4_pool_dims(WSPoolV3* p, int* out_workers, int* out_core);

#ifdef __cplusplus
}
#endif

#endif /* THREAD_POOL_V4_H */
