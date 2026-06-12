//  MIT License � Modified for Mandatory Attribution
//  
//  Copyright(c) 2025 Sergio Paludo
//
//  github.com/sergiocupa
//  
//  Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files, 
//  to use, copy, modify, merge, publish, distribute, and sublicense the software, including for commercial purposes, provided that:
//  
//     01. The original author�s credit is retained in all copies of the source code;
//     02. The original author�s credit is included in any code generated, derived, or distributed from this software, including templates, libraries, or code - generating scripts.
//  
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED.


#ifndef ATOMICS_H
#define ATOMICS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "../include/xplatbase.h"

#ifdef XPLATBASE_WIN

	typedef volatile LONG    xatomic_int;
	typedef volatile LONG    xatomic_uint32;
	typedef volatile LONG64  xatomic_int64;
	typedef volatile LONG64  xatomic_uint64;
	typedef volatile PVOID   xatomic_ptr;

	XPLATBASE_API void     atomic_initialize(xatomic_int* s, int val);
	XPLATBASE_API void     atomic_set(xatomic_int* s, int val);
	XPLATBASE_API int      atomic_get(xatomic_int* s);
	XPLATBASE_API int      atomic_sub(xatomic_int* s, int val);
	XPLATBASE_API int      atomic_add(xatomic_int* s, int val);
	XPLATBASE_API int      atomic_cas(xatomic_int* s, int* expected, int desired);

	XPLATBASE_API void     atomic_u32_set(xatomic_uint32* s, uint32_t val);
	XPLATBASE_API uint32_t atomic_u32_get(xatomic_uint32* s);
	XPLATBASE_API uint32_t atomic_u32_add(xatomic_uint32* s, uint32_t val);
	XPLATBASE_API int      atomic_u32_cas(xatomic_uint32* s, uint32_t* expected, uint32_t desired);

	XPLATBASE_API void     atomic_set64(xatomic_int64* s, int64_t val);
	XPLATBASE_API int64_t  atomic_get64(xatomic_int64* s);
	XPLATBASE_API int64_t  atomic_add64(xatomic_int64* s, int64_t val);
	XPLATBASE_API int64_t  atomic_sub64(xatomic_int64* s, int64_t val);
	XPLATBASE_API int      atomic_cas64(xatomic_int64* s, int64_t* expected, int64_t desired);

	XPLATBASE_API void     atomic_u64_set(xatomic_uint64* s, uint64_t val);
	XPLATBASE_API uint64_t atomic_u64_get(xatomic_uint64* s);
	XPLATBASE_API uint64_t atomic_u64_add(xatomic_uint64* s, uint64_t val);
	XPLATBASE_API int      atomic_u64_cas(xatomic_uint64* s, uint64_t* expected, uint64_t desired);

	XPLATBASE_API void*    atomic_get_ptr(xatomic_ptr* s);
	XPLATBASE_API void     atomic_set_ptr(xatomic_ptr* target, void* value);
	XPLATBASE_API int      atomic_cas_ptr(xatomic_ptr* target, void** expected, void* desired);

#else

    #include <stdatomic.h>
    #include <stdint.h>

	typedef _Atomic int       xatomic_int;
	typedef _Atomic uint32_t  xatomic_uint32;
	typedef _Atomic int64_t   xatomic_int64;
	typedef _Atomic uint64_t  xatomic_uint64;
	typedef _Atomic(void*)    xatomic_ptr;

	void     atomic_initialize(xatomic_int* s, int val);
	void     atomic_set(xatomic_int* s, int val);
	int      atomic_get(xatomic_int* s);
	int      atomic_sub(xatomic_int* s, int val);
	int      atomic_add(xatomic_int* s, int val);
	int      atomic_cas(xatomic_int* s, int* expected, int desired);

	void     atomic_u32_set(xatomic_uint32* s, uint32_t val);
	uint32_t atomic_u32_get(xatomic_uint32* s);
	uint32_t atomic_u32_add(xatomic_uint32* s, uint32_t val);
	int      atomic_u32_cas(xatomic_uint32* s, uint32_t* expected, uint32_t desired);

	void     atomic_set64(xatomic_int64* s, int64_t val);
	int64_t  atomic_get64(xatomic_int64* s);
	int64_t  atomic_add64(xatomic_int64* s, int64_t val);
	int64_t  atomic_sub64(xatomic_int64* s, int64_t val);
	int      atomic_cas64(xatomic_int64* s, int64_t* expected, int64_t desired);

	void     atomic_u64_set(xatomic_uint64* s, uint64_t val);
	uint64_t atomic_u64_get(xatomic_uint64* s);
	uint64_t atomic_u64_add(xatomic_uint64* s, uint64_t val);
	int      atomic_u64_cas(xatomic_uint64* s, uint64_t* expected, uint64_t desired);

	void*    atomic_get_ptr(xatomic_ptr* s);
	void     atomic_set_ptr(xatomic_ptr* target, void* value);
	int      atomic_cas_ptr(xatomic_ptr* target, void** expected, void* desired);

#endif



#ifdef __cplusplus
}
#endif

#endif /* ATOMICS */
