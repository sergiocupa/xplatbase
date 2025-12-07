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


#define xpb_list_add(_this,obj,type) xpb_list_add_internal(_this, instance, sizeof(type), __func__, __FILE__, __LINE__)


void xpb_list_init(ListXPB* list, int32 initial_count, uint64 type_size)
{
    list->Max   = initial_count >= 0 ? initial_count : INITIAL_LIST_COUNT;
    list->Count = 0;
    list->Items = (void**)malloc(list->Max * sizeof(void*));
}

ListXPB* xpb_list_new(int initial_count)
{
    ListXPB* list = malloc(sizeof(ListXPB));
    list->Max   = initial_count >= 0 ? initial_count : INITIAL_LIST_COUNT;
    list->Count = 0;
    list->Items = (void**)malloc(list->Max * sizeof(void*));
    return list;
}

static void xpb_list_add_internal(ListXPB* list, void* instance, uint64 type_size, const char* func, const char* file, int line)
{
    if (type_size != list->TypeSize)
    {
        CallContextGlobalEvent ctx = { func, file, line };
        xpb_event_trigger_error(&ctx, "Item a ser adicionado na lista com tamanho inválido. Informado: %zu | Esperado: %zu", type_size, list->TypeSize);
        return;
    }

    int sz = list->Count + 1;
    if (sz >= list->Max)
    {
        list->Max  *= 2;
        list->Items = (void**)realloc((void**)list->Items, list->Max * sizeof(void*));
    }

    list->Items[list->Count] = instance;
    list->Count++;
}

void xpb_list_remove(ListXPB* list, void* obj)
{
    if (list && list->Active)
    {
        int ix = 0;
        while (ix < list->Count)
        {
            void* am = list->Items[ix];
            if (am == obj)
            {
                // transfere instancias
                while (ix < list->Count - 1)
                {
                    list->Items[ix] = list->Items[ix+1];
                    ix++;
                }
                list->Count--;
                break;
            }
            ix++;
        }
    }
}

void xpb_list_remove_index(ListXPB* list, int index)
{
    if (list && list->Active && index < list->Count)
    {
        int im = index;
        while (im < list->Count - 1)
        {
            list->Items[im] = list->Items[im + 1];
            im++;
        }
        list->Count--;
    }
}


void xpb_list_release(ListXPB** list)
{
    //free((*list)->Items);
    (*list)->Active = false;
    (*list) = 0;
}

