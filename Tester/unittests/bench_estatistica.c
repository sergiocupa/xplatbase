#include "bench_estatistica.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int compara_double(const void* a, const void* b)
{
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

static double mediana_no_lugar(double* t, int n)
{
    qsort(t, (size_t)n, sizeof(double), compara_double);
    return (n & 1) ? t[n / 2] : (t[n / 2 - 1] + t[n / 2]) / 2.0;
}

double est_mediana(const double* v, int n)
{
    double t[EST_MAX_AMOSTRAS];
    if (n <= 0) return 0.0;
    if (n > EST_MAX_AMOSTRAS) n = EST_MAX_AMOSTRAS;
    memcpy(t, v, sizeof(double) * (size_t)n);
    return mediana_no_lugar(t, n);
}

double est_hodges_lehmann(const double* d, int n)
{
    double w[EST_MAX_AMOSTRAS * (EST_MAX_AMOSTRAS + 1) / 2];
    int i, j, k = 0;
    if (n <= 0) return 0.0;
    if (n > EST_MAX_AMOSTRAS) n = EST_MAX_AMOSTRAS;
    for (i = 0; i < n; i++)
        for (j = i; j < n; j++)
            w[k++] = (d[i] + d[j]) / 2.0;
    return mediana_no_lugar(w, k);
}

double est_wilcoxon_p(const double* d, int n)
{
    double a[EST_MAX_AMOSTRAS];
    int    pos[EST_MAX_AMOSTRAS], idx[EST_MAX_AMOSTRAS], r2[EST_MAX_AMOSTRAS];
    // soma maxima dos postos dobrados: 2 * m(m+1)/2
    double cnt[EST_MAX_AMOSTRAS * (EST_MAX_AMOSTRAS + 1) + 1];
    int m = 0, i, j, s, total = 0, wmais = 0;
    double menor_ou_igual = 0, maior_ou_igual = 0, casos, p;

    if (n > EST_MAX_AMOSTRAS) n = EST_MAX_AMOSTRAS;
    for (i = 0; i < n; i++)
    {
        if (fabs(d[i]) < 1e-12) continue;
        a[m] = fabs(d[i]);
        pos[m] = d[i] > 0;
        m++;
    }
    if (m == 0) return 1.0;

    // ordena os indices por |d| (m e pequeno: insercao basta)
    for (i = 0; i < m; i++) idx[i] = i;
    for (i = 1; i < m; i++)
    {
        int x = idx[i];
        for (j = i - 1; j >= 0 && a[idx[j]] > a[x]; j--) idx[j + 1] = idx[j];
        idx[j + 1] = x;
    }
    // postos DOBRADOS: empate recebe a media dos postos, que e multipla de 0,5
    for (i = 0; i < m; )
    {
        int t;
        j = i;
        while (j + 1 < m && a[idx[j + 1]] == a[idx[i]]) j++;
        for (t = i; t <= j; t++) r2[idx[t]] = i + j + 2;   // 2 * ((i+1)+(j+1))/2
        i = j + 1;
    }
    for (i = 0; i < m; i++) { total += r2[i]; if (pos[i]) wmais += r2[i]; }

    // distribuicao exata de W+ sob H0: cada posto entra com sinal + ou - (1/2 cada)
    memset(cnt, 0, sizeof(double) * (size_t)(total + 1));
    cnt[0] = 1.0;
    for (i = 0; i < m; i++)
        for (s = total; s >= r2[i]; s--)
            cnt[s] += cnt[s - r2[i]];

    for (s = 0; s <= total; s++)
    {
        if (s <= wmais) menor_ou_igual += cnt[s];
        if (s >= wmais) maior_ou_igual += cnt[s];
    }
    casos = ldexp(1.0, m);
    p = 2.0 * (menor_ou_igual < maior_ou_igual ? menor_ou_igual : maior_ou_igual) / casos;
    return p > 1.0 ? 1.0 : p;
}

static const double* g_p_ordena;
static int compara_p(const void* a, const void* b)
{
    double x = g_p_ordena[*(const int*)a], y = g_p_ordena[*(const int*)b];
    return x < y ? -1 : x > y ? 1 : 0;
}

void est_benjamini_hochberg(const double* p, int n, double q, int* sig)
{
    int* ord;
    int k, kmax = -1;
    for (k = 0; k < n; k++) sig[k] = 0;
    if (n <= 0) return;
    ord = (int*)malloc(sizeof(int) * (size_t)n);
    if (!ord) return;
    for (k = 0; k < n; k++) ord[k] = k;
    g_p_ordena = p;
    qsort(ord, (size_t)n, sizeof(int), compara_p);
    for (k = 0; k < n; k++)
        if (p[ord[k]] <= (double)(k + 1) * q / (double)n) kmax = k;
    for (k = 0; k <= kmax; k++) sig[ord[k]] = 1;
    free(ord);
}

int est_rng_int(EstRng* r, int n)
{
    uint64_t x = r->S;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    r->S = x;
    return (int)((x * 2685821657736338717ull >> 33) % (uint64_t)n);
}
