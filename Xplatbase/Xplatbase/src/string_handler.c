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
//#include "memory_handler.h"
#include "event_handler.h"


static const char BASE64_TABLE[]     = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const int  BASE64_MOD_TABLE[] = { 0, 2, 1 };



// Implementacoes basicas para restar alocação e append basico


void string_init_ext(StringX* ni, const char* func, const char* file, int line)
{
	ni->Length = 0;
	ni->Max    = INITIAL_STRING_LENGTH;

	int rs = allocate((ni->Max + 1) * sizeof(char),&ni->Content);
	if (rs < 0)
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

	int rs = allocate((ni->Max + 1) * sizeof(char), &ni->Content);
	if (rs < 0)
	{
		CallContextGlobalEvent ctx = { func, file, line };
		xpb_event_trigger_error(&ctx, "Falha ao alocar %zu caracteres para a string.", ni->Max + 1);
		return;
	}
	ni->Content[0] = 0;
}


StringX* string_new(const char* func, const char* file, int line)
{
	StringX* str;
	int rz = allocate_type(sizeof(StringX), sizeof(StringX), &str);
	if (rz < 0)
	{
		CallContextGlobalEvent ctx = { func, file, line };
		xpb_event_trigger_error(&ctx, "Falha ao alocar string.");
		return;
	}

	str->Length = 0;
	str->Max    = INITIAL_STRING_LENGTH;

	rz = allocate(((str->Max + 1) * sizeof(char)), &str->Content);
	if (rz < 0)
	{
		CallContextGlobalEvent ctx = { func, file, line };
		xpb_event_trigger_error(&ctx, "Falha ao alocar %zu caracteres para a string.", str->Max + 1);
		return NULL;
	}
	return str;
}


StringX* string_new_leng(int initial_length, const char* func, const char* file, int line)
{
	StringX* str;
	int rz = allocate_type(sizeof(StringX), sizeof(StringX), &str);
	if (rz < 0)
	{
		CallContextGlobalEvent ctx = { func, file, line };
		xpb_event_trigger_error(&ctx, "Falha ao alocar string.");
		return;
	}

	str->Length = 0;
	str->Max    = initial_length >= 0 ? initial_length : INITIAL_STRING_LENGTH;

	rz = allocate(((str->Max + 1) * sizeof(char)), &str->Content);
	if (rz < 0)
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
			_this->Max = ((_this->Length + leng + 1) + _this->Max) * 2;

			int rz = reallocate_pointer((_this->Max * sizeof(char)), &_this->Content);
			if (rz < 0)
			{
				CallContextGlobalEvent ctx = { func, file, line };
				xpb_event_trigger_error(&ctx, "Falha ao realocar %zu caracteres para a string.", _this->Max + 1);
				return;
			}
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


