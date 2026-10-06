//  Declaracao de todos os testes (as funcoes vivem nos .c; o registro.cpp as lista).

#ifndef TESTES_H
#define TESTES_H

#include "ctest_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// Pool de tarefas
void teste_pool_vigia_liga_e_desliga(TestResult* r);
void teste_pool_perfil_economia_e_performance(TestResult* r);
void teste_pool_rajadas_intermitentes_nao_giram(TestResult* r);
void teste_pool_bench_antes_depois(TestResult* r);        // benchmark
void teste_pool_bench_bibliotecas(TestResult* r);         // benchmark (bench_pool_bibliotecas.cpp)

// Pool de memoria (test_memory_pool.c, bench_alocadores.c)
void teste_memoria_alloc_free_basico(TestResult* r);
void teste_memoria_classes_de_tamanho(TestResult* r);
void teste_memoria_lane_por_thread(TestResult* r);
void teste_memoria_crescimento_por_spans(TestResult* r);
void teste_memoria_free_remoto_reativa(TestResult* r);
void teste_memoria_vazamento_sem_colecao(TestResult* r);
void teste_memoria_bench_alocadores(TestResult* r);       // benchmark

// Strings (test_string_handler.c)
void teste_string_init_create_release(TestResult* r);
void teste_string_append(TestResult* r);
void teste_string_copy(TestResult* r);
void teste_string_equal_matriz(TestResult* r);
void teste_string_equal_part_matriz(TestResult* r);
void teste_string_indexof_char_matriz(TestResult* r);
void teste_string_indexofs_matriz(TestResult* r);
void teste_string_substring_matriz(TestResult* r);
void teste_string_trim(TestResult* r);
void teste_string_stop(TestResult* r);
void teste_string_split_matriz(TestResult* r);
void teste_string_append_format(TestResult* r);

#ifdef __cplusplus
}
#endif

#endif
