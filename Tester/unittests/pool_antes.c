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
// Simbolos publicos que existem so em algumas revisoes: renomear e inofensivo quando nao
// existem, e evita simbolo duplicado quando o ANTES e uma revisao que ja os tem (dd04d29+).
#define pool_vigias              antes_pool_vigias
#define pool_vigias_relative     antes_pool_vigias_relative
#define pool_giro_max_us         antes_pool_giro_max_us
#define pool_giro_max_us_relative antes_pool_giro_max_us_relative
#define pool_perfil              antes_pool_perfil
#define pool_perfil_relative     antes_pool_perfil_relative
#define pool_perfil_atual        antes_pool_perfil_atual
#define pool_perfil_atual_relative antes_pool_perfil_atual_relative

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

// Revisoes com perfil (489ea12+) definem POOL_ECONOMIA_GIRO_US no thread_pool.c.
#ifdef POOL_ECONOMIA_GIRO_US
int pool_antes_tem_perfil(void) { return 1; }
static ThreadPool* antes_cria(int cores)
{
    ThreadPool* p = antes_pool_create_relative(cores);
    if (p && pool_bench_perfil_economia()) antes_pool_perfil_relative(p, POOL_PERFIL_ECONOMIA);
    return p;
}
#else
int pool_antes_tem_perfil(void) { return 0; }
static ThreadPool* antes_cria(int cores) { return antes_pool_create_relative(cores); }
#endif

const PoolVersao POOL_ANTES = {
    "antes",
    antes_cria,
    antes_pool_destroy_relative,
    antes_pool_submit_relative,
    antes_pool_dims_relative,
};

int pool_antes_disponivel(void) { return 1; }

#else

#include "pool_versoes.h"

const PoolVersao POOL_ANTES = { "antes", 0, 0, 0, 0 };

int pool_antes_disponivel(void) { return 0; }
int pool_antes_tem_perfil(void) { return 0; }

#endif

const char* pool_antes_descricao(void)  { return POOL_ANTES_DESC; }
const char* pool_depois_descricao(void) { return POOL_DEPOIS_DESC; }
