//  Versao DEPOIS: o src/thread_pool.c atual, compilado normalmente neste projeto.

#include "pool_versoes.h"
#include <stdlib.h>

int pool_bench_perfil_economia(void)
{
#ifdef _WIN32
    char* v = 0; size_t n = 0; int r = 0;
    if (_dupenv_s(&v, &n, "XPB_BENCH_PERFIL") == 0 && v) { r = (v[0] == 'e' || v[0] == 'E'); free(v); }
    return r;
#else
    const char* v = getenv("XPB_BENCH_PERFIL");
    return v && (v[0] == 'e' || v[0] == 'E');
#endif
}

static ThreadPool* cria(int cores)
{
    ThreadPool* p = pool_create_relative(cores);
    if (p && pool_bench_perfil_economia()) pool_perfil_relative(p, POOL_PERFIL_ECONOMIA);
    return p;
}

const PoolVersao POOL_DEPOIS = {
    "depois",
    cria,
    pool_destroy_relative,
    pool_submit_relative,
    pool_dims_relative,
};
