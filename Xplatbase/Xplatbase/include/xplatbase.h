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

	// Modo de ligacao. O padrao continua ESTATICO: quem ja usa a lib assim nao muda nada.
	//   XPLATBASE_BUILD_SHARED -> compilando a propria biblioteca compartilhada
	//   XPLATBASE_USE_SHARED   -> consumindo a biblioteca compartilhada
	//   (nenhum dos dois)      -> estatico
	// O que havia antes exportava so em Release e nao tinha o ramo de importacao, entao a
	// versao compartilhada nao era utilizavel -- e sem ela cada modulo fica com a SUA copia
	// do pool de memoria e do registro de threads (ver prep-xplatbase/PLANO.md no appserver).
	#if defined(XPLATBASE_WIN)
	    #if   defined(XPLATBASE_BUILD_SHARED)
	        #define XPLATBASE_API __declspec ( dllexport )
	    #elif defined(XPLATBASE_USE_SHARED)
	        #define XPLATBASE_API __declspec ( dllimport )
	    #else
	        #define XPLATBASE_API
	    #endif
	#else
	    #if defined(XPLATBASE_BUILD_SHARED)
	        #define XPLATBASE_API __attribute__ ( ( visibility ( "default" ) ) )
	    #else
	        #define XPLATBASE_API
	    #endif
	#endif

	#ifdef _MSC_VER
	    #define STATIC_INLINE static __forceinline
	#else
	    #define STATIC_INLINE static inline __attribute__((always_inline))
	#endif


#ifndef __cplusplus
    #define false 0
    #define true  1
#endif

    #define INITIAL_LIST_COUNT    100
	#define INITIAL_STRING_LENGTH 50


	typedef uint_fast8_t boolean;
	typedef uint_fast8_t byte;
	typedef int16_t      int16;
	typedef int32_t      int32;
	typedef int64_t      int64;
	typedef uint16_t     uint16;
	typedef uint32_t     uint32;
	typedef uint64_t     uint64;


	// Inicializa a lib: pool de memoria, registro de threads, POOL DE TAREFAS global e
	// monitor de vazamento. Roda SOZINHO na carga da lib (inicializador da CRT / construtor),
	// sem argumentos; chamar de novo nao faz nada. Por isso nao recebe parametros: a
	// configuracao do pool e feita DEPOIS, por funcao, e vale na hora.
	//
	// POOL DE TAREFAS -- VIGIAS DESLIGADOS POR PADRAO (economia de energia):
	//   parado, o pool nao gasta CPU; a 1a tarefa depois de uma pausa espera ~15-20 us para
	//   comecar (o sistema acordar um worker). Se a prontidao importar mais que a bateria:
	//       pool_vigias(1);   // ~2-3 us de prontidao, ao custo de ~1 nucleo sempre ocupado
	//       pool_vigias(0);   // volta ao padrao
	//   Detalhes e numeros em src/thread_pool.h (pool_vigias / pool_vigias_relative).
	XPLATBASE_API void platform_init(void);


	// ---- instancia unica ---------------------------------------------------
	// O pool de memoria, o registro de threads e o pool de tarefas sao estado global de
	// arquivo: cada modulo que linkar o xplatbase ESTATICAMENTE ganha a propria copia, e ai
	// a contabilidade racha e liberar memoria entre modulos corrompe o heap. Estas funcoes
	// detectam esse caso em vez de deixa-lo silencioso (ver src/xplat_instance.c).
	typedef struct
	{
	    uint32 AbiVersion;
	    uint64 Identity;        // endereco marcador da copia -- difere entre instancias
	    char   Module[260];     // modulo (exe/dll) de onde a instancia veio
	}
	XplatInstanceInfo;

	// Registra esta copia no processo. Chamado por memop_init; idempotente.
	XPLATBASE_API void xplat_instance_register(void);
	XPLATBASE_API void xplat_instance_release(void);

	// 0 = instancia unica; 1 = JA existe outra (first recebe a que chegou primeiro).
	XPLATBASE_API int  xplat_instance_check(XplatInstanceInfo* first);

	// Modulo de onde ESTA copia veio.
	XPLATBASE_API const char* xplat_instance_module(void);

	// Duplicata aborta o processo? Padrao: sim em Debug, nao em Release. O teste que provoca
	// a duplicata de proposito desliga antes de carregar o modulo duplicado.
	XPLATBASE_API void xplat_instance_set_fatal(int fatal);


	#ifdef XPLATBASE_WIN

	    #define WIN32_LEAN_AND_MEAN
	    #include <windows.h>
	    #include <stdio.h>

	    // Para tratamento de evento. Tentar capturar antes de encerrar
		#include <dbghelp.h>
		#pragma comment(lib, "dbghelp.lib")


        #ifndef XPLATBASE_NO_AUTO_INIT
	    #pragma section(".CRT$XCU", read)
		    __declspec(allocate(".CRT$XCU")) static void (*init_ptr)() = platform_init;
        #endif

	#else 

	   // Para tratamento de evento. Tentar capturar antes de encerrar
       #include <execinfo.h>

	   #include <stdio.h>

		static void __attribute__((constructor)) xplatbase_auto_init(void)
		{
			platform_init();
		}

	#endif

    /* Utilitarios de plataforma usados internamente pela lib */
    #ifdef XPLATBASE_WIN
        static inline int  xcpu_count(void) { SYSTEM_INFO si; GetSystemInfo(&si); return (int)si.dwNumberOfProcessors; }
        static inline void xcpu_pause(void) { YieldProcessor(); }
        static inline void xsleep_ms(int ms) { Sleep((DWORD)(ms < 0 ? 0 : ms)); }
    #else
        #include <unistd.h>
        #include <time.h>
        static inline int  xcpu_count(void) { return (int)sysconf(_SC_NPROCESSORS_ONLN); }
        #if defined(__x86_64__) || defined(__i386__)
            static inline void xcpu_pause(void) { __asm__ __volatile__("pause" ::: "memory"); }
        #elif defined(__aarch64__) || defined(__arm__)
            static inline void xcpu_pause(void) { __asm__ __volatile__("yield" ::: "memory"); }
        #else
            static inline void xcpu_pause(void) { (void)0; }
        #endif
        static inline void xsleep_ms(int ms) { struct timespec ts = { ms/1000, (long)(ms%1000)*1000000L }; nanosleep(&ts, NULL); }
    #endif


	typedef struct
	{
		const char* Func;
		const char* File;
		int         Line;
	} 
	CallContextGlobalEvent;

	typedef void (*ErrorHandler)(const CallContextGlobalEvent* ctx, const char* msg);

    /* Guarda-chuva da API publica: inclui todos os modulos para quem faz apenas
     * #include "xplatbase.h". E' PULADO quando um header de modulo esta no meio
     * da propria inclusao (ele define XPB_SKIP_UMBRELLA em volta do seu
     * #include deste header), evitando o ciclo em que um modulo-irmao e' puxado
     * antes de o tipo do modulo atual (Thread/ListX) estar definido. Cada modulo
     * ja inclui suas dependencias diretas, entao nada se perde. */
    #ifndef XPB_SKIP_UMBRELLA
        #include "../src/list_hander.h"
        #include "../src/memory_pool.h"
        #include "../src/string_handler.h"
        #include "../src/thread_handler.h"
        #include "../src/thread_pool.h"
    #endif


#ifdef __cplusplus
}
#endif

#endif /* XPLATBASE */
