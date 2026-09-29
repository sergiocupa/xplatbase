//  Estatistica para comparar duas versoes com rodadas PAREADAS (antes e depois medidos na
//  mesma rodada). Sem dependencia externa; tudo pensado para poucas amostras (5..31).

#ifndef BENCH_ESTATISTICA_H
#define BENCH_ESTATISTICA_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EST_MAX_AMOSTRAS 64

double est_mediana(const double* v, int n);

// Estimador de Hodges-Lehmann: mediana das medias de Walsh (d_i + d_j)/2, i <= j.
// E o efeito que acompanha o teste de Wilcoxon: robusto a rodada ruim, sem supor normal.
double est_hodges_lehmann(const double* d, int n);

// Wilcoxon pareado (postos sinalizados), p bilateral EXATO -- inclusive com empates, pela
// distribuicao dos postos dobrados. Diferencas nulas sao descartadas, como manda o teste.
double est_wilcoxon_p(const double* d, int n);

// Benjamini-Hochberg: marca em sig[] quais p-valores sobrevivem a taxa de falsas
// descobertas q. Com dezenas de linhas testadas, alfa de 5% em cada uma daria varios
// "pior" por puro acaso; o BH controla isso para o conjunto.
void est_benjamini_hochberg(const double* p, int n, double q, int* sig);

// Gerador para o bootstrap (xorshift64*): reprodutivel, sem estado global.
typedef struct { uint64_t S; } EstRng;
int est_rng_int(EstRng* r, int n);     // 0..n-1

#ifdef __cplusplus
}
#endif

#endif
