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


#ifndef MEMORY_HANDLER_H
#define MEMORY_HANDLER_H

#ifdef __cplusplus
extern "C" {
#endif

    #include "../include/xplatbase.h"


	int allocate(uint64 size, BufferXPB** output);
	int allocate_type(uint64 size, uint64 type_size, BufferXPB** output);
	int reallocate(BufferXPB* buffer, uint64 new_size);
	int reallocate_pointer(uint64 new_size, void** pointer);
	void release(BufferXPB** buffer);


#ifdef __cplusplus
}
#endif

#endif /* MEMORY_HANDLER */