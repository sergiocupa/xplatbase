#include "ring_queue.h"


void ring_queue_init(RingQueue* r, int capacity)
{
    atomic_set(&r->head, 0);
    atomic_set(&r->tail, 0);

    r->capacity = capacity;
    r->mask     = capacity - 1;
}


int ring_queue_count(RingQueue* r)
{
    int t = atomic_get(&r->tail);
    int h = atomic_get(&r->head);
    return t - h;
}


boolean ring_queue_empty(RingQueue* r)
{
    return ring_queue_count(r) == 0;
}


boolean ring_queue_full(RingQueue* r)
{
    return ring_queue_count(r) >= r->capacity;
}


int ring_queue_free(RingQueue* r)
{
    return r->capacity - ring_queue_count(r);
}


int ring_queue_pos(RingQueue* r, int index)
{
    return index & r->mask;
}


boolean ring_queue_push_(RingQueue* r, void* buffer, const void* item, int item_size)
{
    if (ring_queue_full(r))
        return false;

    int t = atomic_get(&r->tail);
    int pos = t & r->mask;

    memcpy((char*)buffer + pos * item_size, item, item_size);
    atomic_add(&r->tail, 1);
    return true;
}


boolean ring_queue_pop_(RingQueue* r, void* buffer, void* item, int item_size)
{
    if (ring_queue_empty(r)) return false;

    int h = atomic_get(&r->head);
    int pos = h & r->mask;

    memcpy(item, (char*)buffer + pos * item_size, item_size);
    atomic_add(&r->head, 1);
    return true;
}


boolean ring_queue_peek_(RingQueue* r, void* buffer, void* item, int item_size)
{
    if (ring_queue_empty(r)) return false;

    int h = atomic_get(&r->head);
    int pos = h & r->mask;

    memcpy(item, (char*)buffer + pos * item_size, item_size);
    return true;
}


boolean ring_queue_push_mp_(RingQueue* r, void* buffer, const void* item, int item_size)
{
    int t, h;
    do 
    {
        t = atomic_get(&r->tail);
        h = atomic_get(&r->head);

        if (t - h >= r->capacity) return false;
    } 
    while (!atomic_cas(&r->tail, &t, t + 1));

    int pos = t & r->mask;
    memcpy((char*)buffer + pos * item_size, item, item_size);
    return true;
}


boolean ring_queue_pop_mc_(RingQueue* r, void* buffer, void* item, int item_size)
{
    int h, t;
    do 
    {
        h = atomic_get(&r->head);
        t = atomic_get(&r->tail);

        if (h >= t) return false;
    } 
    while (!atomic_cas(&r->head, &h, h + 1));

    int pos = h & r->mask;
    memcpy(item, (char*)buffer + pos * item_size, item_size);
    return true;
}

