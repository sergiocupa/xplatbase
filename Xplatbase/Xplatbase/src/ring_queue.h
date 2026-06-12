//  MIT License � Modified for Mandatory Attribution
//  
//  Copyright(c) 2025 Sergio Paludo
//
//  github.com/sergiocupa
//  
//  Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files, 
//  to use, copy, modify, merge, publish, distribute, and sublicense the software, including for commercial purposes, provided that:
//  
//     01. The original author�s credit is retained in all copies of the source code;
//     02. The original author�s credit is included in any code generated, derived, or distributed from this software, including templates, libraries, or code - generating scripts.
//  
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED.


#ifndef RING_QUEUE_H
#define RING_QUEUE_H

#ifdef __cplusplus
extern "C" {
#endif

    #include "atomics.h"

    /* Tamanho de linha de cache (x86/x64 e ARM comuns = 64 bytes). */
    #ifndef XPL_CACHELINE
    #define XPL_CACHELINE 64
    #endif
    #if defined(_MSC_VER)
    #define XPL_ALIGN(n) __declspec(align(n))
    #else
    #define XPL_ALIGN(n) __attribute__((aligned(n)))
    #endif

    /* head e tail ficam em linhas de cache SEPARADAS: o produtor escreve tail e o
     * consumidor/ladrao escreve head; sem o padding, ambos compartilham a mesma
     * linha e geram ping-pong de coerencia (false sharing) sob work-stealing.
     * O gap de XPL_CACHELINE bytes garante linhas distintas para qualquer base de
     * alocacao (X e X+64 sempre caem em linhas de 64B diferentes). */
    typedef struct XPL_ALIGN(XPL_CACHELINE) _RingQueue
    {
        xatomic_uint32 tail;                                   /* hot: produtor   */
        char           _pad_tail[XPL_CACHELINE - sizeof(xatomic_uint32)];
        xatomic_uint32 head;                                   /* hot: consumidor */
        char           _pad_head[XPL_CACHELINE - sizeof(xatomic_uint32)];
        int          capacity;       /* read-mostly: setados no init             */
        int          mask;
        xatomic_uint32* seqno;       /* sequence number por slot (Vyukov MPMC) */
        xatomic_int*    peek_guard;  /* -1 writer; >= 0 leitores de peek */
    }
    RingQueue;


    XPLATBASE_API void    ring_queue_init(RingQueue* r, int capacity);
    XPLATBASE_API void    ring_queue_destroy(RingQueue* r);
    XPLATBASE_API int     ring_queue_count(RingQueue* r);
    XPLATBASE_API boolean ring_queue_empty(RingQueue* r);
    XPLATBASE_API boolean ring_queue_full(RingQueue* r);
    XPLATBASE_API int     ring_queue_free(RingQueue* r);
    XPLATBASE_API int     ring_queue_pos(RingQueue* r, int index);
    XPLATBASE_API boolean ring_queue_push_(RingQueue* r, void* buffer, const void* item, int item_size);
    XPLATBASE_API boolean ring_queue_pop_(RingQueue* r, void* buffer, void* item, int item_size);
    XPLATBASE_API boolean ring_queue_peek_(RingQueue* r, void* buffer, void* item, int item_size);
    XPLATBASE_API boolean ring_queue_push_mp_(RingQueue* r, void* buffer, const void* item, int item_size);
    XPLATBASE_API boolean ring_queue_pop_mc_(RingQueue* r, void* buffer, void* item, int item_size);


    #define xring_push(r, buffer, item_ptr)    ring_queue_push_((r), (buffer), (item_ptr), sizeof(*(item_ptr)))
    #define xring_pop(r, buffer, item_ptr)     ring_queue_pop_((r), (buffer), (item_ptr), sizeof(*(item_ptr)))
    #define xring_peek(r, buffer, item_ptr)    ring_queue_peek_((r), (buffer), (item_ptr), sizeof(*(item_ptr)))
    #define xring_push_mp(r, buffer, item_ptr) ring_queue_push_mp_((r), (buffer), (item_ptr), sizeof(*(item_ptr)))
    #define xring_pop_mc(r, buffer, item_ptr)  ring_queue_pop_mc_((r), (buffer), (item_ptr), sizeof(*(item_ptr)))



#ifdef __cplusplus
}
#endif

#endif /* RING_QUEUE */
