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

#include "../include/xplatbase.h"
#include "event_handler.h"
#include <stdlib.h>
#include <string.h>

/* NAO usar memop_alloc_raw/memop_free_raw aqui -- mesmo motivo de
 * list_hander.c: string_handler.c e utilitario generico que pode viver mais
 * que qualquer ciclo de memop_shutdown()/memop_test_reset() (o pool e
 * resetavel de proposito; StringX de vida longa nao pode depender disso).
 * Ver comentario completo em list_hander.c. */


static const char BASE64_TABLE[]     = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const int  BASE64_MOD_TABLE[] = { 0, 2, 1 };



// Implementacoes basicas para restar aloca��o e append basico


void string_init_ext(StringX* ni, const char* func, const char* file, int line)
{
	ni->Length = 0;
	ni->Max    = INITIAL_STRING_LENGTH;

	ni->Content = (char*)malloc((ni->Max + 1) * sizeof(char));
	if (!ni->Content)
	{
		CallContextGlobalEvent ctx = { func, file, line };
		xpb_event_trigger_error(&ctx, "Falha ao alocar %zu caracteres para a string.", ni->Max+1);
		return;
	}
	ni->Content[0] = 0;
}


void string_init_leng_ext(StringX* ni, int initial_length, const char* func, const char* file, int line)
{
	ni->Length = 0;
	ni->Max    = initial_length >= 0 ? initial_length : INITIAL_STRING_LENGTH;

	ni->Content = (char*)malloc((ni->Max + 1) * sizeof(char));
	if (!ni->Content)
	{
		CallContextGlobalEvent ctx = { func, file, line };
		xpb_event_trigger_error(&ctx, "Falha ao alocar %zu caracteres para a string.", ni->Max + 1);
		return;
	}
	ni->Content[0] = 0;
}


StringX* string_new(const char* func, const char* file, int line)
{
	StringX* str = (StringX*)malloc(sizeof(StringX));
	if (!str)
	{
		CallContextGlobalEvent ctx = { func, file, line };
		xpb_event_trigger_error(&ctx, "Falha ao alocar string.");
		return NULL;
	}

	str->Length = 0;
	str->Max    = INITIAL_STRING_LENGTH;

	str->Content = (char*)malloc((str->Max + 1) * sizeof(char));
	if (!str->Content)
	{
		CallContextGlobalEvent ctx = { func, file, line };
		xpb_event_trigger_error(&ctx, "Falha ao alocar %zu caracteres para a string.", str->Max + 1);
		return NULL;
	}
	return str;
}


StringX* string_new_leng(int initial_length, const char* func, const char* file, int line)
{
	StringX* str = (StringX*)malloc(sizeof(StringX));
	if (!str)
	{
		CallContextGlobalEvent ctx = { func, file, line };
		xpb_event_trigger_error(&ctx, "Falha ao alocar string.");
		return NULL;
	}

	str->Length = 0;
	str->Max    = initial_length >= 0 ? initial_length : INITIAL_STRING_LENGTH;

	str->Content = (char*)malloc((str->Max + 1) * sizeof(char));
	if (!str->Content)
	{
		CallContextGlobalEvent ctx = { func, file, line };
		xpb_event_trigger_error(&ctx, "Falha ao alocar %zu caracteres para a string.",str->Max + 1);
		return NULL;
	}
	return str;
}




void string_append(StringX* _this, const char* content, const char* func, const char* file, int line)
{
	if (_this)
	{
		int leng = strlen(content);

		if ((_this->Length + leng + 1) >= _this->Max)
		{
			uint64 new_max     = ((_this->Length + leng + 1) + _this->Max) * 2;
			char*  new_content = (char*)realloc(_this->Content, (size_t)(new_max * sizeof(char)));
			if (!new_content)
			{
				CallContextGlobalEvent ctx = { func, file, line };
				xpb_event_trigger_error(&ctx, "Falha ao realocar %zu caracteres para a string.", new_max);
				return;
			}

			_this->Content = new_content;
			_this->Max     = new_max;
		}

		int end = _this->Length + leng;
		int ix = _this->Length;
		while (ix < end)
		{
			_this->Content[_this->Length] = content[ix];
			_this->Length++;
			ix++;
		}
		_this->Content[_this->Length] = 0;
	}
}


