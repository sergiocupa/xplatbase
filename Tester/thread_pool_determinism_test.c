/*
 * thread_pool_determinism_test.c
 *
 * Testes DETERMINISTICOS que pegam bugs reais executando a propria lib.
 * Todo o conteudo fica sob POOL_TEST_HOOKS (nada vaza para producao).
 *
 *   BUG A — Falha de alocacao DURANTE a criacao de workers:
 *           pool_create deve retornar NULL de forma limpa (sem stall longo e
 *           sem incrementar o contador global de force-kill). Hoje ele trata os
 *           WSWorker* criados-mas-nao-iniciados como threads "presas": espera o
 *           join timeout (~2s), faz force-kill fantasma e vaza tudo (poison).
 *           Usa apenas o hook EXISTENTE pool_test_fail_alloc_after — lib intacta.
 *
 *   BUG B — Lane orfa por TOCTOU em activate_lane (reuso de owner):
 *           se uma reativacao ocorre entre o re-check de active_lanes e o CAS
 *           worker->NULL do owner que esta saindo, a lane fica ATIVA sem owner.
 *           Forcado de forma deterministica pela barreira orphan-window (guardada
 *           por POOL_TEST_HOOKS), posicionada exatamente nessa janela.
 *
 * Build (cl, x64): comando no chat. Tem seu proprio main, nao depende do
 * Tester.vcxproj nem de TBB.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdint.h>

#include "../Xplatbase/Xplatbase/src/thread_pool.h"

#ifdef POOL_TEST_HOOKS
#include "../Xplatbase/Xplatbase/src/thread_pool_test_hooks.h"

static int g_pass = 0, g_run = 0;
#define CHECK(cond, msg) do {                                  \
    g_run++;                                                   \
    if (cond) { g_pass++; printf("    [OK]   %s\n", (msg)); }  \
    else      {            printf("    [FALHOU] %s\n", (msg)); }\
} while (0)

static double now_ms(void)
{
    static LARGE_INTEGER f = { 0 };
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

/* ───────────────────────────── BUG A ───────────────────────────── */

static void test_bug_a_alloc_fail_during_worker_creation(void)
{
    printf("\n== BUG A: falha de alloc durante criacao de workers ==\n");

    int    worst_fk   = 0;
    double worst_ms   = 0.0;
    int    null_cases = 0;

    /* Varre o ponto de injecao de falha. Para shard_count alto, varias allocs
     * ocorrem na fase de criacao de workers (1 calloc por WSWorker), depois das
     * lanes. INVARIANTE: toda falha de alloc deve devolver NULL de forma LIMPA —
     * sem stall de ~2s (join timeout) e sem force-kill fantasma. */
    for (int k = 0; k <= 40; k++) {
        int fk0 = pool_force_kill_count();
        pool_test_fail_alloc_after(k);

        PoolConfig cfg   = pool_default_config();
        cfg.shard_count  = 8;
        cfg.max_shards   = 8;
        cfg.reserve_size = 8;

        double t0 = now_ms();
        ShardedPool* pool = pool_create(&cfg);
        double dt = now_ms() - t0;
        int fkd = pool_force_kill_count() - fk0;

        if (pool) {
            pool_test_fail_alloc_after(-1);
            pool_shutdown(pool);
        }
        pool_test_fail_alloc_after(-1);

        if (!pool) {
            null_cases++;
            if (dt  > worst_ms) worst_ms = dt;
            if (fkd > worst_fk) worst_fk = fkd;
            printf("    k=%2d -> NULL  create=%.0fms  force_kill_delta=%d%s\n",
                   k, dt, fkd, (dt > 500.0 || fkd > 0) ? "   <== RUIM" : "");
        }
        if (fkd > 0) break;   /* poison global: nao reciclar mais apos force-kill */
    }

    printf("    casos de falha exercitados=%d  pior_stall=%.0fms  pior_force_kill=%d\n",
           null_cases, worst_ms, worst_fk);

    CHECK(null_cases > 0,  "falhas de alloc foram exercitadas");
    CHECK(worst_fk == 0,   "nenhuma falha de alloc dispara force-kill fantasma");
    CHECK(worst_ms < 500.0,"nenhuma falha de alloc trava ~2s (join timeout)");
}

/* ───────────────────────────── BUG C ───────────────────────────── */

static void short_task(void* a) { (void)a; }

static void test_bug_c_stolen_metric_exposed(void)
{
    printf("\n== BUG C: metrica de work-stealing exposta (total_stolen) ==\n");

    PoolConfig cfg   = pool_default_config();
    cfg.shard_count  = 4;
    cfg.spin_budget_us = 0;
    cfg.spin_iterations = 1;       /* ocia rapido → favorece roubo entre lanes */

    ShardedPool* pool = pool_create(&cfg);
    if (!pool) { CHECK(0, "pool_create != NULL"); return; }

    /* rajada concentrada para gerar backlog numa lane e roubo pelas ociosas */
    for (int i = 0; i < 2000; i++) pool_submit(pool, short_task, NULL);

    for (int spin = 0; spin < 2000; spin++) {
        PoolStats s; pool_stats(pool, &s);
        if (s.pending_tasks == 0) break;
        Sleep(1);
    }

    PoolStats st; pool_stats(pool, &st);
    pool_shutdown(pool);

    printf("    total_stolen=%llu  total_handoffs=%llu  (campos distintos)\n",
           (unsigned long long)st.total_stolen,
           (unsigned long long)st.total_handoffs);

    /* O contrato agora separa os conceitos: roubo de task (work-stealing) e
     * handoff de task longa sao contadores independentes e observaveis. */
    CHECK(st.total_handoffs == 0,
          "sem task longa: total_handoffs == 0 (nao e mais um catch-all)");
    CHECK(st.total_stolen > 0,
          "total_stolen exposto e > 0 quando houve work-stealing");
}

/* ───────────────────────────── BUG B ───────────────────────────── */

static void test_bug_b_orphan_lane_toctou(void)
{
    printf("\n== BUG B: lane orfa (TOCTOU re-check vs CAS em activate_lane) ==\n");

    PoolConfig cfg            = pool_default_config();
    cfg.shard_count           = 1;
    cfg.max_shards            = 2;
    cfg.max_auto_expand_lanes = 2;
    cfg.reserve_size          = 2;
    cfg.ring_capacity         = 32;
    cfg.park_idle_threshold_ms = 0;   /* owner ocia rapido p/ entrar no release path */

    ShardedPool* pool = pool_create(&cfg);
    if (!pool) { CHECK(0, "pool_create != NULL"); return; }

    /* 1) expande: active 1->2, lane[1] ganha owner da reserva */
    int expanded = pool_test_activate_next_lane(pool);

    /* 2) arma a barreira na janela orfa e deixa o monitor contrair (active 2->1).
     *    O owner da lane[1] passa o re-check (index 1 >= active 1) e PARA dentro
     *    da janela, ANTES do CAS worker->NULL. */
    pool_test_enable_orphan_window_barrier();
    int arrived = pool_test_wait_orphan_window_arrived(3000);

    /* 3) com o owner congelado na janela, reativa a lane: activate_lane ve
     *    lane->worker != NULL e segue o caminho de REUSO (nao reatribui owner).
     *    active volta a 2 com lane[1].worker ainda apontando p/ o owner preso. */
    int reactivated = arrived ? pool_test_activate_next_lane(pool) : 0;

    /* 4) libera: o owner faz CAS worker->NULL (sucesso) e sai — deixando a lane
     *    ATIVA porem sem owner (orfa). */
    pool_test_release_orphan_window_barrier();
    Sleep(80);

    int transition = pool_test_orphan_transition_observed();
    int orphans    = pool_test_orphan_lane_count(pool);

    printf("    expanded=%d arrived=%d reactivated=%d transition=%d orphans_agora=%d\n",
           expanded, arrived, reactivated, transition, orphans);

    CHECK(expanded && arrived && reactivated,
          "cenario montado (expandiu, owner chegou na janela, reativou)");
    /* Expectativa SAUDAVEL (vai falhar hoje, evidenciando o bug): */
    CHECK(!transition,
          "nenhuma transicao p/ lane orfa (owner nao deveria zerar worker de lane reativada)");
    CHECK(orphans == 0,
          "nenhuma lane ativa sem owner");

    pool_shutdown(pool);
}

/* ───────────────────────────── BUG D ───────────────────────────── */
/* Dono-duplo na reativacao de lane: o owner que sai re-reivindica a lane DEPOIS
 * que activate_lane viu worker==NULL mas ANTES de instalar o novo owner. Com a
 * escrita simples antiga, o install sobrescrevia e dois workers viravam donos da
 * mesma lane. O fix (CAS condicional a NULL) faz o install falhar e reusar. */

typedef struct { ShardedPool* pool; int result; } DboActivateArg;

static DWORD WINAPI dbo_activate_thread(void* raw)
{
    DboActivateArg* a = (DboActivateArg*)raw;
    a->result = pool_test_activate_next_lane(a->pool);  /* para na barreira dbo_activate */
    return 0;
}

static void test_bug_d_double_owner(void)
{
    printf("\n== BUG D: dono-duplo na reativacao (CAS em activate_lane) ==\n");

    PoolConfig cfg            = pool_default_config();
    cfg.shard_count           = 1;
    cfg.max_shards            = 2;
    cfg.max_auto_expand_lanes = 2;
    cfg.reserve_size          = 4;
    cfg.ring_capacity         = 32;
    cfg.park_idle_threshold_ms = 0;   /* sem parking: owner percebe a contracao rapido */

    ShardedPool* pool = pool_create(&cfg);
    if (!pool) { CHECK(0, "pool_create != NULL"); return; }

    /* 1) expande: active 1->2, lane[1] ganha owner w_old. */
    int expanded = pool_test_activate_next_lane(pool);

    /* 2) arma as 2 barreiras; o monitor contrai 2->1 e o owner de lane[1] faz o
     *    CAS worker->NULL e PARA (lane ja sem dono). */
    pool_test_enable_double_owner_window();
    int owner_paused = pool_test_wait_double_owner_owner(8000);

    /* 2.5) congela o autoscale: sem isso o monitor re-contrai a lane e a contracao
     *      "auto-cura" o dono-duplo (o fantasma volta a reserva) antes da medicao. */
    pool_test_freeze_autoscale(1);

    /* 3) reativa a lane numa thread aux; activate_lane PARA antes de instalar. */
    DboActivateArg arg = { pool, 0 };
    HANDLE th = NULL;
    int activate_paused = 0;
    if (owner_paused) {
        th = CreateThread(NULL, 0, dbo_activate_thread, &arg, 0, NULL);
        activate_paused = pool_test_wait_double_owner_activate(8000);
    }

    /* 4) solta o OWNER primeiro: re-reivindica a lane (CAS NULL->w_old) e segue dono. */
    pool_test_release_double_owner_owner();
    Sleep(50);

    /* 5) solta o ACTIVATE: tenta instalar w_new. CAS falha (lane ja tem w_old) e
     *    reusa; a escrita antiga sobrescreveria -> 2 donos. */
    pool_test_release_double_owner_activate();
    if (th) { WaitForSingleObject(th, 5000); CloseHandle(th); }
    Sleep(50);

    /* autoscale congelado: o estado e estavel, basta uma medicao. */
    int max_owners = pool_test_max_owners_per_lane(pool);
    int orphans    = pool_test_orphan_lane_count(pool);

    printf("    expanded=%d owner_paused=%d activate_paused=%d reactivated=%d max_owners=%d orphans=%d\n",
           expanded, owner_paused, activate_paused, arg.result, max_owners, orphans);

    CHECK(expanded && owner_paused && activate_paused,
          "cenario montado (expandiu, owner parou pos-null, activate parou pre-install)");
    CHECK(max_owners <= 1,
          "nenhuma lane com 2 donos (CAS em activate_lane fecha a janela de dono-duplo)");
    CHECK(orphans == 0,
          "nenhuma lane ativa sem owner");

    pool_shutdown(pool);
}

int main(void)
{
    printf("################################################################\n");
    printf("  thread_pool — testes DETERMINISTICOS de bug (POOL_TEST_HOOKS)\n");
    printf("################################################################\n");
    printf("  ncpu=%d\n", xcpu_count());

    test_bug_b_orphan_lane_toctou();   /* B primeiro: A pode envenenar o processo */
    test_bug_d_double_owner();
    test_bug_c_stolen_metric_exposed();
    test_bug_a_alloc_fail_during_worker_creation();  /* por ultimo: pode envenenar */

    printf("\n----------------------------------------------------------------\n");
    printf("  asserts saudaveis que passaram: %d / %d\n", g_pass, g_run);
    printf("  (asserts que FALHARAM = bugs reproduzidos rodando a lib)\n");
    printf("----------------------------------------------------------------\n");
    return 0;
}

#else  /* sem POOL_TEST_HOOKS: nada compila (produc​ao limpa) */
int main(void) { printf("recompile com -DPOOL_TEST_HOOKS\n"); return 0; }
#endif
