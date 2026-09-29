//  Versao DEPOIS: o src/thread_pool.c atual, compilado normalmente neste projeto.

#include "pool_versoes.h"

const PoolVersao POOL_DEPOIS = {
    "depois",
    pool_create_relative,
    pool_destroy_relative,
    pool_submit_relative,
    pool_dims_relative,
};
