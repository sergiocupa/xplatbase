//  MIT License - Modified for Mandatory Attribution
//
//  Copyright(c) 2025 Sergio Paludo
//
//  github.com/sergiocupa
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED.

#ifndef MEM_LEAK_WATCH_H
#define MEM_LEAK_WATCH_H

#ifdef __cplusplus
extern "C" {
#endif

    #include "../include/xplatbase.h"

    /*
     * mem_leak_watch -- monitor de vazamento SEM coleta, sobre o memory_pool.
     *
     * Faz o mesmo rastreamento de alcancabilidade que um GC usaria pra
     * decidir o que liberar (raizes -> ponteiros alcancados -> spans
     * marcados), mas NUNCA libera nada. O que nao foi alcancado vira um
     * aviso em log, com o mini-backtrace de onde o span foi criado.
     *
     * Duas camadas de custo:
     *   - check barato (timer): so le memop_get_stats(), sem suspender
     *     thread nenhuma. Roda com frequencia alta sem problema.
     *   - scan caro (so quando o check acusa risco): suspende cada thread
     *     registrada (uma de cada vez), le registradores+pilha como raizes,
     *     percorre os spans do memory_pool, reporta o que sobrou.
     *
     * Limitacoes conhecidas (v1, documentadas de proposito -- confirmadas
     * testando, nao so teoricas):
     *   - so Windows por enquanto (thread_activity_win / StackWalk64).
     *   - so rastreia spans de size-class (alocacoes <=16KB); blocos LARGE
     *     nao entram na varredura ainda (nao ficam em heap->spans/full).
     *   - nao varre secao de dados global/estatica (.data/.bss) como raiz,
     *     so pilha+registradores de cada thread viva. Qualquer ponteiro cuja
     *     UNICA referencia viva seja uma variavel global/estatica aparece
     *     como falso "vazamento" (confirmado no teste: a propria ListXPB
     *     interna de thread_handler.c, que e static, e sempre reportada).
     *   - SO enxerga threads criadas via thread_create() (o wrapper desta
     *     lib) -- thread_enum() so sabe da lista que thread_handler.c
     *     mantem. Uma thread OS crua (CreateThread/pthread_create direto,
     *     ou threads de outra lib como TBB) fica INVISIVEL pro scan; tudo
     *     que ela mantiver vivo so na propria pilha vira falso vazamento.
     *     Confirmado testando: precisa passar pelo thread_create() da lib
     *     pra ser visto como raiz.
     *   - marca por SPAN inteiro (nao por bloco individual): um span com
     *     100 blocos e 1 referenciado marca os 100 como alcancaveis.
     *   - snapshot de spans e best-effort: le as listas do dono sem lock
     *     dedicado (mesma discussao de memop_snapshot_spans em memory_pool.h).
     *   - usa SuspendThread/GetThreadContext, que a Microsoft documenta como
     *     arriscado fora de debugger (pode congelar uma thread no meio de um
     *     lock que o proprio scan precisaria) -- mitigado suspendendo uma
     *     thread por vez (nao todas de uma vez) e nao alocando/chamando
     *     dbghelp enquanto alguma thread esta suspensa.
     */

    typedef struct MemLeakWatchConfig
    {
        boolean enabled;               /* liga a gravacao de site + o timer */
        uint32  interval_ms;           /* 0 = usa default (60000 release / 1000 debug) */
        uint64  warn_threshold_bytes;  /* os_reserved_bytes acima disto -> escalona p/ WARN */
        uint64  crit_threshold_bytes;  /* acima disto -> escalona p/ CRITICAL (com contexto das threads) */
        const char* log_path;          /* NULL = "mem_leak_watch.log" no diretorio corrente */
    }
    MemLeakWatchConfig;

    /* Preenche 'out' com os defaults (interval por _DEBUG, limiares conservadores). */
    XPLATBASE_API void    mem_leak_watch_default_config(MemLeakWatchConfig* out);

    /* Inicia o monitor (thread dedicada). cfg=NULL usa os defaults com enabled=true.
     * Chamar de novo sem parar antes e no-op (retorna false). */
    XPLATBASE_API boolean mem_leak_watch_start(const MemLeakWatchConfig* cfg);

    /* Para o monitor e junta a thread. Seguro chamar mesmo se nao iniciado. */
    XPLATBASE_API void    mem_leak_watch_stop(void);

    /* Forca uma varredura agora (fora do timer) -- uso manual/teste. Nivel
     * decidido pelos mesmos limiares configurados. */
    XPLATBASE_API void    mem_leak_watch_scan_now(void);

#ifdef __cplusplus
}
#endif

#endif /* MEM_LEAK_WATCH_H */
