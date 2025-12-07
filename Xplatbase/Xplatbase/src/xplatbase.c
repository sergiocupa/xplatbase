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
#include <string.h>



bool platform_initialized = false;



void test_utf8()
{
	const char* utf8_str = "你";
	int leng = strlen(utf8_str);

	if (leng != 3)
	{
		perror("UTF-8 encoding unsupported");
		exit(EXIT_FAILURE);
	}
}


void platform_init()
{
	if (platform_initialized) return;

	test_utf8();
	xpb_event_init();

	platform_initialized = true;
}
