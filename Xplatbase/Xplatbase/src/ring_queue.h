//  MIT License – Modified for Mandatory Attribution
//  
//  Copyright(c) 2025 Sergio Paludo
//
//  github.com/sergiocupa
//  
//  Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files, 
//  to use, copy, modify, merge, publish, distribute, and sublicense the software, including for commercial purposes, provided that:
//  
//     01. The original author’s credit is retained in all copies of the source code;
//     02. The original author’s credit is included in any code generated, derived, or distributed from this software, including templates, libraries, or code - generating scripts.
//  
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED.


#ifndef RING_QUEUE_H
#define RING_QUEUE_H

#ifdef __cplusplus
extern "C" {
#endif

    #include "atomics.h"

    typedef struct _RingQueue
    {
        xatomic_int head;
        xatomic_int tail;
        int         capacity;
        int         mask;
    }
    RingQueue;


    inline void    ring_queue_init(RingQueue* r, int capacity);
    inline int     ring_queue_count(RingQueue* r);
    inline boolean ring_queue_empty(RingQueue* r);
    inline boolean ring_queue_full(RingQueue* r);
    inline int     ring_queue_free(RingQueue* r);
    inline int     ring_queue_pos(RingQueue* r, int index);
    inline boolean ring_queue_push_(RingQueue* r, void* buffer, const void* item, int item_size);
    inline boolean ring_queue_pop_(RingQueue* r, void* buffer, void* item, int item_size);
    inline boolean ring_queue_peek_(RingQueue* r, void* buffer, void* item, int item_size);
    inline boolean ring_queue_push_mp_(RingQueue* r, void* buffer, const void* item, int item_size);
    inline boolean ring_queue_pop_mc_(RingQueue* r, void* buffer, void* item, int item_size);


    #define xring_push(r, buffer, item_ptr)    ring_queue_push_((r), (buffer), (item_ptr), sizeof(*(item_ptr)))
    #define xring_pop(r, buffer, item_ptr)     ring_queue_pop_((r), (buffer), (item_ptr), sizeof(*(item_ptr)))
    #define xring_peek(r, buffer, item_ptr)    ring_queue_peek_((r), (buffer), (item_ptr), sizeof(*(item_ptr)))
    #define xring_push_mp(r, buffer, item_ptr) ring_queue_push_mp_((r), (buffer), (item_ptr), sizeof(*(item_ptr)))
    #define xring_pop_mc(r, buffer, item_ptr)  ring_queue_pop_mc_((r), (buffer), (item_ptr), sizeof(*(item_ptr)))



#ifdef __cplusplus
}
#endif

#endif /* RING_QUEUE */