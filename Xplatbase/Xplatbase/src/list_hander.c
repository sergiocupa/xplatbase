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

#include "list_hander.h"
#include "memory_pool.h"

/* Alocacao SEMPRE via memory_pool (memop_*), nunca malloc/realloc do CRT.
 * Apenas a struct ListX e o vetor Items saem do pool; os itens apontados sao
 * externos e nunca sao tocados aqui. Ciente: o pool e resetavel
 * (memop_test_reset/memop_shutdown) -- nao segure uma ListX viva atraves de um
 * reset do pool. */


void list_init(ListX* list, int32 initial_count, uint64 type_size)
{
    list->TypeSize = type_size;
    list->Max      = initial_count >= 0 ? initial_count : INITIAL_LIST_COUNT;
    list->Count    = 0;

    /* Items e o VETOR de ponteiros em si (sem header por item). */
    list->Items = (void**)memop_alloc_raw((uint64)list->Max * sizeof(void*));
    if (!list->Items)
    {
        xpb_event_trigger_error(0, "Falha ao alocar %zu itens para a lista.", list->Max);
    }
}

ListX* list_create(int initial_count, uint64 type_size)
{
    ListX* list = (ListX*)memop_alloc_raw(sizeof(ListX));
    if (!list)
    {
        xpb_event_trigger_error(0, "Falha ao alocar a lista.");
        return NULL;
    }

    list->TypeSize = type_size;
    list->Max      = initial_count >= 0 ? initial_count : INITIAL_LIST_COUNT;
    list->Count    = 0;

    list->Items = (void**)memop_alloc_raw((uint64)list->Max * sizeof(void*));
    if (!list->Items)
    {
        xpb_event_trigger_error(0, "Falha ao alocar %zu itens para a lista.", list->Max);
    }

    return list;
}


boolean list_add(ListX* list, void* instance, uint64 type_size)
{
    if (type_size != list->TypeSize)
    {
        xpb_event_trigger_error(0, "Item a ser adicionado na lista com tamanho inválido. Informado: %zu | Esperado: %zu", type_size, list->TypeSize);
        return false;
    }

    int sz = list->Count + 1;
    if (sz >= list->Max)
    {
        uint64 new_max   = (uint64)list->Max * 2;
        void** new_items = (void**)memop_realloc_raw(list->Items, new_max * sizeof(void*));
        if (!new_items)
        {
            xpb_event_trigger_error(0, "Não foi possível realocar novo tamanho dos itens. Informado: %zu | Esperado: %zu", type_size, list->TypeSize);
            return false;
        }

        list->Items = new_items;
        list->Max   = (uint64)new_max;
    }

    list->Items[list->Count] = instance;
    list->Count++;
    return true;
}


void list_remove(ListX* list, void* obj)
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


void list_remove_index(ListX* list, int index)
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


void list_release(ListX** list)
{
    if (!list || !*list)
    {
        return;
    }

    /* Libera apenas o vetor Items (os itens apontados sao externos). A propria
     * struct ListX NAO e liberada aqui: list_init pode ter sido usado com uma
     * ListX embutida/na pilha, e um memop_free_raw dela corromperia memoria.
     * Listas criadas por list_create (struct no pool) devem ser liberadas pelo
     * dono apos este release. */
    if ((*list)->Items)
    {
        memop_free_raw((*list)->Items);
        (*list)->Items = NULL;
    }
    (*list)->Active = false;
    (*list) = 0;
}

