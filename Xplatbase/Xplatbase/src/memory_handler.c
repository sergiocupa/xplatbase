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




static void event_trigger(int code, uint64 size, const char* func, const char* file, int line)
{
	if (code == -1)
	{
		CallContextGlobalEvent ctx = { func, file, line };
		xpb_event_trigger_error(&ctx, "Falha ao alocar o buffer.");
		return NULL;
	}
	if (code == -2)
	{
		CallContextGlobalEvent ctx = { func, file, line };
		xpb_event_trigger_error(&ctx, "Falha ao alocar %zu bytes para o buffer.", size);
		return NULL;
	}
}


int reallocate(BufferXPB* buffer, uint64 new_size)
{
	void* nm = realloc(buffer->Data, new_size);
	if (!nm)
	{
		return -1;
	}
	buffer->Data = nm;
	return 0;
}
int reallocate_pointer(uint64 new_size, void** pointer)
{
	void* nm = realloc(*pointer, new_size);
	if (!nm)
	{
		return -1;
	}
	*pointer = nm;
	return 0;
}

int allocate(uint64 size, BufferXPB** output)
{
	BufferXPB* buffer = malloc(sizeof(BufferXPB));

	if (!buffer)
	{
		return -1;
	}

	buffer->Max      = 0;
	buffer->Size     = size;
	buffer->TypeSize = 1;
	buffer->Data     = malloc(buffer->TypeSize * buffer->Size);

	if (!buffer->Data)
	{
		return -2;
	}

	*output = buffer;
	return 0;
}

int allocate_type(uint64 size, uint64 type_size, BufferXPB** output)
{
	BufferXPB* buffer = malloc(sizeof(BufferXPB));

	if (!buffer)
	{
		return -1;
	}

	buffer->Max      = 0;
	buffer->Size     = size;
	buffer->TypeSize = type_size;
	buffer->Data     = malloc(buffer->TypeSize * buffer->Size);

	if (!buffer->Data)
	{
		return -2;
	}

	*output = buffer;
	return 0;
}

void release(BufferXPB** buffer)
{
	if ((*buffer) && (*buffer)->Active)
	{
		(*buffer)->Active = false;
		//free((*buffer)->Data);
	}
}



BufferXPB* allocate_ext(uint64 size, const char* func, const char* file, int line)
{
	BufferXPB* buffer = 0;
	int rs = allocate(size, &buffer);

	if (rs < 0)
	{
		event_trigger(rs, size, func, file, line);
	}
	return buffer;
}

BufferXPB* reallocate_ext(BufferXPB* buffer, uint64 new_size, const char* func, const char* file, int line)
{
	int rs = reallocate(buffer, new_size);
	if (rs < 0)
	{
		event_trigger(rs, new_size, func, file, line);
	}
	return buffer;
}


BufferXPB* allocate_type_ext(uint64 size, uint64 type_size, const char* func, const char* file, int line)
{
	BufferXPB* buffer = 0;
	int rs = allocate_type(size, type_size, &buffer);
	 
	if(rs < 0)
	{
		event_trigger(rs, size, func, file, line);
	}
	return buffer;
}






