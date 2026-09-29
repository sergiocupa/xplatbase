//  Versao AJUSTE do bench: o MESMO src/thread_pool.c do DEPOIS, com 1 vigia ligado
//  (pool_vigias_relative). Mede o custo e o ganho de ligar o vigia no codigo que vai para
//  producao -- o vigia vem desligado por padrao.

#include "pool_versoes.h"

static ThreadPool* cria_com_vigia(int cores)
{
    ThreadPool* p = pool_create_relative(cores);
    pool_vigias_relative(p, 1);
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
    return "src/thread_pool.c (o mesmo do depois) com pool_vigias_relative(pool, 1)";
}
