//  Executor dos testes portaveis do pool no Linux (x64, ARM64 emulado, ThreadSanitizer).
//  No Windows os mesmos testes rodam pelo Gerenciador de Testes (unittests/registro.cpp).
//
//  uso: executa_pool [repeticoes]      saida 0 = todos passaram

#include "ctest_core.h"
#include "testes.h"

#include <stdio.h>
#include <stdlib.h>

typedef struct { const char* nome; void (*fn)(TestResult*); } Caso;

static const Caso CASOS[] =
{
    { "PoolDeTarefas.ContadoresSobMonitor", teste_pool_contadores_sob_monitor },
};

int main(int argc, char** argv)
{
    int rep = argc > 1 ? atoi(argv[1]) : 1, falhas = 0, total = 0;
    if (rep < 1) rep = 1;

    for (size_t i = 0; i < sizeof(CASOS) / sizeof(CASOS[0]); i++)
    {
        for (int k = 0; k < rep; k++)
        {
            TestResult r;
            printf("[ RODA ] %s (%d/%d)\n", CASOS[i].nome, k + 1, rep);
            fflush(stdout);
            CASOS[i].fn(&r);
            total++;
            if (!r.ok) { falhas++; printf("[FALHOU] %s: %s\n", CASOS[i].nome, r.msg); }
            else if (r.skipped) printf("[PULADO] %s: %s\n", CASOS[i].nome, r.msg);
            else printf("[  OK  ] %s\n", CASOS[i].nome);
            fflush(stdout);
        }
    }
    printf("%d de %d execucoes passaram\n", total - falhas, total);
    return falhas ? 1 : 0;
}
