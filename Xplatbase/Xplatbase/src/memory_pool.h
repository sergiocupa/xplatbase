//  MIT License - Modified for Mandatory Attribution
//
//  Copyright(c) 2025 Sergio Paludo
//
//  github.com/sergiocupa
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED.

#ifndef MEMORY_POOL_H
#define MEMORY_POOL_H

#ifdef __cplusplus
extern "C" {
#endif

    #include "../include/xplatbase.h"
    #include "thread_handler.h"

    typedef struct MemBuffer
    {
        void*  Ptr;
        uint64 Size;
    }
    MemBuffer;

    typedef struct MemPoolStats
    {
        uint64 lanes_created;
        uint64 lanes_destroyed;
        uint64 alloc_count;
        uint64 free_count;
        uint64 sync_refills;
        uint64 async_refills;
        uint64 async_requests;
        uint64 remote_frees;
        uint64 os_reserved_bytes;  /* memoria 4MB-segment reservada do SO agora */
        uint64 cached_chunks;      /* chunks de 64KB livres no cache global */
        uint64 purge_count;        /* segmentos devolvidos ao SO pela purga */
    }
    MemPoolStats;

    void memop_init(void);
    void memop_shutdown(void);

    MemBuffer memop_alloc(uint64 size);
    void memop_free(MemBuffer* buffer);

    /* Fast path de ponteiro cru: void* em registrador, sem o struct MemBuffer.
     * Mesma semantica de memop_alloc/free, mas sem carregar o tamanho de volta. */
    void* memop_alloc_raw(uint64 size);
    void  memop_free_raw(void* ptr);

    void memop_get_stats(MemPoolStats* out_stats);
    void memop_test_reset(void);

    /* Trim explicito: devolve ao SO os segmentos ociosos agora (app-driven).
     * A purga automatica (com atraso/histerese) roda sozinha; isto e opcional. */
    void memop_purge(void);

    void memop_on_created_thread(const Thread* thr);
    void memop_on_ended_thread(const Thread* thr);


    /* ----------------------------------------------------------------------
     * Introspeccao para mem_leak_watch (opt-in, so grava/copia se habilitado).
     * mem_leak_watch NUNCA toca em MemHeap/MemSpan diretamente -- so consome
     * este snapshot, que ja vem desacoplado do layout interno do alocador.
     * ---------------------------------------------------------------------- */

    /* Liga/desliga a captura do site de origem por span em span_create.
     * Desligado (default) = custo zero, nem a captura roda.
     *
     * NOTA DE DESIGN: nao da p/ propagar __FILE__/__LINE__ do chamador ate
     * aqui sem virar macro e mudar a assinatura publica de memop_alloc_raw
     * (usada em toda a lib e por consumidores externos). Em vez disso o site
     * e um mini-backtrace (enderecos crus, CaptureStackBackTrace) capturado
     * DENTRO de span_create -- pega o caminho de chamada real sem tocar em
     * nenhuma assinatura existente. Simbolizacao (endereco -> nome) e feita
     * sob demanda, so no relatorio (mem_leak_watch), nunca no hot path. */
    #define MEMOP_SITE_MAX_FRAMES 6

    void memop_leak_watch_enable(boolean on);

    typedef struct MemSpanInfo
    {
        void*  base;         /* endereco do span (64KB-alinhado) */
        uint32 class_id;     /* MEMOP_CLASS_LARGE p/ blocos grandes (nao rastreados ainda) */
        uint32 stride;       /* tamanho usavel por bloco */
        uint32 block_count;  /* blocos no span */
        uint32 used;         /* blocos em uso agora (nao-atomico, snapshot aproximado) */
        void*  site_frames[MEMOP_SITE_MAX_FRAMES]; /* 0 = vazio/nao capturado */
        int    site_frame_count;
        uint64 created_ms;   /* memop_now_ms() no momento do span_create */
    }
    MemSpanInfo;

    /* Numero de spans vivos AGORA (heaps->spans[] + full lists), para
     * dimensionar o array antes do snapshot. Aproximado sob concorrencia. */
    uint64 memop_span_count(void);

    /* Copia metadado de todos os spans vivos para 'out' (capacidade 'max').
     * Toma o lock global so durante a copia; NAO segura nada depois de
     * retornar -- seguro chamar antes de suspender qualquer thread. */
    uint64 memop_snapshot_spans(MemSpanInfo* out, uint64 max);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_POOL_H */