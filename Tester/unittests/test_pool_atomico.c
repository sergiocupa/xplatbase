//  Pool de tarefas: contadores que o monitor le enquanto os workers escrevem.
//
//  Cada worker escreve so os proprios contadores (in_task: profundidade; start_count: tarefas
//  iniciadas) e o monitor os le a cada POOL_MON_MS para julgar travamento. Um escritor e um
//  leitor em threads diferentes: em C11 isso e corrida de dados (comportamento indefinido)
//  quando o acesso nao e atomico, mesmo que em x86/x64 e ARM64 o load/store alinhado nao
//  rasgue -- o compilador pode juntar, repetir ou adiar o acesso comum.
//
//  O que PROVA a atomicidade e o ThreadSanitizer (Linux: Tester/linux/roda_pool_atomico.sh
//  tsan): ele aponta a corrida se algum desses acessos for comum, e o executor sai com erro.
//  No Windows (sem TSan) e no Linux x64/ARM64 o caso confere o funcional com o monitor lendo
//  sem parar: tarefas curtas em rajada, tarefas longas que prendem todos os workers com fila
//  atras (o monitor acha "travados" e acorda os elasticos) e tarefas que submetem tarefas
//  (submit reentrante: deque do worker com roubo pelos outros e, com o deque cheio, tarefa
//  aninhada). Toda tarefa roda uma vez e o pool volta a ficar ocioso.
//
//  Portavel (Windows, Linux x64, Linux ARM64): so a API do pool e o atomics.h.

#include "ctest_core.h"
#include "testes.h"
#include "thread_pool.h"
#include "atomics.h"

#ifdef _WIN32
  #include <windows.h>
#else
  #include <time.h>
#endif

#define CURTAS_MS          300   // fase 1: rajadas de tarefas curtas por este tempo
#define LOTE               256   // tarefas por rajada
#define LONGA_US         12000   // fase 2: mais que 2 passos do monitor (POOL_MON_MS = 5)
#define RODADAS_LONGAS       4
#define FILA_ATRAS         512   // curtas na fila enquanto as longas prendem os workers
#define PAIS                64   // fase 3: tarefas que submetem outras
#define FILHOS_POR_PAI      16
#define FILHOS_DO_PAI_GRANDE 20000 // fase 4: bem mais que o deque do worker (POOL_DEQUE_CAP =
                                   // 4096); o excedente roda aninhado no pool_run (in_task = 2)
#define ESPERA_PRESOS_US   1000000 // fase 4: tempo maximo para os outros workers ficarem presos
#define PARADA_US            50000 // fase 4: ...ou a contagem de presos parada por este tempo
#define TENTATIVAS_CHEIO         5 // fase 4: elasticos acordados pelo monitor tambem roubam; se
                                   // esvaziarem o deque (visto no qemu), tenta de novo

#ifdef _MSC_VER
  #define POR_THREAD __declspec(thread)
#else
  #define POR_THREAD __thread
#endif

static POR_THREAD int g_dentro_do_pai;   // 1 enquanto a tarefa_pai_grande desta thread submete

static long long agora_us(void)
{
#ifdef _WIN32
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (long long)(t.QuadPart * 1000000 / f.QuadPart);
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000000 + t.tv_nsec / 1000;
#endif
}

typedef struct
{
    ThreadPool*  pool;
    xatomic_int  feitas;       // tarefas que rodaram
    xatomic_int  filhos_fora;  // submits reentrantes que falharam
    xatomic_int  aninhadas;    // rodaram dentro do submit da tarefa_pai_grande (deque cheio)
    xatomic_int  presas;       // tarefa_presa rodando
    xatomic_int  soltar;       // 1 = as tarefa_presa terminam
}
Contexto;

static void tarefa_curta(void* arg)
{
    Contexto* c = (Contexto*)arg;
    if (g_dentro_do_pai) atomic_add_inline(&c->aninhadas, 1);
    atomic_add_inline(&c->feitas, 1);
}

static void tarefa_longa(void* arg)
{
    Contexto* c = (Contexto*)arg;
    long long fim = agora_us() + LONGA_US;
    while (agora_us() < fim) { }
    atomic_add_inline(&c->feitas, 1);
}

static void tarefa_pai(void* arg)
{
    Contexto* c = (Contexto*)arg;
    for (int i = 0; i < FILHOS_POR_PAI; i++)
        if (!pool_submit_relative(c->pool, tarefa_curta, c)) atomic_add_inline(&c->filhos_fora, 1);
    atomic_add_inline(&c->feitas, 1);
}

// Prende o worker ate o pai grande terminar de submeter: ninguem rouba do deque dele.
static void tarefa_presa(void* arg)
{
    Contexto* c = (Contexto*)arg;
    atomic_add_inline(&c->presas, 1);
    while (!atomic_get_inline(&c->soltar)) { }
    atomic_add_inline(&c->feitas, 1);
}

static void tarefa_pai_grande(void* arg)
{
    Contexto* c = (Contexto*)arg;
    g_dentro_do_pai = 1;
    for (int i = 0; i < FILHOS_DO_PAI_GRANDE; i++)
        if (!pool_submit_relative(c->pool, tarefa_curta, c)) atomic_add_inline(&c->filhos_fora, 1);
    g_dentro_do_pai = 0;
    atomic_set_inline(&c->soltar, 1);
    atomic_add_inline(&c->feitas, 1);
}

void teste_pool_contadores_sob_monitor(TestResult* r)
{
    Contexto c;
    int workers = 0, core = 0, enviadas = 0, falhas = 0;
    long long t0, fim, t_curtas, t_longas, t_pais, t_cheio;

    t_start(r);
    atomic_initialize_inline(&c.feitas, 0);
    atomic_initialize_inline(&c.filhos_fora, 0);
    atomic_initialize_inline(&c.aninhadas, 0);
    atomic_initialize_inline(&c.presas, 0);
    atomic_initialize_inline(&c.soltar, 0);
    c.pool = pool_create_relative(0);
    T_ASSERT(r, c.pool != 0, "pool_create_relative falhou");
    pool_perfil_relative(c.pool, POOL_PERFIL_PERFORMANCE);   // monitor a cada POOL_MON_MS
    pool_dims_relative(c.pool, &workers, &core);

    // Fase 1: rajadas curtas -- start_count/in_task mudando o tempo todo sob o monitor.
    t0 = agora_us();
    fim = t0 + CURTAS_MS * 1000;
    while (agora_us() < fim)
    {
        for (int i = 0; i < LOTE; i++)
            if (pool_submit_relative(c.pool, tarefa_curta, &c)) enviadas++; else falhas++;
        pool_wait_idle_relative(c.pool);
    }
    t_curtas = agora_us() - t0;

    // Fase 2: todos os workers presos numa tarefa longa, com fila atras. O monitor ve o
    // mesmo start_count com in_task ligado em dois passos e acorda os elasticos.
    t0 = agora_us();
    for (int k = 0; k < RODADAS_LONGAS; k++)
    {
        for (int i = 0; i < workers; i++)
            if (pool_submit_relative(c.pool, tarefa_longa, &c)) enviadas++; else falhas++;
        for (int i = 0; i < FILA_ATRAS; i++)
            if (pool_submit_relative(c.pool, tarefa_curta, &c)) enviadas++; else falhas++;
        pool_wait_idle_relative(c.pool);
    }
    t_longas = agora_us() - t0;

    // Fase 3: submit reentrante (a tarefa do worker submete no proprio deque, os outros roubam).
    t0 = agora_us();
    for (int i = 0; i < PAIS; i++)
        if (pool_submit_relative(c.pool, tarefa_pai, &c)) enviadas += 1 + FILHOS_POR_PAI; else falhas++;
    pool_wait_idle_relative(c.pool);
    t_pais = agora_us() - t0;

    // Fase 4: deque cheio. Os outros workers ficam presos ate o pai grande terminar (sem eles o
    // roubo esvazia o deque mais rapido que o pai enche, com muitos nucleos), e o excedente
    // roda aninhado no pool_run do pai: in_task = 2.
    t0 = agora_us();
    int tentativas = 0, presas = 0;
    while (tentativas < TENTATIVAS_CHEIO && atomic_get_inline(&c.aninhadas) == 0)
    {
        tentativas++;
        atomic_set_inline(&c.presas, 0);
        atomic_set_inline(&c.soltar, 0);
        for (int i = 0; i < workers - 1; i++)
            if (pool_submit_relative(c.pool, tarefa_presa, &c)) enviadas++; else falhas++;
        // Um worker pode pegar duas de uma vez (lote do shard) e a segunda so roda depois: a
        // espera para quando todos estao presos ou quando a contagem para de subir por PARADA_US.
        fim = agora_us() + ESPERA_PRESOS_US;
        long long desde = 0;
        for (int vistas = -1; agora_us() < fim; )
        {
            int n = atomic_get_inline(&c.presas);
            if (n >= workers - 1) break;
            if (n != vistas) { vistas = n; desde = agora_us(); }
            else if (agora_us() - desde > PARADA_US) break;
        }
        presas = atomic_get_inline(&c.presas);
        if (pool_submit_relative(c.pool, tarefa_pai_grande, &c)) enviadas += 1 + FILHOS_DO_PAI_GRANDE;
        else { falhas++; atomic_set_inline(&c.soltar, 1); }
        pool_wait_idle_relative(c.pool);
    }
    t_cheio = agora_us() - t0;

    int feitas = atomic_get_inline(&c.feitas);
    int filhos_fora = atomic_get_inline(&c.filhos_fora);
    int aninhadas = atomic_get_inline(&c.aninhadas);
    pool_destroy_relative(c.pool);

    t_logf("  pool: %d workers (%d core) | curtas %lld ms, longas %lld ms, reentrantes %lld ms, deque cheio %lld ms\n",
           workers, core, t_curtas / 1000, t_longas / 1000, t_pais / 1000, t_cheio / 1000);
    t_logf("  tarefas: %d enviadas, %d rodaram, %d aninhadas (tentativa %d; %d de %d workers presos ao soltar o pai), %d submits recusados, %d reentrantes recusados\n",
           enviadas, feitas, aninhadas, tentativas, presas, workers - 1, falhas, filhos_fora);

    T_ASSERT(r, falhas == 0, "%d submits recusados com o pool rodando", falhas);
    T_ASSERT(r, filhos_fora == 0, "%d submits reentrantes recusados", filhos_fora);
    T_ASSERT(r, feitas == enviadas - filhos_fora, "rodaram %d de %d tarefas", feitas, enviadas);
    T_ASSERT(r, aninhadas > 0, "nenhuma tarefa rodou aninhada em %d tentativas: o deque nao encheu, o caminho in_task = 2 ficou de fora", tentativas);
}
