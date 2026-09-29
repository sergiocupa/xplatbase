//  Versao ANTES do thread_pool.c, compilada no mesmo executavel que a atual.
//
//  O arquivo de verdade (thread_pool_antes_src.c) e gerado na compilacao pelo alvo
//  GeraPoolAntes do .vcxproj: "git show <PoolAntesRef>:Xplatbase/Xplatbase/src/thread_pool.c".
//  Aqui so se renomeiam os simbolos PUBLICOS antes de inclui-lo, para nao colidirem com os
//  da versao atual. Todo o resto do thread_pool.c e static (inclusive o pool global e o
//  worker corrente), entao cada versao fica com o proprio estado.
//
//  Os #define precisam vir ANTES de qualquer include que puxe o thread_pool.h (o
//  xplatbase.h puxa): e no header que as declaracoes publicas ficam com o nome novo.

#include "pool_antes_info.h"     // gerado: POOL_ANTES_DESC, POOL_DEPOIS_DESC, POOL_ANTES_INDISPONIVEL

#ifndef POOL_ANTES_INDISPONIVEL

#define pool_create_relative     antes_pool_create_relative
#define pool_destroy_relative    antes_pool_destroy_relative
#define pool_submit_relative     antes_pool_submit_relative
#define pool_wait_idle_relative  antes_pool_wait_idle_relative
#define pool_dims_relative       antes_pool_dims_relative
#define pool_create              antes_pool_create
#define pool_destroy             antes_pool_destroy
#define pool_submit              antes_pool_submit
#define pool_wait_idle           antes_pool_wait_idle
#define pool_dims                antes_pool_dims

#include "thread_pool_antes_src.c"

#undef pool_create_relative
#undef pool_destroy_relative
#undef pool_submit_relative
#undef pool_wait_idle_relative
#undef pool_dims_relative
#undef pool_create
#undef pool_destroy
#undef pool_submit
#undef pool_wait_idle
#undef pool_dims

#include "pool_versoes.h"

const PoolVersao POOL_ANTES = {
    "antes",
    antes_pool_create_relative,
    antes_pool_destroy_relative,
    antes_pool_submit_relative,
    antes_pool_dims_relative,
};

int pool_antes_disponivel(void) { return 1; }

#else

#include "pool_versoes.h"

const PoolVersao POOL_ANTES = { "antes", 0, 0, 0, 0 };

int pool_antes_disponivel(void) { return 0; }

#endif

const char* pool_antes_descricao(void)  { return POOL_ANTES_DESC; }
const char* pool_depois_descricao(void) { return POOL_DEPOIS_DESC; }
