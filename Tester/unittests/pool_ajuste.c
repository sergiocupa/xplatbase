//  Versao AJUSTE do bench: o MESMO src/thread_pool.c do DEPOIS, com um ajuste ligado:
//    - padrao: 1 vigia (pool_vigias_relative);
//    - com a variavel XPB_BENCH_AJUSTE_GIRO_US=<us>: giro maximo limitado (pool_giro_max_us),
//      no lugar do vigia;
//    - com XPB_BENCH_AJUSTE_PERFIL=economia: perfil economia (pool_perfil_relative).
//  Mede o custo e o ganho do ajuste no codigo que vai para producao (os dois vem desligados).

#include "pool_versoes.h"
#include <stdlib.h>
#include <stdio.h>

static int giro_us(void)
{
#ifdef _WIN32
    char* v = 0; size_t n = 0; int r = -1;
    if (_dupenv_s(&v, &n, "XPB_BENCH_AJUSTE_GIRO_US") == 0 && v) { r = atoi(v); free(v); }
    return r;
#else
    const char* v = getenv("XPB_BENCH_AJUSTE_GIRO_US");
    return v ? atoi(v) : -1;
#endif
}

static int perfil_economia(void)
{
#ifdef _WIN32
    char* v = 0; size_t n = 0; int r = 0;
    if (_dupenv_s(&v, &n, "XPB_BENCH_AJUSTE_PERFIL") == 0 && v) { r = (v[0] == 'e' || v[0] == 'E'); free(v); }
    return r;
#else
    const char* v = getenv("XPB_BENCH_AJUSTE_PERFIL");
    return v && (v[0] == 'e' || v[0] == 'E');
#endif
}

static ThreadPool* cria_com_vigia(int cores)
{
    ThreadPool* p = pool_create_relative(cores);
    int g = giro_us();
    if (perfil_economia()) pool_perfil_relative(p, POOL_PERFIL_ECONOMIA);
    else if (g >= 0) pool_giro_max_us_relative(p, g);
    else pool_vigias_relative(p, 1);
    return p;
}

const PoolVersao POOL_AJUSTE = {
    "ajuste",
    cria_com_vigia,
    pool_destroy_relative,
    pool_submit_relative,
    pool_dims_relative,
};

int pool_ajuste_disponivel(void) { return 1; }

const char* pool_ajuste_descricao(void)
{
    static char d[160];
    int g = giro_us();
    if (perfil_economia()) snprintf(d, sizeof(d), "src/thread_pool.c (o mesmo do depois) com pool_perfil_relative(pool, POOL_PERFIL_ECONOMIA)");
    else if (g >= 0) snprintf(d, sizeof(d), "src/thread_pool.c (o mesmo do depois) com pool_giro_max_us_relative(pool, %d)", g);
    else snprintf(d, sizeof(d), "src/thread_pool.c (o mesmo do depois) com pool_vigias_relative(pool, 1)");
    return d;
}
