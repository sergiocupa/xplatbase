/* Reprodutor isolado: falha de alocacao durante a CRIACAO de workers no
 * pool_create deve resultar em retorno limpo (NULL), SEM stall longo e SEM
 * incrementar pool_force_kill_count (nenhuma thread real foi iniciada ainda).
 *
 * Build (cl, x64, com POOL_TEST_HOOKS) — ver comando no chat.
 */
#define POOL_TEST_HOOKS
#include "../Xplatbase/Xplatbase/src/thread_pool.h"
#include "../Xplatbase/Xplatbase/src/thread_pool_test_hooks.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

static double now_ms(void)
{
    static LARGE_INTEGER f = { 0 };
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

int main(void)
{
    /* Varre o ponto de falha de alocacao. Para shard_count alto, varias
     * chamadas de alloc ocorrem DENTRO da criacao de workers (calloc por
     * WSWorker), depois das lanes. Procuramos um k que falhe nessa fase. */
    PoolConfig base = pool_default_config();
    base.shard_count = 8;
    base.max_shards  = 8;
    base.reserve_size = 8;

    printf("ncpu=%d\n", xcpu_count());
    printf(" k | create_ms | pool? | forcekill_delta\n");
    printf("---+-----------+-------+----------------\n");

    for (int k = 0; k <= 40; k++) {
        int fk0 = pool_force_kill_count();
        pool_test_fail_alloc_after(k);

        PoolConfig cfg = base;
        double t0 = now_ms();
        ShardedPool* pool = pool_create(&cfg);
        double dt = now_ms() - t0;

        int fkd = pool_force_kill_count() - fk0;
        printf("%2d | %9.1f | %5s | %d%s\n",
               k, dt, pool ? "OK" : "NULL", fkd,
               (!pool && (dt > 500.0 || fkd > 0)) ? "   <== SUSPEITO" : "");

        if (pool) {
            pool_test_fail_alloc_after(-1); /* desliga injecao p/ shutdown limpo */
            pool_shutdown(pool);
        }
        pool_test_fail_alloc_after(-1);
        if (fkd > 0) {
            printf("  >>> force-kill em falha de alocacao (k=%d): provavel leak+poison\n", k);
            /* nao reciclar mais apos poison global; encerra a varredura */
            break;
        }
    }
    return 0;
}
