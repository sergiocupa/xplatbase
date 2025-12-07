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

#include "../include/xplatbase.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>


static ErrorHandler g_error_handler = default_handler;



// Tenta pegar origem do erro antes de encerrar

void print_stacktrace(FILE* f) 
{
#ifdef _WIN32
    void* stack[32];
    HANDLE process = GetCurrentProcess();
    SymInitialize(process, NULL, TRUE);

    WORD frames = CaptureStackBackTrace(0, 32, stack, NULL);

    SYMBOL_INFO* symbol = (SYMBOL_INFO*)calloc(sizeof(SYMBOL_INFO) + 256, 1);
    symbol->MaxNameLen = 255;
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);

    fprintf(f, "Stack trace:\n");
    for (int i = 0; i < frames; i++) {
        SymFromAddr(process, (DWORD64)stack[i], 0, symbol);
        fprintf(f, "  %d: %s (0x%llx)\n", i, symbol->Name, (unsigned long long)symbol->Address);
    }

    free(symbol);
#else
    void* buffer[32];
    int count = backtrace(buffer, 32);
    char** symbols = backtrace_symbols(buffer, count);

    fprintf(f, "Stack trace:\n");
    for (int i = 0; i < count; i++) {
        fprintf(f, "  %d: %s\n", i, symbols[i]);
    }

    free(symbols);
#endif
}

void crash_handler(int sig) 
{
    FILE* f = fopen("crash.log", "a");
    if (f) 
    {
        fprintf(f, "\n=== CRASH DETECTADO ===\n");
        fprintf(f, "Signal: %d (%s)\n", sig, sig == SIGSEGV ? "SIGSEGV" : sig == SIGABRT ? "SIGABRT" : "OUTRO");

        print_stacktrace(f);

        fclose(f);
    }

    _exit(1);
}





static void default_handler(const CallContextGlobalEvent* ctx, const char* msg)
{
	fprintf(stderr, "\n[ERRO] %s\n", msg);
	fprintf(stderr, "  Origem: %s()\n", ctx->Func);
	fprintf(stderr, "  Arquivo: %s:%d\n", ctx->File, ctx->Line);
}


void xpb_event_trigger_error(const CallContextGlobalEvent* ctx, const char* fmt, ...)
{
	char buffer[256];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);

	if (g_error_handler)
	{
		g_error_handler(ctx, buffer);
	}
}


void xpb_set_error_handler(ErrorHandler handler)
{
	g_error_handler = handler ? handler : default_handler;
}



void xpb_event_init(void)
{
    signal(SIGSEGV, crash_handler);
    signal(SIGABRT, crash_handler);
}