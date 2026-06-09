#include "atomics.h"


#ifdef XPLATBASE_WIN



void atomic_initialize(xatomic_int* s, int val)
{
    *s = (LONG)val;
}

void atomic_set(xatomic_int* s, int val)
{
    InterlockedExchange(s, (LONG)val);
}

int atomic_get(xatomic_int* s)
{
    return (int)ReadAcquire(s);
}

void* atomic_get_ptr(xatomic_ptr* s)
{
    return ReadPointerAcquire((PVOID volatile const*)s);
}

void atomic_set_ptr(xatomic_ptr* target, void* value)
{
    InterlockedExchangePointer((volatile PVOID*)target, value);
}

int atomic_add(xatomic_int* s, int val)
{
    return (int)InterlockedExchangeAddNoFence(s, (LONG)val);
}

int atomic_sub(xatomic_int* s, int val)
{
    return (int)InterlockedExchangeAdd(s, -(LONG)val);
}

int atomic_cas(xatomic_int* s, int* expected, int desired)
{
    LONG old = InterlockedCompareExchange(s, (LONG)desired, (LONG)*expected);
    if (old == (LONG)*expected)
        return 1;
    *expected = (int)old;
    return 0;
}

void atomic_set64(xatomic_int64* s, int64_t val)
{
    InterlockedExchange64(s, (LONG64)val);
}

int64_t atomic_get64(xatomic_int64* s)
{
    return (int64_t)ReadAcquire64(s);
}

int atomic_cas64(xatomic_int64* s, int64_t* expected, int64_t desired)
{
    LONG64 old = InterlockedCompareExchange64(s, (LONG64)desired, (LONG64)*expected);
    if (old == (LONG64)*expected)
        return 1;
    *expected = (int64_t)old;
    return 0;
}

int atomic_cas_ptr(xatomic_ptr* target, void** expected, void* desired)
{
    PVOID old = InterlockedCompareExchangePointer((volatile PVOID*)target, desired, *expected);
    if (old == *expected)
        return 1;
    *expected = old;
    return 0;
}



#else




inline void atomic_initialize(xatomic_int* s, int val)
{
    atomic_init(s, val);
}

inline void atomic_set(xatomic_int* s, int val)
{
    atomic_store_explicit(s, val, memory_order_release);
}

inline int atomic_get(xatomic_int* s)
{
    return atomic_load_explicit(s, memory_order_acquire);
}

inline int atomic_sub(xatomic_int* s, int val)
{
    return atomic_fetch_sub_explicit(s, val, memory_order_relaxed);
}

inline int atomic_add(xatomic_int* s, int val)
{
    return atomic_fetch_add_explicit(s, val, memory_order_relaxed);
}

inline int atomic_cas(xatomic_int* s, int* expected, int desired)
{
    return atomic_compare_exchange_strong_explicit(s, expected, desired, memory_order_acq_rel, memory_order_relaxed);
}

inline void atomic_set64(xatomic_int64* s, int64_t val)
{
    atomic_store_explicit(s, val, memory_order_release);
}

inline int64_t atomic_get64(xatomic_int64* s)
{
    return atomic_load_explicit(s, memory_order_acquire);
}

inline int atomic_cas64(xatomic_int64* s, int64_t* expected, int64_t desired)
{
    return atomic_compare_exchange_strong_explicit(s, expected, desired, memory_order_acq_rel, memory_order_relaxed);
}

inline void* atomic_get_ptr(xatomic_ptr* s)
{
    return atomic_load_explicit(s, memory_order_acquire);
}

inline void atomic_set_ptr(xatomic_ptr* target, void* value)
{
    atomic_store_explicit(target, value, memory_order_release);
}

inline int atomic_cas_ptr(xatomic_ptr* target, void** expected, void* desired)
{
    return atomic_compare_exchange_strong_explicit(target, expected, desired, memory_order_acq_rel, memory_order_relaxed);
}



#endif
