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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* NAO usar memop_alloc_raw/memop_free_raw aqui, de proposito.
 *
 * list_hander.c e infraestrutura de vida do PROCESSO (ex.: a lista `Threads`
 * de thread_handler.c e global, nunca resetada). memory_pool.c e projetado
 * pra ser RESETAVEL (memop_shutdown()/memop_test_reset() dao VirtualFree em
 * todos os segmentos do pool). Se o Items de uma ListXPB de vida longa fosse
 * alocado do pool, um memop_shutdown() no meio do programa (comum em testes/
 * benchmarks que trocam de alocador) libera a memoria por baixo do ponteiro
 * -- Threads.Items fica dangling, e o proximo thread_create() escreve em
 * memoria ja devolvida ao SO (confirmado: SEGV reproduzido no bench.c ao
 * criar a primeira thread real depois de um memop_shutdown() anterior).
 * malloc/realloc puro nao tem esse acoplamento de ciclo de vida. */



void xpb_list_init_ext(ListXPB* list, int32 initial_count, uint64 type_size, const char* func, const char* file, int line)
{
    list->TypeSize = type_size;
    list->Max      = initial_count >= 0 ? initial_count : INITIAL_LIST_COUNT;
    list->Count    = 0;

    /* Items e o VETOR de ponteiros em si (sem header por item). */
    list->Items = (void**)malloc((size_t)list->Max * sizeof(void*));
    if (!list->Items)
    {
        CallContextGlobalEvent ctx = { func, file, line };
        xpb_event_trigger_error(&ctx, "Falha ao alocar %zu itens para a lista.", list->Max);
    }
}

ListXPB* xpb_list_new_ext(int initial_count, uint64 type_size, const char* func, const char* file, int line)
{
    ListXPB* list = (ListXPB*)malloc(sizeof(ListXPB));
    if (!list)
    {
        CallContextGlobalEvent ctx = { func, file, line };
        xpb_event_trigger_error(&ctx, "Falha ao alocar a lista.");
        return NULL;
    }

    list->TypeSize = type_size;
    list->Max      = initial_count >= 0 ? initial_count : INITIAL_LIST_COUNT;
    list->Count    = 0;

    list->Items = (void**)malloc((size_t)list->Max * sizeof(void*));
    if (!list->Items)
    {
        CallContextGlobalEvent ctx = { func, file, line };
        xpb_event_trigger_error(&ctx, "Falha ao alocar %zu itens para a lista.", list->Max);
    }

    return list;
}


void xpb_list_add_ext(ListXPB* list, void* instance, uint64 type_size, const char* func, const char* file, int line)
{
    if (type_size != list->TypeSize)
    {
        CallContextGlobalEvent ctx = { func, file, line };
        xpb_event_trigger_error(&ctx, "Item a ser adicionado na lista com tamanho inv�lido. Informado: %zu | Esperado: %zu", type_size, list->TypeSize);
        return;
    }

    int sz = list->Count + 1;
    if (sz >= list->Max)
    {
        uint64 new_max   = (uint64)list->Max * 2;
        void** new_items = (void**)realloc(list->Items, (size_t)(new_max * sizeof(void*)));
        if (!new_items)
        {
            CallContextGlobalEvent ctx = { func, file, line };
            xpb_event_trigger_error(&ctx, "N�o foi poss�vel realocar novo tamanho dos itens. Informado: %zu | Esperado: %zu", type_size, list->TypeSize);
            return;
        }

        list->Items = new_items;
        list->Max   = (uint64)new_max;
    }

    list->Items[list->Count] = instance;
    list->Count++;
}


void xpb_list_remove_ext(ListXPB* list, void* obj)
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

