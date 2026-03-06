#include "ring_queue.h"
#include "atomics.h"



inline void ring_queue_init(RingQueue* r, int capacity)
{
    atomic_store(&r->head, 0);
    atomic_store(&r->tail, 0);
    r->capacity = capacity;
    r->mask = capacity - 1;
}


inline int ring_queue_count(RingQueue* r)
{
    int t = atomic_load_explicit(&r->tail, memory_order_acquire);
    int h = atomic_load_explicit(&r->head, memory_order_acquire);
    return t - h;
}


inline bool ring_queue_empty(RingQueue* r)
{
    return xring_count(r) == 0;
}


inline bool ring_queue_full(RingQueue* r)
{
    return xring_count(r) >= r->capacity;
}


inline int ring_queue_free(RingQueue* r)
{
    return r->capacity - xring_count(r);
}


inline int ring_queue_pos(RingQueue* r, int index)
{
    return index & r->mask;
}


inline bool ring_queue_push_(RingQueue* r, void* buffer, const void* item, int item_size)
{
    if (xring_full(r))
        return false;

    int t = atomic_load_explicit(&r->tail, memory_order_relaxed);
    int pos = t & r->mask;

    memcpy((char*)buffer + pos * item_size, item, item_size);
    atomic_fetch_add_explicit(&r->tail, 1, memory_order_release);
    return true;
}


inline bool ring_queue_pop_(RingQueue* r, void* buffer, void* item, int item_size)
{
    if (xring_empty(r))
        return false;

    int h = atomic_load_explicit(&r->head, memory_order_relaxed);
    int pos = h & r->mask;

    memcpy(item, (char*)buffer + pos * item_size, item_size);
    atomic_fetch_add_explicit(&r->head, 1, memory_order_release);
    return true;
}


inline bool ring_queue_peek_(RingQueue* r, void* buffer, void* item, int item_size)
{
    if (xring_empty(r))
        return false;

    int h = atomic_load_explicit(&r->head, memory_order_acquire);
    int pos = h & r->mask;

    memcpy(item, (char*)buffer + pos * item_size, item_size);
    return true;
}


inline bool ring_queue_push_mp_(RingQueue* r, void* buffer, const void* item, int item_size)
{
    int t, h;
    do {
        t = atomic_load_explicit(&r->tail, memory_order_relaxed);
        h = atomic_load_explicit(&r->head, memory_order_acquire);

        if (t - h >= r->capacity)
            return false;

    } while (!atomic_compare_exchange_weak_explicit(
        &r->tail, &t, t + 1,
        memory_order_acq_rel, memory_order_relaxed));

    int pos = t & r->mask;
    memcpy((char*)buffer + pos * item_size, item, item_size);
    return true;
}


inline bool ring_queue_pop_mc_(RingQueue* r, void* buffer, void* item, int item_size)
{
    int h, t;
    do {
        h = atomic_load_explicit(&r->head, memory_order_relaxed);
        t = atomic_load_explicit(&r->tail, memory_order_acquire);

        if (h >= t)
            return false;

    } while (!atomic_compare_exchange_weak_explicit(
        &r->head, &h, h + 1,
        memory_order_acq_rel, memory_order_relaxed));

    int pos = h & r->mask;
    memcpy(item, (char*)buffer + pos * item_size, item_size);
    return true;
}