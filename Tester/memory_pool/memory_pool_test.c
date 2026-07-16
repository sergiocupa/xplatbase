#include "../../Xplatbase/Xplatbase/src/memory_pool.h"
#include "../../Xplatbase/Xplatbase/src/mem_leak_watch.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#ifdef XPLATBASE_WIN
#include <share.h>
#endif

static int g_failed = 0;

#define CHECK(name, expr) do { \
    if (expr) { printf("  [OK]   %s\n", name); } \
    else { printf("  [FAIL] %s\n", name); g_failed++; } \
} while (0)

static void reset_pool(void)
{
    memop_test_reset();
    thread_init(memop_on_created_thread, memop_on_ended_thread);
}

/* ------------------------------------------------------------------ */
/* Helpers do TESTE 6 (mem_leak_watch)                                  */
/* ------------------------------------------------------------------ */
#define TEST_SPAN_SIZE ((uintptr_t)65536)   /* == MEMOP_SPAN_SIZE em memory_pool.c */
#define TEST_SPAN_MASK (~(TEST_SPAN_SIZE - 1))

static void span_hex_of(void* ptr, char* out, size_t out_sz)
{
    void* span = (void*)((uintptr_t)ptr & TEST_SPAN_MASK);
    snprintf(out, out_sz, "%p", span);
}

/* Conta linhas do log cujo campo 'span' bate com o span (64KB) de 'ptr'
 * (qualquer linha, se ptr==NULL) e que contenham 'filter' (qualquer uma,
 * se filter==NULL). Comparacao por endereco exato evita ter que resetar o
 * pool entre cenarios -- cada um so procura pelo proprio ponteiro. */
static int count_log_matches(const char* path, void* ptr, const char* filter)
{
    /* mem_leak_watch mantem o log aberto (modo append) durante toda a sessao
     * start/stop -- abrir aqui com fopen() puro pode esbarrar em violacao de
     * compartilhamento nesse meio-tempo. _fsopen com _SH_DENYNO pede acesso
     * explicitamente compartilhado (so leitura), sem essa disputa. */
#ifdef XPLATBASE_WIN
    FILE* f = _fsopen(path, "r", _SH_DENYNO);
#else
    FILE* f = fopen(path, "r");
#endif
    char line[1024];
    char span_hex[32] = "";
    int n = 0;
    if (!f) return 0;
    if (ptr) span_hex_of(ptr, span_hex, sizeof(span_hex));
    fgets(line, sizeof(line), f);   /* cabecalho */
    while (fgets(line, sizeof(line), f))
    {
        int span_ok = !ptr || strstr(line, span_hex) != NULL;
        int filt_ok = !filter || strstr(line, filter) != NULL;
        if (span_ok && filt_ok) n++;
    }
    fclose(f);
    return n;
}

static void test_basic_alloc_free(void)
{
    MemBuffer b;
    MemPoolStats s;

    printf("\nTESTE 1: alloc/free basico\n");
    reset_pool();

    b = memop_alloc(32);
    CHECK("alloc retorna ponteiro", b.Ptr != NULL);
    CHECK("buffer atende tamanho pedido", b.Size >= 32);

    if (b.Ptr)
    {
        memset(b.Ptr, 0xAB, (size_t)b.Size);
    }

    memop_free(&b);
    CHECK("free zera ponteiro", b.Ptr == NULL);

    memop_get_stats(&s);
    CHECK("stats alloc_count == 1", s.alloc_count == 1);
    CHECK("stats free_count == 1", s.free_count == 1);
    CHECK("refill sincrono inicial aconteceu", s.sync_refills >= 1);
}

static void test_size_classes(void)
{
    MemBuffer b1;
    MemBuffer b2;
    MemBuffer b3;

    printf("\nTESTE 2: classes de tamanho\n");
    reset_pool();

    b1 = memop_alloc(64);
    b2 = memop_alloc(65);
    b3 = memop_alloc(4097);

    CHECK("64 usa classe >= 64", b1.Ptr != NULL && b1.Size >= 64);
    CHECK("65 sobe para classe maior", b2.Ptr != NULL && b2.Size >= 65);
    CHECK("4097 sobe para classe maior", b3.Ptr != NULL && b3.Size >= 4097);

    memop_free(&b1);
    memop_free(&b2);
    memop_free(&b3);
}

#ifdef XPLATBASE_WIN
static DWORD WINAPI lane_thread_fn(void* arg)
#else
static void* lane_thread_fn(void* arg)
#endif
{
    MemBuffer b;
    int* ok = (int*)arg;

    b = memop_alloc(128);
    if (b.Ptr && b.Size >= 128)
    {
        memset(b.Ptr, 0xCD, (size_t)b.Size);
        *ok = 1;
    }
    memop_free(&b);

    return (xthread_result_t)0;
}

static void test_thread_lane_lifecycle(void)
{
    int status = 0;
    int ok = 0;
    Thread* t;
    MemPoolStats s;

    printf("\nTESTE 3: lane por thread criada\n");
    reset_pool();

    t = thread_create(lane_thread_fn, &ok, &status);
    CHECK("thread criada", status == 1 && t != NULL);
    thread_join(&t);

    CHECK("alloc dentro da thread funcionou", ok == 1);
    memop_get_stats(&s);
    CHECK("callback criou lane", s.lanes_created >= 1);
    CHECK("callback encerrou lane", s.lanes_destroyed >= 1);
}

static void test_span_growth(void)
{
    /* v2: sem worker assincrono. Consumir mais blocos do que cabe num span
     * (64KB) deve disparar refill sincrono = alocar spans adicionais. */
    enum { N = 5000 };
    MemBuffer* buffers;
    MemPoolStats s;
    int i;
    int all_allocs_ok = 1;

    printf("\nTESTE 4: crescimento por spans (refill sincrono)\n");
    reset_pool();
    buffers = (MemBuffer*)calloc(N, sizeof(MemBuffer));

    for (i = 0; i < N; i++)
    {
        buffers[i] = memop_alloc(32);
        if (!buffers[i].Ptr) all_allocs_ok = 0;
        else { ((char*)buffers[i].Ptr)[0] = (char)i; }
    }
    CHECK("5000 allocs de 32B ok", all_allocs_ok);

    memop_get_stats(&s);
    CHECK("consumo > 1 span alocou spans extras", s.sync_refills >= 2);
    CHECK("alloc_count contabilizado", s.alloc_count >= (uint64)N);

    for (i = 0; i < N; i++) memop_free(&buffers[i]);
    free(buffers);
}

/* TESTE 5: frees REMOTOS a spans cheios devem reativa-los (lista full ->
 * classe) quando o dono volta a alocar, sem precisar criar spans novos. */
typedef struct RemoteCtx
{
    MemBuffer* bufs;
    volatile int ready;      /* worker terminou round 1            */
    volatile int freed;      /* main liberou tudo (frees remotos)  */
    volatile int round2_ok;  /* worker realocou tudo no round 2    */
    uint64 refills_r1;       /* sync_refills apos o round 1        */
} RemoteCtx;

enum { RN = 3000 };   /* 3000 x 64B ~= 3 spans da classe 64 */

#ifdef XPLATBASE_WIN
static DWORD WINAPI remote_owner_fn(void* arg)
#else
static void* remote_owner_fn(void* arg)
#endif
{
    RemoteCtx* ctx = (RemoteCtx*)arg;
    MemPoolStats s;
    int i, ok = 1;

    for (i = 0; i < RN; i++)
    {
        ctx->bufs[i] = memop_alloc(64);
        if (!ctx->bufs[i].Ptr) ok = 0;
        else ((char*)ctx->bufs[i].Ptr)[0] = (char)i;
    }
    memop_get_stats(&s);
    ctx->refills_r1 = s.sync_refills;
    ctx->ready = 1;

    while (!ctx->freed) thread_sleep0();

    /* round 2: os blocos voltaram por free REMOTO; deve reaproveitar os spans
     * reativados em vez de pedir memoria nova. */
    for (i = 0; i < RN; i++)
    {
        ctx->bufs[i] = memop_alloc(64);
        if (!ctx->bufs[i].Ptr) ok = 0;
        else ((char*)ctx->bufs[i].Ptr)[0] = (char)(i + 1);
    }
    for (i = 0; i < RN; i++) memop_free(&ctx->bufs[i]);
    ctx->round2_ok = ok;
    return (xthread_result_t)0;
}

static void test_remote_free_reactivation(void)
{
    RemoteCtx ctx;
    MemBuffer warm;
    MemPoolStats s1, s2;
    Thread* t;
    int status = 0;
    int i;

    printf("\nTESTE 5: free remoto reativa spans cheios\n");
    reset_pool();

    memset(&ctx, 0, sizeof(ctx));
    ctx.bufs = (MemBuffer*)calloc(RN, sizeof(MemBuffer));

    /* garante heap na main (stats de free remoto sao do heap do liberador) */
    warm = memop_alloc(16);
    memop_free(&warm);

    t = thread_create(remote_owner_fn, &ctx, &status);
    CHECK("thread dona criada", status == 1 && t != NULL);

    while (!ctx.ready) thread_sleep0();

    /* frees REMOTOS: main libera blocos cujo dono e o worker */
    for (i = 0; i < RN; i++) memop_free(&ctx.bufs[i]);
    memop_get_stats(&s1);
    ctx.freed = 1;

    thread_join(&t);
    memop_get_stats(&s2);

    CHECK("frees remotos contabilizados", s1.remote_frees >= (uint64)RN);
    CHECK("round 2 realocou tudo", ctx.round2_ok == 1);
    CHECK("reativou spans (sem criar novos)", s2.sync_refills <= ctx.refills_r1 + 1);

    free(ctx.bufs);
}

/* ------------------------------------------------------------------ */
/* TESTE 6: mem_leak_watch -- deteccao de vazamento sem colecao         */
/* ------------------------------------------------------------------ */

#ifdef XPLATBASE_WIN
static DWORD WINAPI lw_leak_simple_fn(void* arg)
#else
static void* lw_leak_simple_fn(void* arg)
#endif
{
    void** out = (void**)arg;
    *out = memop_alloc_raw(96);
    /* nao guarda em lugar alcancavel; thread termina agora -- vazamento de verdade */
    return (xthread_result_t)0;
}

typedef struct { void* kept; volatile int* stop; } LwAliveCtx;

#ifdef XPLATBASE_WIN
static DWORD WINAPI lw_alive_ref_fn(void* arg)
#else
static void* lw_alive_ref_fn(void* arg)
#endif
{
    LwAliveCtx* ctx = (LwAliveCtx*)arg;
    void* kept = memop_alloc_raw(160);
    ctx->kept = kept;
    while (!*ctx->stop)
    {
        Sleep(20);
        { volatile void* keep_alive = kept; (void)keep_alive; }   /* mantem 'kept' vivo na pilha */
    }
    return (xthread_result_t)0;
}

typedef struct { void* ptrs[5]; } LwMultiCtx;

#ifdef XPLATBASE_WIN
static DWORD WINAPI lw_leak_multi_fn(void* arg)
#else
static void* lw_leak_multi_fn(void* arg)
#endif
{
    LwMultiCtx* ctx = (LwMultiCtx*)arg;
    int i;
    for (i = 0; i < 5; i++) ctx->ptrs[i] = memop_alloc_raw(48);   /* mesma classe -> mesmo span */
    return (xthread_result_t)0;
}

#ifdef XPLATBASE_WIN
static DWORD WINAPI lw_leak_large_fn(void* arg)
#else
static void* lw_leak_large_fn(void* arg)
#endif
{
    void** out = (void**)arg;
    *out = memop_alloc_raw(32768);   /* >16KB: caminho LARGE, fora do rastreio hoje (limite documentado) */
    return (xthread_result_t)0;
}

static void* g_lw_static_ref = NULL;   /* global: NUNCA e' varrido como raiz (limite documentado) */

#ifdef XPLATBASE_WIN
static DWORD WINAPI lw_leak_static_ref_fn(void* arg)
#else
static void* lw_leak_static_ref_fn(void* arg)
#endif
{
    (void)arg;
    g_lw_static_ref = memop_alloc_raw(72);
    return (xthread_result_t)0;
}

static void test_leak_watch(void)
{
    MemLeakWatchConfig cfg;
    const char* path = "memory_pool_test_leak.log";
    Thread* t;
    int status;
    void* leaked_simple = NULL;
    void* leaked_large  = NULL;
    LwAliveCtx alive_ctx;
    LwMultiCtx multi_ctx;
    volatile int stop_flag = 0;

    printf("\nTESTE 6: mem_leak_watch - deteccao sem colecao\n");
    reset_pool();
    remove(path);

    mem_leak_watch_default_config(&cfg);
    cfg.interval_ms = 60000;   /* nao deixa o timer interferir; controlamos via scan_now() */
    cfg.log_path = path;
    CHECK("mem_leak_watch iniciou", mem_leak_watch_start(&cfg) == true);

    /* 6.1: vazamento simples -- thread termina sem guardar o ponteiro em lugar nenhum. */
    t = thread_create(lw_leak_simple_fn, &leaked_simple, &status);
    thread_join(&t);
    mem_leak_watch_scan_now();
    CHECK("6.1 vazamento simples aparece como leak_candidate",
        count_log_matches(path, leaked_simple, "leak_candidate") >= 1);

    /* 6.2: referencia mantida viva na pilha de uma thread AINDA RODANDO -- nao deve aparecer. */
    memset(&alive_ctx, 0, sizeof(alive_ctx));
    alive_ctx.stop = &stop_flag;
    t = thread_create(lw_alive_ref_fn, &alive_ctx, &status);
    while (!alive_ctx.kept) Sleep(5);   /* espera a thread alocar e publicar o ponteiro */
    Sleep(50);                          /* garante que ja entrou no loop de espera */
    mem_leak_watch_scan_now();
    CHECK("6.2 referencia viva (pilha de thread_create) NAO aparece",
        count_log_matches(path, alive_ctx.kept, "leak_candidate") == 0);
    stop_flag = 1;
    thread_join(&t);

    /* 6.3: 5 blocos vazados na MESMA classe -- granularidade e por SPAN inteiro
     * (64KB), entao devem virar 1 unica linha de report, nao 5. */
    t = thread_create(lw_leak_multi_fn, &multi_ctx, &status);
    thread_join(&t);
    mem_leak_watch_scan_now();
    {
        int same_span = 1, i;
        char h0[32], hi[32];
        span_hex_of(multi_ctx.ptrs[0], h0, sizeof(h0));
        for (i = 1; i < 5; i++)
        {
            span_hex_of(multi_ctx.ptrs[i], hi, sizeof(hi));
            if (strcmp(h0, hi) != 0) same_span = 0;
        }
        CHECK("6.3 os 5 blocos ficaram no mesmo span (pre-condicao do teste)", same_span);
        CHECK("6.3 5 blocos vazados no mesmo span viram 1 unica linha de report",
            count_log_matches(path, multi_ctx.ptrs[0], "leak_candidate") == 1);
    }

    /* 6.4: alocacao LARGE (>16KB) vazada -- limite documentado: hoje nao fica
     * em heap->spans[]/full, entao nao entra na varredura. Confirma o gap. */
    t = thread_create(lw_leak_large_fn, &leaked_large, &status);
    thread_join(&t);
    mem_leak_watch_scan_now();
    CHECK("6.4 vazamento LARGE nao e detectado (limite documentado)",
        count_log_matches(path, leaked_large, "leak_candidate") == 0);

    /* 6.5: referencia mantida SO numa variavel global/estatica -- o scan so
     * varre pilha/registrador de thread viva, nunca secao de dados global.
     * Falso positivo ESPERADO e documentado: deve aparecer mesmo "em uso". */
    t = thread_create(lw_leak_static_ref_fn, NULL, &status);
    thread_join(&t);
    mem_leak_watch_scan_now();
    CHECK("6.5 referencia so em global conta como falso positivo (limite documentado)",
        count_log_matches(path, g_lw_static_ref, "leak_candidate") >= 1);

    mem_leak_watch_stop();

    /* 6.6: escalonamento automatico pra CRITICAL via limiares baixos --
     * desta vez quem decide o nivel e' o TIMER, nao o scan_now() manual. */
    mem_leak_watch_default_config(&cfg);
    cfg.interval_ms = 100;
    cfg.warn_threshold_bytes = 1;
    cfg.crit_threshold_bytes = 1;
    cfg.log_path = path;
    remove(path);
    CHECK("mem_leak_watch reiniciou p/ teste de CRITICAL", mem_leak_watch_start(&cfg) == true);
    Sleep(400);   /* deixa o timer (100ms) disparar e escalonar pra CRITICAL */
    mem_leak_watch_stop();
    CHECK("6.6 limiar baixo escalona automaticamente pra CRITICAL",
        count_log_matches(path, NULL, "CRITICAL") >= 1);

    remove(path);
}

int main(void)
{
    printf("memory_pool_test\n");

    test_basic_alloc_free();
    test_size_classes();
    test_thread_lane_lifecycle();
    test_span_growth();
    test_remote_free_reactivation();
    test_leak_watch();

    memop_shutdown();

    if (g_failed)
    {
        printf("\nRESULTADO: %d falhas\n", g_failed);
        return 1;
    }

    printf("\nRESULTADO: todos os testes passaram\n");
    return 0;
}
