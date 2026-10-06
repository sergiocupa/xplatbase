//  Pool de tarefas: ANTES x DEPOIS x AJUSTE, no mesmo processo.
//
//  Tres versoes do pool (pool_versoes.h): ANTES (o thread_pool.c de antes da ultima
//  alteracao commitada), DEPOIS (o src atual) e AJUSTE (o src atual com 1 vigia ligado).
//  Comparacoes, cada uma com o seu calculo e o seu veredito: depois x antes, ajuste x antes
//  e ajuste x depois.
//
//  Cenarios: todos os do Tester/thread_pool/thread_pool_bench.cpp (carga progressiva
//  baixa -> ultra, saturacao, rajadas, tarefas longas misturadas, alocacao dentro da tarefa,
//  spawn em arvore), com latencia por tarefa (p50/p90/p99/p999/max10/max), mais os de pool
//  parado (CPU ociosa, despertar, fork-join apos ocioso). Ver bench_pool.h.
//
//  Cada cenario roda R vezes em cada versao; a ordem das versoes GIRA a cada rodada (cada
//  versao passa o mesmo numero de vezes em cada posicao), e as medidas de uma mesma rodada
//  formam o PAR (ou trio) comparado, medido nas mesmas condicoes da maquina.
//
//  Calculo, por comparacao: cada linha (cenario x metrica) vira R razoes pareadas. Por
//  linha: efeito de Hodges-Lehmann, p do Wilcoxon pareado exato, Benjamini-Hochberg por
//  familia (grupo carga/parado x metrica). Uma linha so e PIOR/MELHOR se a diferenca for
//  significativa E passar da margem da metrica. Desempenho: wall, p50. Estabilidade: p90,
//  p99, p999, max10, max -- todos decidem. No geral: media geometrica das razoes, ponderada
//  pela precisao de cada linha, com IC 95% por bootstrap das rodadas. Veredito
//  (nao-inferioridade): manteve o patamar / nao manteve / piorou / melhorou / inconclusivo.
//  O RESUMO com os vereditos sai no TOPO do relatorio (o painel do Gerenciador corta saida
//  longa), e o relatorio inteiro vai tambem para pool_antes_depois.txt ao lado da DLL.
//
//  So mede em Release: em Debug (sem otimizacao, heap de Debug) o teste e PULADO -- ja deu
//  "piora" de vazao que em Release nao existe. A solution compila este projeto em Release
//  tambem na configuracao Debug.
//
//  O teste falha so se alguma versao TRAVAR, e e PULADO se a versao ANTES nao existir: o
//  veredito de desempenho fica no relatorio (a maquina tambem varia).
//
//  Variaveis: XPB_BENCH_REPS (rodadas, padrao 15), XPB_BENCH_FILTRO (so cenarios cujo nome
//  contem o texto), XPB_BENCH_TSV_ENTRADA (nao mede: refaz o relatorio e o calculo sobre as
//  rodadas de um TSV ja gravado), XPB_BENCH_DEBUG=1 (mede mesmo em Debug), XPB_BENCH_PERFIL=
//  economia (ANTES e DEPOIS no perfil economia: compara o perfil antes x depois). Rodadas brutas:
//  pool_antes_depois.tsv ao lado da DLL.

#include "ctest_core.h"
#include "testes.h"
#include "pool_versoes.h"
#include "bench_pool.h"
#include "bench_estatistica.h"
#include "memory_pool.h"
#include "thread_handler.h"

#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define MAX_REPS        31
// 15: com o Wilcoxon exato o menor p possivel e 2/2^R. Com 11 (p >= 0,00098) nenhuma linha
// isolada passa na correcao de ~28 linhas por metrica com folga; com 15 (p >= 0,00006), passa.
#define REPS_PADRAO     15
#define N_BOOTSTRAP     2000
#define FDR_Q           0.05
#define MARGEM_GERAL    0.05      // a media geral precisa ficar dentro de +-5% para "mesmo patamar"
// Piso da variancia de d no peso da linha: (5%)^2. Nenhuma linha pesa mais do que uma com 5%
// de ruido entre rodadas -- sem teto, uma linha de ruido quase nulo dominaria a media sozinha.
#define PISO_VARIANCIA  (0.05 * 0.05)
#define N_FLAT_EXTERNO  1000000   // o N_flat padrao do thread_pool_bench
#define N_ARVORE        1000000

static const CenarioPool CENARIOS[] = {
    // pool parado: o que o bench antigo nao via
    { "cpu-ocioso",                  CEN_CPU_OCIOSO,      0 },
    { "despertar-apos-ocioso",       CEN_DESPERTAR,       0 },
    { "fork-join-apos-ocioso-cpu",   CEN_FJ_OCIOSO_CPU,   64 },
    { "fork-join-apos-ocioso-tempo", CEN_FJ_OCIOSO_TEMPO, 64 },
    { "fork-join-em-regime",         CEN_FJ_REGIME,       64 },
    // carga: os cenarios do thread_pool_bench, mesmos parametros
    { "flat-externo",                CEN_FLAT,  N_FLAT_EXTERNO },
    { "spawn-arvore",                CEN_SPAWN, N_ARVORE },
    { "rapida/baixa",                CEN_FLAT,   20000,  0,  0,  0,    0, 0, 0 },
    { "rapida/media",                CEN_FLAT,   60000,  0,  0,  0,    0, 0, 0 },
    { "rapida/alta",                 CEN_FLAT,  120000,  0,  0,  0,    0, 0, 0 },
    { "rapida/ultra",                CEN_FLAT,  180000,  0,  0,  0,    0, 0, 0 },
    { "media/baixa",                 CEN_FLAT,   20000, 15, 25,  0,    0, 0, 0 },
    { "media/media",                 CEN_FLAT,   60000, 15, 35,  0,    0, 0, 0 },
    { "media/alta",                  CEN_FLAT,  120000, 15, 45,  0,    0, 0, 0 },
    { "media/ultra",                 CEN_FLAT,  180000, 15, 55,  0,    0, 0, 0 },
    { "mista/baixa",                 CEN_FLAT,   20000,  0,  5, 16,  250, 0, 0 },
    { "mista/media",                 CEN_FLAT,   60000,  0,  5, 16,  300, 0, 0 },
    { "mista/alta",                  CEN_FLAT,  120000,  0,  5, 16,  350, 0, 0 },
    { "mista/ultra",                 CEN_FLAT,  180000,  0,  5, 16,  400, 0, 0 },
    { "satur/alta",                  CEN_FLAT,  160000,  0,  0,  0,    0, 0, 0 },
    { "satur/ultra",                 CEN_FLAT,  240000,  0,  0,  0,    0, 0, 0 },
    { "rajada-curta/media",          CEN_FLAT,   60000,  0,  0,  0,    0, 0, 0 },
    { "rajada-mista/media",          CEN_FLAT,   60000,  0,  5, 12,  300, 0, 0 },
    { "mista-massiva/alta",          CEN_FLAT,  180000,  0,  8, 24,  500, 0, 0 },
    { "mista-massiva/ultra",         CEN_FLAT,  240000,  0,  8, 24,  600, 0, 0 },
    { "malloc-default/media",        CEN_FLAT,   60000,  0,  0,  0,    0, 8, 0 },
    { "malloc-default/alta",         CEN_FLAT,  120000,  0,  0,  0,    0, 8, 0 },
    { "mempool/media",               CEN_FLAT,   60000,  0,  0,  0,    0, 8, 1 },
    { "mempool/alta",                CEN_FLAT,  120000,  0,  0,  0,    0, 8, 1 },
    { "lento-misto",                 CEN_FLAT,    4000,  0,  0,  2, 8000, 0, 0 },
};
#define N_CEN ((int)(sizeof(CENARIOS) / sizeof(CENARIOS[0])))

static const char*  MET_TITULO[N_METRICAS] = { "wall ms", "p50 us", "p90 us", "p99 us", "p999 us", "max10 us", "max us", "nucleos" };
static const char*  MET_CURTO[N_METRICAS]  = { "wall", "p50", "p90", "p99", "p999", "max10", "max", "nucleos" };
// Margem de "mesmo patamar" por metrica: a cauda varia mais por natureza, entao aceita mais.
// p90, p99 e max sao os sinais de ESTABILIDADE (cauda e pior caso) e decidem o veredito.
static const double MET_MARGEM[N_METRICAS] = { 0.05, 0.10, 0.10, 0.15, 0.20, 0.20, 0.25, 0.10 };
// Piso somado a antes e depois na razao: evita razao explosiva perto de zero. Latencia: 1 us
// (10 tiques do QPC de 10 MHz) -- p50 de 0,4 us contra 0,7 us e resolucao do relogio, nao
// diferenca de pool, e sem o piso virava "razao 1,75". CPU ociosa: 0,01 nucleo.
static const double MET_PISO[N_METRICAS]   = { 0.01, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 0.01 };

// Papel de cada linha (cenario x metrica) no calculo final.
enum { PAPEL_INFO = 0, PAPEL_DECIDE = 1, PAPEL_OBJETIVO = 2 };

// Grupo do cenario: pool sob carga ou pool parado. Sao perguntas diferentes e cada um tem a
// sua familia na correcao de multiplas comparacoes -- misturados, as 4 linhas de pool parado
// ficavam diluidas entre as 28 de carga e um efeito consistente (pior em 12 de 15 rodadas)
// nao aparecia.
enum { GRUPO_CARGA = 0, GRUPO_PARADO = 1 };
static int grupo_de(const CenarioPool* c) { return (c->Tipo == CEN_FLAT || c->Tipo == CEN_SPAWN) ? GRUPO_CARGA : GRUPO_PARADO; }

// Indices dos resumos no calculo final: 0..N_METRICAS-1 por metrica, depois geral e grupos.
#define G_GERAL   (N_METRICAS)
#define G_CARGA   (N_METRICAS + 1)
#define G_PARADO  (N_METRICAS + 2)
#define N_GRUPOS  (N_METRICAS + 3)

static int papel(const CenarioPool* c, int met)
{
    if (c->Tipo == CEN_CPU_OCIOSO) return PAPEL_OBJETIVO;         // o alvo da alteracao
    if (met == MET_NUCLEOS) return PAPEL_INFO;                    // CPU sob carga: consequencia, nao meta
    return PAPEL_DECIDE;   // inclusive o max: o pior caso de cada rodada e o sinal de instabilidade
}

typedef struct
{
    int    Cen, Met, Papel;
    double D[MAX_REPS];          // ln((versao + piso) / (base + piso)) por rodada
    double Efeito;               // Hodges-Lehmann de D
    double Peso;                 // precisao: rodadas / variancia de D (com piso)
    double P;                    // Wilcoxon pareado, bilateral
    int    Sig;                  // sobreviveu ao Benjamini-Hochberg
    int    Abaixo;               // antes e depois abaixo da resolucao: fora do calculo
    int    Classe;               // -1 melhor, 0 mesmo patamar, +1 pior
}
Linha;

// ---- utilidades -------------------------------------------------------------------------

static int le_env_int(const char* nome, int padrao)
{
    char* v = 0;
    size_t n = 0;
    int r = padrao;
    if (_dupenv_s(&v, &n, nome) == 0 && v) { if (v[0]) r = atoi(v); free(v); }
    return r;
}

static int cenario_pedido(const char* nome)
{
    char* v = 0;
    size_t n = 0;
    int ok = 1;
    if (_dupenv_s(&v, &n, "XPB_BENCH_FILTRO") == 0 && v)
    {
        if (v[0]) ok = strstr(nome, v) != 0;
        free(v);
    }
    return ok;
}

static FILE* abre_ao_lado(const char* arquivo, char* caminho, size_t tam)
{
    HMODULE eu = 0;
    char* barra;
    // Ao lado da DLL de teste: o diretorio de trabalho do executor nao e previsivel.
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)(void*)&abre_ao_lado, &eu);
    if (!GetModuleFileNameA(eu, caminho, (DWORD)tam)) return 0;
    barra = strrchr(caminho, '\\');
    if (!barra) return 0;
    snprintf(barra + 1, tam - (size_t)(barra + 1 - caminho), "%s", arquivo);
    return fopen(caminho, "w");
}

// Relatorio: vai para a saida do teste (t_logf) e, inteiro, para pool_antes_depois.txt --
// o painel do Gerenciador corta saida longa.
static FILE* g_rel;

static void rel(const char* fmt, ...)
{
    char linha[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(linha, sizeof(linha), fmt, ap);
    va_end(ap);
    t_logf("%s", linha);
    if (g_rel) fputs(linha, g_rel);
}

static int compara_double(const void* a, const void* b)
{
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

static void nome_com_tamanho(const CenarioPool* c, char* out, size_t tam)
{
    if (c->Tipo == CEN_FLAT || c->Tipo == CEN_SPAWN)
    {
        if (c->Tarefas >= 1000000) snprintf(out, tam, "%s (%d mi)", c->Nome, c->Tarefas / 1000000);
        else                       snprintf(out, tam, "%s (%d mil)", c->Nome, c->Tarefas / 1000);
    }
    else snprintf(out, tam, "%s", c->Nome);
}

// ---- versoes e medidas ------------------------------------------------------------------

#define N_VERSOES 3
enum { V_ANTES, V_DEPOIS, V_AJUSTE };
static const char* NOME_VERSAO[N_VERSOES] = { "antes", "depois", "ajuste" };
static int g_presente[N_VERSOES];

// Medidas: [cenario][versao][rodada]
#define MED(c, v, k) g_med[((c) * N_VERSOES + (v)) * MAX_REPS + (k)]
static Medida* g_med;

static double mediana_versao(int c, int v, int met, int reps)
{
    double t[MAX_REPS];
    int k;
    for (k = 0; k < reps; k++) t[k] = MED(c, v, k).V[met];
    return est_mediana(t, reps);
}

// Reanalise: le as rodadas de um TSV gravado por este teste, em vez de medir de novo.
// Devolve o numero de rodadas (0 = arquivo ilegivel). Aceita TSV de 2 ou 3 versoes.
static int carrega_tsv(const char* caminho, int* ativo)
{
    char linha[512];
    int maxk = 0, c, m, v;
    FILE* f = fopen(caminho, "r");
    if (!f) return 0;
    for (c = 0; c < N_CEN; c++) ativo[c] = 0;
    for (v = 0; v < N_VERSOES; v++) g_presente[v] = 0;
    if (!fgets(linha, sizeof(linha), f)) { fclose(f); return 0; }   // cabecalho
    while (fgets(linha, sizeof(linha), f))
    {
        char* ctx = 0;
        char* cen = strtok_s(linha, "\t", &ctx);
        char* met = strtok_s(0, "\t", &ctx);
        char* ver = strtok_s(0, "\t", &ctx);
        char* rod = strtok_s(0, "\t", &ctx);
        char* val = strtok_s(0, "\t\r\n", &ctx);
        int k;
        if (!val) continue;
        for (c = 0; c < N_CEN && strcmp(CENARIOS[c].Nome, cen) != 0; c++) {}
        for (m = 0; m < N_METRICAS && strcmp(MET_CURTO[m], met) != 0; m++) {}
        for (v = 0; v < N_VERSOES && strcmp(NOME_VERSAO[v], ver) != 0; v++) {}
        k = atoi(rod) - 1;
        if (c == N_CEN || m == N_METRICAS || v == N_VERSOES || k < 0 || k >= MAX_REPS) continue;
        MED(c, v, k).V[m] = atof(val);
        ativo[c] = 1;
        g_presente[v] = 1;
        if (k + 1 > maxk) maxk = k + 1;
    }
    fclose(f);
    return maxk;
}

// ---- calculo por comparacao -------------------------------------------------------------

typedef struct { double Estimativa, Ic0, Ic1; int N, Melhor, Igual, Pior; } Grupo;

typedef struct
{
    int         A, B;             // compara B contra A: razao = B / A
    const char* Titulo;           // "depois x antes"
    const char* Curto;            // rotulo da linha de razao na tabela
    int         Ativa;            // as duas versoes existem
    Linha*      Lin;
    int         NLin;
    Grupo       Grp[N_GRUPOS];
    int         Piores, Melhores, AvisoPoder;
    char        Veredito[64], Motivo[256];
}
Comparacao;

static Comparacao COMP[] = {
    { V_ANTES,  V_DEPOIS, "depois x antes",  "dep/ant" },
    { V_ANTES,  V_AJUSTE, "ajuste x antes",  "aju/ant" },
    { V_DEPOIS, V_AJUSTE, "ajuste x depois", "aju/dep" },
};
#define N_COMP ((int)(sizeof(COMP) / sizeof(COMP[0])))

static void resume_grupo(Grupo* g, const double* boot, int b)
{
    double* t = (double*)malloc(sizeof(double) * (size_t)b);
    if (!t) return;
    memcpy(t, boot, sizeof(double) * (size_t)b);
    qsort(t, (size_t)b, sizeof(double), compara_double);
    g->Ic0 = exp(t[(int)(0.025 * b)]);
    g->Ic1 = exp(t[(int)(0.975 * b) < b ? (int)(0.975 * b) : b - 1]);
    free(t);
}

// Bootstrap das rodadas: reamostra os PARES de cada cenario (os mesmos indices para todas
// as metricas do cenario, que sao correlacionadas) e recalcula a media geometrica PONDERADA
// das razoes de cada resumo (pesos fixos, da amostra original): por metrica, geral, carga e
// pool parado.
static void bootstrap(const Linha* lin, int nlin, const int* ativo, int reps, double* boot /* [N_GRUPOS][B] */)
{
    EstRng rng = { 0x9E3779B97F4A7C15ull };
    double tmp[MAX_REPS];
    int idx[MAX_REPS];
    int b, c, k, i, g;

    for (b = 0; b < N_BOOTSTRAP; b++)
    {
        double soma[N_GRUPOS] = { 0 };
        double n[N_GRUPOS] = { 0 };           // soma dos pesos
        for (c = 0; c < N_CEN; c++)
        {
            int gc = grupo_de(&CENARIOS[c]) == GRUPO_CARGA ? G_CARGA : G_PARADO;
            if (!ativo[c]) continue;
            for (k = 0; k < reps; k++) idx[k] = est_rng_int(&rng, reps);
            for (i = 0; i < nlin; i++)
            {
                double h, w;
                if (lin[i].Cen != c || lin[i].Papel != PAPEL_DECIDE) continue;
                for (k = 0; k < reps; k++) tmp[k] = lin[i].D[idx[k]];
                h = est_hodges_lehmann(tmp, reps);
                w = lin[i].Peso;
                soma[lin[i].Met] += w * h; n[lin[i].Met] += w;
                soma[G_GERAL]    += w * h; n[G_GERAL]    += w;
                soma[gc]         += w * h; n[gc]         += w;
            }
        }
        for (g = 0; g < N_GRUPOS; g++)
            boot[g * N_BOOTSTRAP + b] = n[g] > 0 ? soma[g] / n[g] : 0.0;
    }
}

static int linha_no_resumo(const Linha* l, int g)
{
    if (l->Papel != PAPEL_DECIDE) return 0;
    if (g < N_METRICAS) return l->Met == g;
    if (g == G_GERAL)   return 1;
    return grupo_de(&CENARIOS[l->Cen]) == (g == G_CARGA ? GRUPO_CARGA : GRUPO_PARADO);
}

// Veredito (nao-inferioridade): pior em alguma linha que decide -> nao manteve; senao, o IC
// do geral decide se descarta piora maior que MARGEM_GERAL.
static void veredito_de(Comparacao* cp)
{
    const Grupo* g = &cp->Grp[G_GERAL];
    double lim_sup = 1.0 + MARGEM_GERAL, lim_inf = 1.0 / (1.0 + MARGEM_GERAL);
    char* ve = cp->Veredito;
    char* mo = cp->Motivo;
    size_t tv = sizeof(cp->Veredito), tm = sizeof(cp->Motivo);

    if (!g->N)
    { snprintf(ve, tv, "SEM DADOS"); snprintf(mo, tm, "nenhum cenario que decide foi medido (filtro?)"); }
    else if (cp->Piores > 0)
    { snprintf(ve, tv, "NAO MANTEVE O PATAMAR"); snprintf(mo, tm, "%d linha(s) pioraram de forma significativa e alem da margem", cp->Piores); }
    else if (g->Ic0 > lim_sup)
    { snprintf(ve, tv, "PIOROU NO GERAL"); snprintf(mo, tm, "IC 95%% do geral [%.3f, %.3f] inteiro acima de +%.0f%%", g->Ic0, g->Ic1, MARGEM_GERAL * 100); }
    else if (g->Ic1 < lim_inf)
    { snprintf(ve, tv, "MELHOROU NO GERAL"); snprintf(mo, tm, "IC 95%% do geral [%.3f, %.3f] inteiro abaixo de -%.0f%% e nenhuma linha pior", g->Ic0, g->Ic1, MARGEM_GERAL * 100); }
    else if (g->Ic1 <= lim_sup)
    {
        // o limite de CIMA do IC descarta piora maior que a margem; o de baixo abaixo de
        // -margem so diz que PODE ter melhorado
        snprintf(ve, tv, "MANTEVE O PATAMAR");
        snprintf(mo, tm, "IC 95%% do geral [%.3f, %.3f]: piora acima de +%.0f%% descartada, e nenhuma linha pior%s",
                 g->Ic0, g->Ic1, MARGEM_GERAL * 100, cp->Melhores ? "; com melhoras pontuais" : "");
    }
    else
    {
        snprintf(ve, tv, "INCONCLUSIVO");
        snprintf(mo, tm, "IC 95%% do geral [%.3f, %.3f] nao descarta piora acima de +%.0f%%: ruido alto, aumente XPB_BENCH_REPS",
                 g->Ic0, g->Ic1, MARGEM_GERAL * 100);
    }
}

static int analisa(Comparacao* cp, const int* ativo, int reps, double* boot)
{
    int aplica[N_METRICAS];
    int c, m, k, i, fam;
    double* p;
    int* sig;
    int* qual;

    cp->NLin = 0;
    cp->Piores = cp->Melhores = cp->AvisoPoder = 0;

    // ---- estatistica por linha ----
    for (c = 0; c < N_CEN; c++)
    {
        if (!ativo[c]) continue;
        bench_pool_metricas(CENARIOS[c].Tipo, aplica);
        for (m = 0; m < N_METRICAS; m++)
        {
            Linha* l;
            if (!aplica[m]) continue;
            l = &cp->Lin[cp->NLin++];
            memset(l, 0, sizeof(*l));
            l->Cen = c; l->Met = m; l->Papel = papel(&CENARIOS[c], m);
            // Latencia menor que a resolucao nos DOIS lados (p50 de 0,4 us): nao ha o que
            // comparar. Fica na tabela, marcada, e sai do calculo.
            if (m >= MET_P50 && m <= MET_MAX &&
                mediana_versao(c, cp->A, m, reps) < MET_PISO[m] && mediana_versao(c, cp->B, m, reps) < MET_PISO[m])
            { l->Abaixo = 1; l->Papel = PAPEL_INFO; }
            for (k = 0; k < reps; k++)
                l->D[k] = log((MED(c, cp->B, k).V[m] + MET_PISO[m]) / (MED(c, cp->A, k).V[m] + MET_PISO[m]));
            l->Efeito = est_hodges_lehmann(l->D, reps);
            l->P      = est_wilcoxon_p(l->D, reps);
            {
                double mu = 0, var = 0;
                for (k = 0; k < reps; k++) mu += l->D[k];
                mu /= reps;
                for (k = 0; k < reps; k++) var += (l->D[k] - mu) * (l->D[k] - mu);
                var /= (reps - 1);
                if (var < PISO_VARIANCIA) var = PISO_VARIANCIA;
                l->Peso = reps / var;
            }
        }
    }

    // ---- Benjamini-Hochberg por FAMILIA ----
    // Grupo (carga / pool parado) x metrica para as linhas que decidem, o objetivo sozinho, e
    // as informativas por metrica. Numa familia unica de ~150 linhas o limiar de uma linha
    // isolada fica abaixo do menor p que o Wilcoxon consegue dar com as rodadas usuais --
    // nada seria detectado, nem uma queda de 2,6 para 0,1 nucleo (aconteceu na primeira
    // versao deste calculo).
    p    = (double*)malloc(sizeof(double) * (size_t)cp->NLin);
    sig  = (int*)malloc(sizeof(int) * (size_t)cp->NLin);
    qual = (int*)malloc(sizeof(int) * (size_t)cp->NLin);
    if (!p || !sig || !qual) { free(p); free(sig); free(qual); return 0; }
    // familias: [0, 2*N_METRICAS) decide (grupo*N_METRICAS + metrica), 2*N_METRICAS objetivo,
    // (2*N_METRICAS, 3*N_METRICAS] informativas por metrica
    for (fam = 0; fam < 3 * N_METRICAS + 1; fam++)
    {
        int n = 0;
        for (i = 0; i < cp->NLin; i++)
        {
            const Linha* l = &cp->Lin[i];
            int f = l->Papel == PAPEL_DECIDE   ? grupo_de(&CENARIOS[l->Cen]) * N_METRICAS + l->Met
                  : l->Papel == PAPEL_OBJETIVO ? 2 * N_METRICAS
                  :                              2 * N_METRICAS + 1 + l->Met;
            if (f == fam && !l->Abaixo) { qual[n] = i; p[n] = l->P; n++; }
        }
        if (!n) continue;
        est_benjamini_hochberg(p, n, FDR_Q, sig);
        for (k = 0; k < n; k++) cp->Lin[qual[k]].Sig = sig[k];
        // poder: com R rodadas o menor p possivel e 2/2^R; se nem ele passa, nada passa
        if (fam < 2 * N_METRICAS && 2.0 / ldexp(1.0, reps) > FDR_Q / n) cp->AvisoPoder = 1;
    }
    free(p); free(sig); free(qual);

    for (i = 0; i < cp->NLin; i++)
    {
        Linha* l = &cp->Lin[i];
        double razao = exp(l->Efeito), mg = MET_MARGEM[l->Met];
        l->Classe = !l->Sig ? 0 : razao > 1.0 + mg ? 1 : razao < 1.0 / (1.0 + mg) ? -1 : 0;
        if (l->Papel == PAPEL_DECIDE && l->Classe) { if (l->Classe > 0) cp->Piores++; else cp->Melhores++; }
    }

    // ---- resumos (por metrica, geral, carga, pool parado) ----
    bootstrap(cp->Lin, cp->NLin, ativo, reps, boot);
    for (m = 0; m < N_GRUPOS; m++)
    {
        Grupo* g = &cp->Grp[m];
        double soma = 0, pesos = 0;
        memset(g, 0, sizeof(*g));
        for (i = 0; i < cp->NLin; i++)
        {
            const Linha* l = &cp->Lin[i];
            if (!linha_no_resumo(l, m)) continue;
            soma  += l->Peso * l->Efeito;
            pesos += l->Peso;
            g->N++;
            if (l->Classe < 0) g->Melhor++; else if (l->Classe > 0) g->Pior++; else g->Igual++;
        }
        g->Estimativa = pesos > 0 ? exp(soma / pesos) : 1.0;
        if (g->N) resume_grupo(g, boot + m * N_BOOTSTRAP, N_BOOTSTRAP);
    }
    veredito_de(cp);
    return 1;
}

// ---- relatorio --------------------------------------------------------------------------

static const Linha* linha_de(const Comparacao* cp, int c, int m)
{
    int i;
    for (i = 0; i < cp->NLin; i++) if (cp->Lin[i].Cen == c && cp->Lin[i].Met == m) return &cp->Lin[i];
    return 0;
}

static void imprime_objetivo(const Comparacao* cp, int reps)
{
    int i;
    for (i = 0; i < cp->NLin; i++)
    {
        const Linha* l = &cp->Lin[i];
        if (l->Papel != PAPEL_OBJETIVO) continue;
        rel("    CPU com o pool parado: %s %.3f -> %s %.3f nucleos (razao %.3f, p=%.4f)  %s\n",
            NOME_VERSAO[cp->A], mediana_versao(l->Cen, cp->A, l->Met, reps),
            NOME_VERSAO[cp->B], mediana_versao(l->Cen, cp->B, l->Met, reps),
            exp(l->Efeito), l->P,
            l->Classe < 0 ? "MELHOROU" : l->Classe > 0 ? "PIOROU" : "mesmo patamar");
    }
}

static void imprime_diferencas(const Comparacao* cp, int reps)
{
    int i, alguma = 0;
    rel("    Linhas com diferenca real (significativa e alem da margem):\n");
    for (i = 0; i < cp->NLin; i++)
    {
        const Linha* l = &cp->Lin[i];
        if (!l->Classe || l->Papel == PAPEL_OBJETIVO) continue;
        rel("      %-6s %-30s %-8s %10.2f -> %10.2f  razao %.2f  p=%.4f%s\n",
            l->Classe > 0 ? "PIOR" : "MELHOR", CENARIOS[l->Cen].Nome, MET_CURTO[l->Met],
            mediana_versao(l->Cen, cp->A, l->Met, reps), mediana_versao(l->Cen, cp->B, l->Met, reps),
            exp(l->Efeito), l->P, l->Papel == PAPEL_INFO ? "  (informativo)" : "");
        alguma = 1;
    }
    if (!alguma) rel("      nenhuma\n");
}

static void imprime_resumos(const Comparacao* cp)
{
    static const char* NOME_RESUMO[] = { "GERAL", "carga", "pool parado" };
    int m;
    rel("    %-11s %5s %12s %22s %8s %7s %6s %5s\n", "resumo", "linhas", "razao geo", "IC 95%", "margem", "melhor", "igual", "pior");
    rel("    -----------------------------------------------------------------------------------\n");
    for (m = 0; m < N_GRUPOS; m++)
    {
        const Grupo* g = &cp->Grp[m];
        if (!g->N) continue;
        if (m == G_GERAL) rel("    -----------------------------------------------------------------------------------\n");
        rel("    %-11s %5d %12.3f        [%6.3f , %6.3f] %7.0f%% %7d %6d %5d\n",
            m < N_METRICAS ? MET_CURTO[m] : NOME_RESUMO[m - N_METRICAS], g->N, g->Estimativa, g->Ic0, g->Ic1,
            (m < N_METRICAS ? MET_MARGEM[m] : MARGEM_GERAL) * 100, g->Melhor, g->Igual, g->Pior);
    }
}

// ---- teste ------------------------------------------------------------------------------

void teste_pool_bench_antes_depois(TestResult* r)
{
    static int iniciado = 0;
    const PoolVersao* versao[N_VERSOES] = { &POOL_ANTES, &POOL_DEPOIS, &POOL_AJUSTE };
    int reps = le_env_int("XPB_BENCH_REPS", REPS_PADRAO);
    int ativo[N_CEN], aplica[N_METRICAS];
    int c, k, v, m, i, workers = 0, core = 0, secao = -1, tsv_ok = 0, ok_analise = 1;
    char tsv_caminho[MAX_PATH], rel_caminho[MAX_PATH], entrada[MAX_PATH], nome[64];
    FILE* tsv;
    double* boot;

    tsv_caminho[0] = entrada[0] = '\0';
    t_start(r);
    {
        // XPB_BENCH_TSV_ENTRADA: refaz so o calculo sobre rodadas ja medidas (um TSV deste teste)
        char* e = 0;
        size_t n = 0;
        if (_dupenv_s(&e, &n, "XPB_BENCH_TSV_ENTRADA") == 0 && e) { snprintf(entrada, sizeof(entrada), "%s", e); free(e); }
    }
    if (reps < 3) reps = 3;
    if (reps > MAX_REPS) reps = MAX_REPS;

#ifdef _DEBUG
    // Medir em Debug da numero que nao existe em producao: sem otimizacao, sem inline, heap
    // de Debug (malloc ~30x mais lento). Ja aconteceu: 6-10% de "piora" de vazao que em
    // Release nao existe. A solution compila este projeto em Release tambem na configuracao
    // Debug; isto so pega quem compilar o .vcxproj em Debug direto. Reanalise de TSV pode.
    if (!entrada[0] && le_env_int("XPB_BENCH_DEBUG", 0) != 1)
        T_SKIP(r, "build Debug nao mede desempenho real (sem otimizacao, heap de Debug). Compile em Release -- "
                  "a solution ja compila este projeto em Release tambem na configuracao Debug. XPB_BENCH_DEBUG=1 forca.");
#endif

    g_med = (Medida*)calloc((size_t)N_CEN * N_VERSOES * MAX_REPS, sizeof(Medida));
    boot  = (double*)malloc(sizeof(double) * N_GRUPOS * N_BOOTSTRAP);
    for (i = 0; i < N_COMP; i++) COMP[i].Lin = (Linha*)calloc((size_t)N_CEN * N_METRICAS, sizeof(Linha));
    T_ASSERT(r, g_med && boot && COMP[0].Lin && COMP[1].Lin && COMP[2].Lin, "sem memoria para o bench");

    if (entrada[0])
    {
        reps = carrega_tsv(entrada, ativo);
        T_ASSERT(r, reps >= 3, "reanalise: nao consegui ler rodadas suficientes de %s", entrada);
        for (c = 0; c < N_CEN; c++) if (ativo[c] && !cenario_pedido(CENARIOS[c].Nome)) ativo[c] = 0;
    }
    else
    {
        if (!pool_antes_disponivel())
            T_SKIP(r, "versao ANTES indisponivel: %s", pool_antes_descricao());
        g_presente[V_ANTES] = g_presente[V_DEPOIS] = 1;
        g_presente[V_AJUSTE] = pool_ajuste_disponivel();

        if (!iniciado)
        {
            // Mesma ligacao da producao: cada worker criado ganha a sua lane no memory_pool.
            memop_init();
            thread_init(memop_on_created_thread, memop_on_ended_thread);
            bench_pool_prepara(N_FLAT_EXTERNO, N_ARVORE);
            iniciado = 1;
        }
        {
            ThreadPool* p = POOL_DEPOIS.Criar(0);
            POOL_DEPOIS.Dims(p, &workers, &core);
            POOL_DEPOIS.Destruir(p);
        }
        for (c = 0; c < N_CEN; c++) ativo[c] = cenario_pedido(CENARIOS[c].Nome);

        // ---- medicao ----
        tsv = abre_ao_lado("pool_antes_depois.tsv", tsv_caminho, sizeof(tsv_caminho));
        tsv_ok = tsv != 0;
        if (tsv) fprintf(tsv, "cenario\tmetrica\tversao\trodada\tvalor\n");
        for (c = 0; c < N_CEN; c++)
        {
            const CenarioPool* cn = &CENARIOS[c];
            if (!ativo[c]) continue;
            bench_pool_metricas(cn->Tipo, aplica);
            for (k = 0; k < reps; k++)
            {
                // A ordem GIRA a cada rodada: cada versao passa o mesmo numero de vezes em
                // cada posicao, e quem roda primeiro (maquina mais fria ou mais quente) varia.
                int pos;
                for (pos = 0; pos < N_VERSOES; pos++)
                {
                    v = (pos + k) % N_VERSOES;
                    if (!g_presente[v]) continue;
                    if (!bench_pool_mede(cn, versao[v], &MED(c, v, k)))
                    {
                        if (tsv) fclose(tsv);
                        T_ASSERT(r, 0, "pool TRAVOU no cenario %s, versao %s (tarefa nao terminou em 60 s)", cn->Nome, NOME_VERSAO[v]);
                    }
                }
                if (tsv)
                    for (v = 0; v < N_VERSOES; v++)
                        if (g_presente[v])
                            for (m = 0; m < N_METRICAS; m++)
                                if (aplica[m])
                                    fprintf(tsv, "%s\t%s\t%s\t%d\t%.6f\n", cn->Nome, MET_CURTO[m], NOME_VERSAO[v], k + 1, MED(c, v, k).V[m]);
            }
            printf("  ... %s\n", cn->Nome);
        }
        if (tsv) fclose(tsv);
    }

    // ---- calculo de cada comparacao ----
    for (i = 0; i < N_COMP; i++)
    {
        COMP[i].Ativa = g_presente[COMP[i].A] && g_presente[COMP[i].B];
        if (COMP[i].Ativa) ok_analise &= analisa(&COMP[i], ativo, reps, boot);
    }
    T_ASSERT(r, ok_analise, "sem memoria para o calculo");

    // ---- relatorio: cabecalho e RESUMO (no topo: e o que sobrevive ao corte do painel) ----
    g_rel = abre_ao_lado("pool_antes_depois.txt", rel_caminho, sizeof(rel_caminho));
    rel("\n=== Pool de tarefas: ANTES x DEPOIS x AJUSTE ===\n");
    if (entrada[0])
        rel("  REANALISE das rodadas de: %s (versoes descritas no relatorio original) | %d rodadas\n", entrada, reps);
    else
    {
        rel("  antes : %s\n", pool_antes_descricao());
        rel("  depois: %s\n", pool_depois_descricao());
        rel("  ajuste: %s\n", pool_ajuste_descricao());
        if (pool_bench_perfil_economia())
            rel("  perfil: ECONOMIA no depois%s (XPB_BENCH_PERFIL=economia)\n",
                pool_antes_tem_perfil() ? " e no antes" : "; o ANTES nao tem perfil e roda no padrao");
        rel("  pool: %d workers (%d core) | %d rodadas por cenario, ordem girando | TSC %.2f GHz\n",
            workers, core, reps, bench_tsc_hz() / 1e9);
    }
#ifdef _DEBUG
    rel("  ATENCAO: build Debug (sem otimizacao, heap de Debug). Os numeros nao representam desempenho.\n");
#endif
    if (g_rel) rel("  Relatorio completo (o painel do Gerenciador corta saida longa): %s\n", rel_caminho);

    rel("\n  RESUMO\n");
    rel("  %-16s %-22s %-24s %-24s\n", "comparacao", "veredito", "carga (IC 95%)", "pool parado (IC 95%)");
    for (i = 0; i < N_COMP; i++)
    {
        const Comparacao* cp = &COMP[i];
        char carga[40], parado[40];
        if (!cp->Ativa) continue;
        snprintf(carga,  sizeof(carga),  "%.3f [%.3f, %.3f]", cp->Grp[G_CARGA].Estimativa,  cp->Grp[G_CARGA].Ic0,  cp->Grp[G_CARGA].Ic1);
        snprintf(parado, sizeof(parado), "%.3f [%.3f, %.3f]", cp->Grp[G_PARADO].Estimativa, cp->Grp[G_PARADO].Ic0, cp->Grp[G_PARADO].Ic1);
        rel("  %-16s %-22s %-24s %-24s\n", cp->Titulo, cp->Veredito, cp->Grp[G_CARGA].N ? carga : "-", cp->Grp[G_PARADO].N ? parado : "-");
    }
    for (i = 0; i < N_COMP; i++)
    {
        const Comparacao* cp = &COMP[i];
        if (!cp->Ativa) continue;
        rel("\n  %s: %s\n  (%s)\n", cp->Titulo, cp->Veredito, cp->Motivo);
        imprime_objetivo(cp, reps);
        imprime_diferencas(cp, reps);
        if (cp->AvisoPoder)
            rel("    ATENCAO: com %d rodadas nenhuma linha ISOLADA pode ser declarada pior ou melhor; use XPB_BENCH_REPS=15 ou mais.\n", reps);
    }

    // ---- tabela por cenario ----
    rel("\n  Valores = mediana das rodadas. Latencia = submit -> inicio da tarefa. Menor e melhor em tudo.\n");
    rel("  razao (Hodges-Lehmann das rodadas pareadas):  '=' mesmo patamar   '+' melhor   '!' pior\n");
    rel("  '~' = as duas versoes abaixo da resolucao do relogio (< 1 us): sem comparacao possivel, fora do calculo\n");
    rel("  (so e '+'/'!' o que e estatisticamente significativo E passa da margem da metrica)\n");
    {
        char linha[512];
        int n = snprintf(linha, sizeof(linha), "\n  %-30s %-7s", "cenario", "");
        for (m = 0; m < N_METRICAS; m++) n += snprintf(linha + n, sizeof(linha) - (size_t)n, " %10s", MET_TITULO[m]);
        rel("%s\n", linha);
        n = snprintf(linha, sizeof(linha), "  %-30s %-7s", "", "");
        for (m = 0; m < N_METRICAS; m++) n += snprintf(linha + n, sizeof(linha) - (size_t)n, " %10s", m == MET_NUCLEOS ? "(info)" : "");
        rel("%s\n", linha);
    }
    for (c = 0; c < N_CEN; c++)
    {
        char linha[512];
        int n, primeira = 1;
        if (!ativo[c]) continue;
        {
            int carga = CENARIOS[c].Tipo == CEN_FLAT || CENARIOS[c].Tipo == CEN_SPAWN;
            if (carga != secao)
            {
                rel(carga ? "  -- carga (cenarios do thread_pool_bench) ---------------------------------------------------------------------------------\n"
                          : "  -- pool parado --------------------------------------------------------------------------------------------------------------\n");
                secao = carga;
            }
        }
        bench_pool_metricas(CENARIOS[c].Tipo, aplica);
        nome_com_tamanho(&CENARIOS[c], nome, sizeof(nome));
        for (v = 0; v < N_VERSOES; v++)
        {
            if (!g_presente[v]) continue;
            n = snprintf(linha, sizeof(linha), "  %-30s %-7s", primeira ? nome : "", NOME_VERSAO[v]);
            primeira = 0;
            for (m = 0; m < N_METRICAS; m++)
            {
                if (!aplica[m]) { n += snprintf(linha + n, sizeof(linha) - (size_t)n, " %10s", "-"); continue; }
                n += snprintf(linha + n, sizeof(linha) - (size_t)n, m == MET_NUCLEOS ? " %10.3f" : " %10.2f",
                              mediana_versao(c, v, m, reps));
            }
            rel("%s\n", linha);
        }
        for (i = 0; i < N_COMP; i++)
        {
            const Comparacao* cp = &COMP[i];
            if (!cp->Ativa) continue;
            n = snprintf(linha, sizeof(linha), "  %-30s %-7s", "", cp->Curto);
            for (m = 0; m < N_METRICAS; m++)
            {
                const Linha* l = linha_de(cp, c, m);
                if (!l) { n += snprintf(linha + n, sizeof(linha) - (size_t)n, " %10s", ""); continue; }
                n += snprintf(linha + n, sizeof(linha) - (size_t)n, " %8.2f %c", exp(l->Efeito),
                              l->Abaixo ? '~' : l->Classe > 0 ? '!' : l->Classe < 0 ? '+' : '=');
            }
            rel("%s\n", linha);
        }
    }

    // ---- calculo final, detalhado ----
    rel("\n=== CALCULO FINAL ===\n");
    rel("  Por comparacao, cada linha = um cenario x uma metrica, com %d rodadas pareadas.\n", reps);
    rel("  d = ln(versao/base) em cada rodada; razao = e^(Hodges-Lehmann de d); p = Wilcoxon pareado exato;\n");
    rel("  Benjamini-Hochberg (FDR %.0f%%) por familia: grupo (carga / pool parado) x metrica.\n", FDR_Q * 100);
    rel("  Pior/melhor = significativo E alem da margem da metrica.\n");
    rel("  Decidem: wall, p50, p90, p99, p999, max10 e max (p90, p99 e max = estabilidade). Nucleos sob carga e informativo.\n");
    rel("  Geral = media geometrica das razoes das linhas que decidem, PONDERADA pela precisao de cada linha\n");
    rel("  (peso = rodadas / variancia de d; linha instavel pesa menos, e nenhuma pesa mais do que uma com 5%% de ruido).\n");
    rel("  IC 95%% por bootstrap das rodadas (%d reamostras).\n", N_BOOTSTRAP);
    for (i = 0; i < N_COMP; i++)
    {
        const Comparacao* cp = &COMP[i];
        if (!cp->Ativa) continue;
        rel("\n  --- %s ---\n", cp->Titulo);
        imprime_resumos(cp);
        imprime_objetivo(cp, reps);
        rel("    VEREDITO: %s\n    (%s)\n", cp->Veredito, cp->Motivo);
    }
    if (tsv_ok) rel("\n  Rodadas brutas: %s\n", tsv_caminho);

    if (g_rel) { fclose(g_rel); g_rel = 0; }
    for (i = 0; i < N_COMP; i++) { free(COMP[i].Lin); COMP[i].Lin = 0; }
    free(boot);
    free(g_med);
    g_med = 0;
}
