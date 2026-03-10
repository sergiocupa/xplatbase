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


#ifndef ATOMICS_H
#define ATOMICS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "../include/xplatbase.h"

#ifdef XPLATBASE_WIN

	typedef volatile LONG xatomic_int;

	XPLATBASE_API void atomic_initialize(xatomic_int* s, int val);
	XPLATBASE_API void atomic_set(xatomic_int* s, int val);
	XPLATBASE_API int  atomic_get(xatomic_int* s);
	XPLATBASE_API int  atomic_sub(xatomic_int* s, int val);
	XPLATBASE_API int  atomic_add(xatomic_int* s, int val);
	XPLATBASE_API int  atomic_cas(xatomic_int* s, int* expected, int desired);

#else 

    #include <stdatomic.h>

	typedef _Atomic int xatomic_int;

	inline void atomic_initialize(xatomic_int* s, int val);
	inline void atomic_set(xatomic_int* s, int val);
	inline int  atomic_get(xatomic_int* s);
	inline int  atomic_sub(xatomic_int* s, int val);
	inline int  atomic_add(xatomic_int* s, int val);
	inline int  atomic_cas(xatomic_int* s, int* expected, int desired);

#endif



#ifdef __cplusplus
}
#endif

#endif /* ATOMICS */