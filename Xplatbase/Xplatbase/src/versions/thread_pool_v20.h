/*
 * thread_pool_v20.h
 *
 *   V20 = V4 (Chase-Lev + inbox) com CONCORRENCIA DINAMICA via SPINNER CAP.
 *
 *   Todos os workers existem (= cores), mas no maximo V20_SPIN_CAP podem ficar
 *   SPINANDO ociosos ao mesmo tempo; os demais parqueiam (acordam por demanda
 *   no submit). O cap so limita o spin OCIOSO, nunca a execucao de task — logo:
 *     - spawn: workers ocupados rodando tasks -> cap nao morde -> usa todos os cores.
 *     - flat : workers ociosos entre tasks    -> cap limita os quentes -> cauda baixa + menos CPU.
 *
 *   Default: V20_SPIN_CAP = cores * 3/4. Override -D V20_SPIN_CAP_NUM/DEN.
 */

#ifndef THREAD_POOL_V20_H
#define THREAD_POOL_V20_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WSPoolV20 WSPoolV20;
typedef void (*v20_task_fn)(void*);

WSPoolV20* v20_pool_create (int cores_override);
void       v20_pool_destroy(WSPoolV20* p);
bool       v20_pool_submit (WSPoolV20* p, v20_task_fn fn, void* arg);
void       v20_pool_wait_idle(WSPoolV20* p);
void       v20_pool_dims(WSPoolV20* p, int* out_workers, int* out_lanes);

#ifdef __cplusplus
}
#endif

#endif /* THREAD_POOL_V20_H */
