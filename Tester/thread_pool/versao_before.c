/* Coluna BEFORE do thread_pool_bench: o thread_pool.c de antes da ultima alteracao
 * commitada (Tester/unittests/referencia/thread_pool_antes.c, o mesmo do bench antes x
 * depois), compilado ao lado do atual com os simbolos publicos renomeados (before_*).
 * Todo o resto do thread_pool.c e static: cada versao tem o proprio estado. */
#define pool_create_relative     before_pool_create_relative
#define pool_destroy_relative    before_pool_destroy_relative
#define pool_submit_relative     before_pool_submit_relative
#define pool_wait_idle_relative  before_pool_wait_idle_relative
#define pool_dims_relative       before_pool_dims_relative
#define pool_create              before_pool_create
#define pool_destroy             before_pool_destroy
#define pool_submit              before_pool_submit
#define pool_wait_idle           before_pool_wait_idle
#define pool_dims                before_pool_dims

#include "../unittests/referencia/thread_pool_antes.c"
