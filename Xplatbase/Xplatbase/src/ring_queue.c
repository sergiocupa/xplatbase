#include "ring_queue.h"
#include <stdlib.h>


void ring_queue_init(RingQueue* r, int capacity)
{
    atomic_set(&r->head, 0);
    atomic_set(&r->tail, 0);

    r->capacity = capacity;
    r->mask     = capacity - 1;

    r->seqno = (xatomic_int*)malloc(capacity * sizeof(xatomic_int));
    for (int i = 0; i < capacity; i++)
        atomic_set(&r->seqno[i], i);
}


void ring_queue_destroy(RingQueue* r)
{
    free(r->seqno);
    r->seqno = NULL;
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


/*
 * push_mp_ / pop_mc_: algoritmo MPMC de Dmitry Vyukov com sequence numbers
 * por slot. Garante que o consumer só lê depois que o producer terminou de
 * escrever, eliminando a race condition head/tail clássica.
 *
 * Invariante de seqno[pos]:
 *   == tail          → slot livre para escrita neste turno
 *   == tail + 1      → dado escrito, pronto para leitura
 *   == head + cap    → slot liberado pelo consumer, livre para próximo turno
 */

boolean ring_queue_push_mp_(RingQueue* r, void* buffer, const void* item, int item_size)
{
    int t;
    for (;;)
    {
        t       = atomic_get(&r->tail);
        int pos = t & r->mask;
        int seq = atomic_get(&r->seqno[pos]);
        int diff = seq - t;

        if (diff == 0)
        {
            /* slot livre para escrever — tenta reservar */
            if (atomic_cas(&r->tail, &t, t + 1))
                break;
        }
        else if (diff < 0)
        {
            return false;   /* fila cheia */
        }
        /* diff > 0: outro producer ganhou o CAS, retry */
    }

    int pos = t & r->mask;
    memcpy((char*)buffer + pos * item_size, item, item_size);
    atomic_set(&r->seqno[pos], t + 1);     /* sinaliza dado pronto para leitura */
    return true;
}


boolean ring_queue_pop_mc_(RingQueue* r, void* buffer, void* item, int item_size)
{
    int h;
    for (;;)
    {
        h       = atomic_get(&r->head);
        int pos = h & r->mask;
        int seq = atomic_get(&r->seqno[pos]);
        int diff = seq - (h + 1);

        if (diff == 0)
        {
            /* dado pronto — tenta reivindicar o slot */
            if (atomic_cas(&r->head, &h, h + 1))
                break;
        }
        else if (diff < 0)
        {
            return false;   /* fila vazia ou producer ainda escrevendo */
        }
        /* diff > 0: outro consumer ganhou o CAS, retry */
    }

    int pos = h & r->mask;
    memcpy(item, (char*)buffer + pos * item_size, item_size);
    atomic_set(&r->seqno[pos], h + r->capacity);   /* libera slot para próximo turno */
    return true;
}

