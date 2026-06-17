/*
 * thread_pool_v3.h — pool estilo ARENA: fila de submit COMPARTILHADA (shards) +
 * deque Chase-Lev local por worker (spawn). Todos os workers sao "core" (auto-
 * servem das shards). Ver thread_pool_v3.c.
 *
 * V4 = este mesmo codigo recompilado com V3_CORE_RESERVE=1 (core + reserva).
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
void      v3_pool_dims(WSPoolV3* p, int* out_workers, int* out_core);

#ifdef __cplusplus
}
#endif

#endif /* THREAD_POOL_V3_H */
