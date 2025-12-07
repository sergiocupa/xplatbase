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
#include "event_handler.h"
#include <stdio.h>
#include <stdlib.h>



// Criar thread para monitorar todos os buffer, listas e strings marcados como active == false, para desalocar...



#define xpb_allocate(size) xpb_allocate_internal(size, __func__, __FILE__, __LINE__)
#define xpb_allocate_type(size,type) xpb_allocate_type_internal(size,sizeof(type), __func__, __FILE__, __LINE__)



static BufferXPB* xpb_allocate_type_internal(uint64 size, uint64 type_size, const char* func, const char* file, int line)
{
	BufferXPB* buffer = malloc(sizeof(BufferXPB));

	if (!buffer)
	{
		CallContextGlobalEvent ctx = { func, file, line };
		xpb_event_trigger_error(&ctx, "Falha ao alocar buffer", size);
		return NULL;
	}

	buffer->Max      = 0;
	buffer->Size     = size;
	buffer->TypeSize = type_size;
	buffer->Data     = malloc(buffer->TypeSize * buffer->Size);

	if (!buffer->Data)
	{
		CallContextGlobalEvent ctx = { func, file, line };
		xpb_event_trigger_error(&ctx, "Falha ao alocar %zu bytes para o buffer", size);
		return NULL;
	}
	return buffer;
}

static BufferXPB* xpb_allocate_internal(uint64 size, const char* func, const char* file, int line)
{
	BufferXPB* buffer = malloc(sizeof(BufferXPB));

	if (!buffer)
	{
		CallContextGlobalEvent ctx = { func, file, line };
		xpb_event_trigger_error(&ctx, "Falha ao alocar buffer", size);
		return NULL;
	}

	buffer->Max      = 0;
	buffer->Size     = size;
	buffer->TypeSize = 1;
	buffer->Data     = malloc(buffer->TypeSize * buffer->Size);

	if (!buffer->Data)
	{
		CallContextGlobalEvent ctx = { func, file, line };
		xpb_event_trigger_error(&ctx, "Falha ao alocar %zu bytes para o buffer", size);
		return NULL;
	}
	return buffer;
}


static void xpb_release_internal(BufferXPB** buffer)
{
	if ((*buffer) && (*buffer)->Active)
	{
		(*buffer)->Active = false;
		//free((*buffer)->Data);
	}
}






