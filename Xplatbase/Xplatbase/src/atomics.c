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

inline void* atomic_get_ptr(xatomic_ptr* s)
{
    return atomic_load_explicit(s, memory_order_acquire);
}

inline void atomic_set_ptr(xatomic_ptr* target, void* value)
{
    atomic_store_explicit(target, value, memory_order_release);
}



#endif
