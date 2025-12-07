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


#ifndef XPLATBASE_H
#define XPLATBASE_H

#ifdef __cplusplus
extern "C" {
#endif

	#include <stdint.h>

	#if defined(_WIN32) || defined(_WIN64)
	    #define XPLATBASE_WIN
	#pragma execution_character_set("utf-8")
	#endif

	#if defined(XPLATBASE_WIN) && !defined(_DEBUG) 
        #define XPLATBASE_API __declspec ( dllexport )
	#else 
	    #define XPLATBASE_API
	#endif 


    #define false 0
    #define true  1

    #define INITIAL_LIST_COUNT 100;

	typedef uint_fast8_t bool;
	typedef uint_fast8_t byte;
	typedef int16_t      int16;
	typedef int32_t      int32;
	typedef int64_t      int64;
	typedef uint16_t     uint16;
	typedef uint32_t     uint32;
	typedef uint64_t     uint64;


	XPLATBASE_WIN void platform_init();


	#ifdef XPLATBASE_WIN

	    #define WIN32_LEAN_AND_MEAN
	    #include <windows.h>
	    #include <stdio.h>

	    // Para tratamento de evento. Tentar capturar antes de encerrar
		#include <dbghelp.h>
		#pragma comment(lib, "dbghelp.lib")


	    #pragma section(".CRT$XCU", read)
		    __declspec(allocate(".CRT$XCU")) static void (*init_ptr)() = platform_init;



	#else 

	   // Para tratamento de evento. Tentar capturar antes de encerrar
       #include <execinfo.h>

	   #include <stdio.h>

		__attribute__((constructor)) void my_init()
		{
			platform_init();
		}

	#endif


    typedef struct 
	{
		bool   Active;
		uint64 TypeSize;
		uint64 Max;
		uint64 Size;
		void*  Data;
	}
	BufferXPB;


	typedef struct
	{
		bool    Active;
		uint64  TypeSize;
		uint64  Max;
		uint64  Count;
		void**  Items;
	}
	ListXPB;


	typedef struct
	{
		bool   Active;
		uint64 Max;
		uint64 Length;
		void*  Content;
	}
	StringXPB;


	typedef struct
	{
		const char* Func;
		const char* File;
		int         Line;
	} 
	CallContextGlobalEvent;

	typedef void (*ErrorHandler)(const CallContextGlobalEvent* ctx, const char* msg);




	//void xpb_event_trigger_error(const CallContextGlobalEvent* ctx, const char* fmt, ...);



#ifdef __cplusplus
}
#endif

#endif /* XPLATBASE */