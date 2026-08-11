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


#ifndef LIST_HAND_H
#define LIST_HAND_H

#ifdef __cplusplus
extern "C" {
#endif

    #include "../include/xplatbase.h"
    #include "event_handler.h"


	typedef struct
	{
		boolean Active;
		uint64  TypeSize;
		uint64  Max;
		uint64  Count;
		void** Items;
	}
	ListX;


	XPLATBASE_API void list_init(ListX* list, int32 initial_count, uint64 type_size);
	XPLATBASE_API ListX* list_create(int initial_count, uint64 type_size);
	XPLATBASE_API boolean list_add(ListX* list, void* instance, uint64 type_size);
	XPLATBASE_API void list_remove(ListX* list, void* obj);
	XPLATBASE_API void list_remove_index(ListX* list, int index);
	XPLATBASE_API void list_release(ListX** list);



#ifdef __cplusplus
}
#endif

#endif /* RING_QUEUE */
