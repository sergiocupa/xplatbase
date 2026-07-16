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

#include "thread_pool.h"
#include "thread_handler.h"
#include "memory_pool.h"
#include "mem_leak_watch.h"
#include "../include/xplatbase.h"
#include "event_handler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>



boolean platform_initialized = false;



void test_utf8()
{
	/* bytes UTF-8 explicitos (U+20AC euro): nao depende da codificacao com que
	 * o arquivo foi salvo nem do charset de execucao do compilador */
	const char* utf8_str = "\xE2\x82\xAC";
	int leng = strlen(utf8_str);

	if (leng != 3)
	{
		perror("UTF-8 encoding unsupported");
		exit(EXIT_FAILURE);
	}
}


void platform_init()
{
	if (platform_initialized) return;

	test_utf8();
	xpb_event_init();

	memop_init();

	// parametro para o memory_pool monitorar cria��o de threads para gerenciar suas lanes internas
	thread_init(memop_on_created_thread, memop_on_ended_thread);

	pool_create();

	mem_leak_watch_start(NULL);

	platform_initialized = true;
}
