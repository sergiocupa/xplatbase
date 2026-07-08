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

#include "atomics.h"

/* Definicao unica (linkage externa) das versoes exportadas -- so encaminham
 * para a variante _inline. Uso interno da lib deve chamar direto os _inline. */

void atomic_initialize(xatomic_int* s, int val)
{
    atomic_initialize_inline(s, val);
}

void atomic_set(xatomic_int* s, int val)
{
    atomic_set_inline(s, val);
}

int atomic_get(xatomic_int* s)
{
    return atomic_get_inline(s);
}

void* atomic_get_ptr(xatomic_ptr* s)
{
    return atomic_get_ptr_inline(s);
}

void atomic_set_ptr(xatomic_ptr* target, void* value)
{
    atomic_set_ptr_inline(target, value);
}

int atomic_add(xatomic_int* s, int val)
{
    return atomic_add_inline(s, val);
}

int atomic_sub(xatomic_int* s, int val)
{
    return atomic_sub_inline(s, val);
}

int atomic_cas(xatomic_int* s, int* expected, int desired)
{
    return atomic_cas_inline(s, expected, desired);
}

void atomic_u32_set(xatomic_uint32* s, uint32_t val)
{
    atomic_u32_set_inline(s, val);
}

uint32_t atomic_u32_get(xatomic_uint32* s)
{
    return atomic_u32_get_inline(s);
}

uint32_t atomic_u32_add(xatomic_uint32* s, uint32_t val)
{
    return atomic_u32_add_inline(s, val);
}

int atomic_u32_cas(xatomic_uint32* s, uint32_t* expected, uint32_t desired)
{
    return atomic_u32_cas_inline(s, expected, desired);
}

void atomic_set64(xatomic_int64* s, int64_t val)
{
    atomic_set64_inline(s, val);
}

int64_t atomic_get64(xatomic_int64* s)
{
    return atomic_get64_inline(s);
}

int64_t atomic_add64(xatomic_int64* s, int64_t val)
{
    return atomic_add64_inline(s, val);
}

int64_t atomic_sub64(xatomic_int64* s, int64_t val)
{
    return atomic_sub64_inline(s, val);
}

int atomic_cas64(xatomic_int64* s, int64_t* expected, int64_t desired)
{
    return atomic_cas64_inline(s, expected, desired);
}

void atomic_u64_set(xatomic_uint64* s, uint64_t val)
{
    atomic_u64_set_inline(s, val);
}

uint64_t atomic_u64_get(xatomic_uint64* s)
{
    return atomic_u64_get_inline(s);
}

uint64_t atomic_u64_add(xatomic_uint64* s, uint64_t val)
{
    return atomic_u64_add_inline(s, val);
}

int atomic_u64_cas(xatomic_uint64* s, uint64_t* expected, uint64_t desired)
{
    return atomic_u64_cas_inline(s, expected, desired);
}

int atomic_cas_ptr(xatomic_ptr* target, void** expected, void* desired)
{
    return atomic_cas_ptr_inline(target, expected, desired);
}
