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
    /* Com fence: em x86/x64 gera o mesmo 'lock xadd' do NoFence (custo zero),
     * mas garante a ordenacao em ARM, onde o NoFence permitiria reordenacao
     * (ex.: pool_api_enter incrementa api_users e logo le shutdown). */
    return (int)InterlockedExchangeAdd(s, (LONG)val);
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

void atomic_u32_set(xatomic_uint32* s, uint32_t val)
{
    InterlockedExchange(s, (LONG)val);
}

uint32_t atomic_u32_get(xatomic_uint32* s)
{
    return (uint32_t)ReadAcquire(s);
}

uint32_t atomic_u32_add(xatomic_uint32* s, uint32_t val)
{
    /* idem atomic_add: fence sem custo em x86/x64, correto em ARM. */
    return (uint32_t)InterlockedExchangeAdd(s, (LONG)val);
}

int atomic_u32_cas(xatomic_uint32* s, uint32_t* expected, uint32_t desired)
{
    LONG old = InterlockedCompareExchange(s, (LONG)desired, (LONG)*expected);
    if ((uint32_t)old == *expected)
        return 1;
    *expected = (uint32_t)old;
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

int64_t atomic_add64(xatomic_int64* s, int64_t val)
{
    return (int64_t)InterlockedExchangeAdd64(s, (LONG64)val);
}

int64_t atomic_sub64(xatomic_int64* s, int64_t val)
{
    return (int64_t)InterlockedExchangeAdd64(s, -(LONG64)val);
}

int atomic_cas64(xatomic_int64* s, int64_t* expected, int64_t desired)
{
    LONG64 old = InterlockedCompareExchange64(s, (LONG64)desired, (LONG64)*expected);
    if (old == (LONG64)*expected)
        return 1;
    *expected = (int64_t)old;
    return 0;
}

void atomic_u64_set(xatomic_uint64* s, uint64_t val)
{
    InterlockedExchange64(s, (LONG64)val);
}

uint64_t atomic_u64_get(xatomic_uint64* s)
{
    return (uint64_t)ReadAcquire64(s);
}

uint64_t atomic_u64_add(xatomic_uint64* s, uint64_t val)
{
    return (uint64_t)InterlockedExchangeAdd64(s, (LONG64)val);
}

int atomic_u64_cas(xatomic_uint64* s, uint64_t* expected, uint64_t desired)
{
    LONG64 old = InterlockedCompareExchange64(s, (LONG64)desired, (LONG64)*expected);
    if ((uint64_t)old == *expected)
        return 1;
    *expected = (uint64_t)old;
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




void atomic_initialize(xatomic_int* s, int val)
{
    atomic_init(s, val);
}

void atomic_set(xatomic_int* s, int val)
{
    atomic_store_explicit(s, val, memory_order_release);
}

int atomic_get(xatomic_int* s)
{
    return atomic_load_explicit(s, memory_order_acquire);
}

int atomic_sub(xatomic_int* s, int val)
{
    return atomic_fetch_sub_explicit(s, val, memory_order_relaxed);
}

int atomic_add(xatomic_int* s, int val)
{
    return atomic_fetch_add_explicit(s, val, memory_order_relaxed);
}

int atomic_cas(xatomic_int* s, int* expected, int desired)
{
    return atomic_compare_exchange_strong_explicit(s, expected, desired, memory_order_acq_rel, memory_order_relaxed);
}

void atomic_u32_set(xatomic_uint32* s, uint32_t val)
{
    atomic_store_explicit(s, val, memory_order_release);
}

uint32_t atomic_u32_get(xatomic_uint32* s)
{
    return atomic_load_explicit(s, memory_order_acquire);
}

uint32_t atomic_u32_add(xatomic_uint32* s, uint32_t val)
{
    return atomic_fetch_add_explicit(s, val, memory_order_relaxed);
}

int atomic_u32_cas(xatomic_uint32* s, uint32_t* expected, uint32_t desired)
{
    return atomic_compare_exchange_strong_explicit(
        s, expected, desired, memory_order_acq_rel, memory_order_relaxed);
}

void atomic_set64(xatomic_int64* s, int64_t val)
{
    atomic_store_explicit(s, val, memory_order_release);
}

int64_t atomic_get64(xatomic_int64* s)
{
    return atomic_load_explicit(s, memory_order_acquire);
}

int64_t atomic_add64(xatomic_int64* s, int64_t val)
{
    return atomic_fetch_add_explicit(s, val, memory_order_relaxed);
}

int64_t atomic_sub64(xatomic_int64* s, int64_t val)
{
    return atomic_fetch_sub_explicit(s, val, memory_order_relaxed);
}

int atomic_cas64(xatomic_int64* s, int64_t* expected, int64_t desired)
{
    return atomic_compare_exchange_strong_explicit(s, expected, desired, memory_order_acq_rel, memory_order_relaxed);
}

void atomic_u64_set(xatomic_uint64* s, uint64_t val)
{
    atomic_store_explicit(s, val, memory_order_release);
}

uint64_t atomic_u64_get(xatomic_uint64* s)
{
    return atomic_load_explicit(s, memory_order_acquire);
}

uint64_t atomic_u64_add(xatomic_uint64* s, uint64_t val)
{
    return atomic_fetch_add_explicit(s, val, memory_order_relaxed);
}

int atomic_u64_cas(xatomic_uint64* s, uint64_t* expected, uint64_t desired)
{
    return atomic_compare_exchange_strong_explicit(
        s, expected, desired, memory_order_acq_rel, memory_order_relaxed);
}

void* atomic_get_ptr(xatomic_ptr* s)
{
    return atomic_load_explicit(s, memory_order_acquire);
}

void atomic_set_ptr(xatomic_ptr* target, void* value)
{
    atomic_store_explicit(target, value, memory_order_release);
}

int atomic_cas_ptr(xatomic_ptr* target, void** expected, void* desired)
{
    return atomic_compare_exchange_strong_explicit(target, expected, desired, memory_order_acq_rel, memory_order_relaxed);
}



#endif
