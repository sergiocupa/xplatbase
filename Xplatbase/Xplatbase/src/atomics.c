#include "../include/xplatbase.h"


#ifdef XPLATBASE_WIN



__forceinline void atomic_initialize(xatomic_int* s, int val)
{
    *s = (LONG)val; 
}

__forceinline void atomic_set(xatomic_int* s, int val)
{
    InterlockedExchange(s, (LONG)val);
}

__forceinline int atomic_get(xatomic_int* s)
{
    return (int)ReadAcquire(s);
}

__forceinline int atomic_inc(xatomic_int* s)
{
    return (int)InterlockedExchangeAdd(s, 1);
}

__forceinline int atomic_dec(xatomic_int* s)
{
    return (int)InterlockedExchangeAdd(s, -1);
}

__forceinline int atomic_add(xatomic_int* s, int val)
{
    return (int)InterlockedExchangeAdd(s, (LONG)val);
}

__forceinline int atomic_cas(xatomic_int* s, int* expected, int desired)
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

inline int atomic_inc(xatomic_int* s)
{
    return atomic_fetch_add_explicit(s, 1, memory_order_relaxed);
}

inline int atomic_dec(xatomic_int* s)
{
    return atomic_fetch_sub_explicit(s, 1, memory_order_relaxed);
}

inline int atomic_add(xatomic_int* s, int val)
{
    return atomic_fetch_add_explicit(s, val, memory_order_relaxed);
}

inline int atomic_cas(xatomic_int* s, int* expected, int desired)
{
    return atomic_compare_exchange_strong_explicit(
        s, expected, desired,
        memory_order_acquire,
        memory_order_relaxed);
}



#endif
