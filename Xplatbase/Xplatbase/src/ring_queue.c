// MIT License - Modified for Mandatory Attribution
//
// Copyright(c) 2025 Sergio Paludo
//
// github.com/sergiocupa
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files, to use, copy, modify,
// merge, publish, distribute, and sublicense the software, including for
// commercial purposes, provided that:
//
// 01. The original author's credit is retained in all copies of the source code;
// 02. The original author's credit is included in any code generated, derived,
//     or distributed from this software, including templates, libraries, or
//     code-generating scripts.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED.

#include "ring_queue.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef POOL_TEST_HOOKS
static volatile LONG g_ring_queue_test_alloc_after = -1;

void ring_queue_test_fail_alloc_after(int successful_allocations)
{
    InterlockedExchange(&g_ring_queue_test_alloc_after, successful_allocations);
}

static int ring_queue_test_should_fail_alloc(void)
{
    for (;;) {
        LONG current = InterlockedCompareExchange(&g_ring_queue_test_alloc_after, 0, 0);
        if (current < 0) return 0;
        if (current == 0) {
            if (InterlockedCompareExchange(&g_ring_queue_test_alloc_after, -1, 0) == 0)
                return 1;
            continue;
        }
        if (InterlockedCompareExchange(&g_ring_queue_test_alloc_after,
                                       current - 1, current) == current)
            return 0;
    }
}

static void* ring_queue_test_malloc(size_t size)
{
    if (ring_queue_test_should_fail_alloc()) return NULL;
    return malloc(size);
}

#define malloc ring_queue_test_malloc
#endif

static void ring_writer_lock(RingQueue* r, int pos)
{
    for (;;) {
        int expected = 0;
        if (atomic_cas_inline(&r->peek_guard[pos], &expected, -1))
            return;
    }
}

static void ring_writer_unlock(RingQueue* r, int pos)
{
    atomic_set_inline(&r->peek_guard[pos], 0);
}

static boolean ring_reader_lock(RingQueue* r, int pos)
{
    int readers = atomic_get_inline(&r->peek_guard[pos]);
    for (;;) {
        if (readers < 0 || readers == INT32_MAX)
            return false;
        if (atomic_cas_inline(&r->peek_guard[pos], &readers, readers + 1))
            return true;
    }
}

static void ring_reader_unlock(RingQueue* r, int pos)
{
    atomic_sub_inline(&r->peek_guard[pos], 1);
}

void ring_queue_init(RingQueue* r, int capacity)
{
    if (!r) return;

    r->capacity = 0;
    r->mask = 0;
    r->seqno = NULL;
    r->peek_guard = NULL;
    atomic_u32_set_inline(&r->head, 0);
    atomic_u32_set_inline(&r->tail, 0);

    if (capacity <= 0 || capacity > (1 << 30) ||
        (capacity & (capacity - 1)) != 0)
        return;

    r->capacity = capacity;
    r->mask = capacity - 1;
    r->seqno = (xatomic_uint32*)malloc((size_t)capacity * sizeof(xatomic_uint32));
    r->peek_guard = (xatomic_int*)malloc((size_t)capacity * sizeof(xatomic_int));
    if (!r->seqno || !r->peek_guard) {
        free(r->seqno);
        free(r->peek_guard);
        r->seqno = NULL;
        r->peek_guard = NULL;
        r->capacity = 0;
        r->mask = 0;
        return;
    }

    for (int i = 0; i < capacity; i++) {
        atomic_u32_set_inline(&r->seqno[i], (uint32_t)i);
        atomic_set_inline(&r->peek_guard[i], 0);
    }
}

void ring_queue_destroy(RingQueue* r)
{
    if (!r) return;
    free(r->seqno);
    free(r->peek_guard);
    r->seqno = NULL;
    r->peek_guard = NULL;
    r->capacity = 0;
    r->mask = 0;
}

int ring_queue_count(RingQueue* r)
{
    if (!r || r->capacity <= 0) return 0;

    for (int retry = 0; retry < 4; retry++) {
        uint32_t h1 = atomic_u32_get_inline(&r->head);
        uint32_t t = atomic_u32_get_inline(&r->tail);
        uint32_t h2 = atomic_u32_get_inline(&r->head);
        if (h1 == h2) {
            uint32_t count = t - h1;
            return count > (uint32_t)r->capacity ? r->capacity : (int)count;
        }
    }

    uint32_t h = atomic_u32_get_inline(&r->head);
    uint32_t t = atomic_u32_get_inline(&r->tail);
    uint32_t count = t - h;
    return count > (uint32_t)r->capacity ? r->capacity : (int)count;
}

boolean ring_queue_empty(RingQueue* r)
{
    return ring_queue_count(r) == 0;
}

boolean ring_queue_full(RingQueue* r)
{
    return !r || r->capacity <= 0 || ring_queue_count(r) >= r->capacity;
}

int ring_queue_free(RingQueue* r)
{
    if (!r || r->capacity <= 0) return 0;
    return r->capacity - ring_queue_count(r);
}

int ring_queue_pos(RingQueue* r, int index)
{
    return r ? index & r->mask : 0;
}

boolean ring_queue_push_(RingQueue* r, void* buffer, const void* item, int item_size)
{
    if (!r || !r->seqno || !r->peek_guard || !buffer || !item ||
        item_size <= 0 || ring_queue_full(r))
        return false;

    uint32_t t = atomic_u32_get_inline(&r->tail);
    int pos = (int)(t & (uint32_t)r->mask);
    ring_writer_lock(r, pos);
    memcpy((char*)buffer + (size_t)pos * (size_t)item_size, item, (size_t)item_size);
    ring_writer_unlock(r, pos);
    atomic_u32_set_inline(&r->seqno[pos], t + 1u);
    atomic_u32_add_inline(&r->tail, 1u);
    return true;
}

boolean ring_queue_pop_(RingQueue* r, void* buffer, void* item, int item_size)
{
    if (!r || !r->seqno || !buffer || !item || item_size <= 0 ||
        ring_queue_empty(r))
        return false;

    uint32_t h = atomic_u32_get_inline(&r->head);
    int pos = (int)(h & (uint32_t)r->mask);
    memcpy(item, (char*)buffer + (size_t)pos * (size_t)item_size, (size_t)item_size);
    atomic_u32_set_inline(&r->seqno[pos], h + (uint32_t)r->capacity);
    atomic_u32_add_inline(&r->head, 1u);
    return true;
}

boolean ring_queue_peek_(RingQueue* r, void* buffer, void* item, int item_size)
{
    if (!r || !r->seqno || !r->peek_guard || !buffer || !item || item_size <= 0)
        return false;

    for (int retry = 0; retry < 4; retry++) {
        uint32_t h = atomic_u32_get_inline(&r->head);
        int pos = (int)(h & (uint32_t)r->mask);
        if (atomic_u32_get_inline(&r->seqno[pos]) != h + 1u)
            return false;
        if (!ring_reader_lock(r, pos))
            continue;
        if (atomic_u32_get_inline(&r->head) == h &&
            atomic_u32_get_inline(&r->seqno[pos]) == h + 1u) {
            memcpy(item, (char*)buffer + (size_t)pos * (size_t)item_size,
                   (size_t)item_size);
            ring_reader_unlock(r, pos);
            return true;
        }
        ring_reader_unlock(r, pos);
    }
    return false;
}

/*
 * Bounded MPMC queue based on per-slot sequence numbers. All position
 * arithmetic is uint32_t modular arithmetic, so wrap is defined by C.
 */
boolean ring_queue_push_mp_(RingQueue* r, void* buffer, const void* item, int item_size)
{
    if (!r || !r->seqno || !r->peek_guard || !buffer || !item || item_size <= 0)
        return false;

    uint32_t t;
    for (;;) {
        t = atomic_u32_get_inline(&r->tail);
        int pos = (int)(t & (uint32_t)r->mask);
        uint32_t seq = atomic_u32_get_inline(&r->seqno[pos]);
        uint32_t diff = seq - t;

        if (diff == 0) {
            if (atomic_u32_cas_inline(&r->tail, &t, t + 1u))
                break;
        } else if (diff & UINT32_C(0x80000000)) {
            return false;
        }
    }

    int pos = (int)(t & (uint32_t)r->mask);
    ring_writer_lock(r, pos);
    memcpy((char*)buffer + (size_t)pos * (size_t)item_size, item, (size_t)item_size);
    ring_writer_unlock(r, pos);
    atomic_u32_set_inline(&r->seqno[pos], t + 1u);
    return true;
}

boolean ring_queue_pop_mc_(RingQueue* r, void* buffer, void* item, int item_size)
{
    if (!r || !r->seqno || !buffer || !item || item_size <= 0)
        return false;

    uint32_t h;
    for (;;) {
        h = atomic_u32_get_inline(&r->head);
        int pos = (int)(h & (uint32_t)r->mask);
        uint32_t seq = atomic_u32_get_inline(&r->seqno[pos]);
        uint32_t diff = seq - (h + 1u);

        if (diff == 0) {
            if (atomic_u32_cas_inline(&r->head, &h, h + 1u))
                break;
        } else if (diff & UINT32_C(0x80000000)) {
            return false;
        }
    }

    int pos = (int)(h & (uint32_t)r->mask);
    memcpy(item, (char*)buffer + (size_t)pos * (size_t)item_size, (size_t)item_size);
    atomic_u32_set_inline(&r->seqno[pos], h + (uint32_t)r->capacity);
    return true;
}
