#include "../../Xplatbase/Xplatbase/src/memory_pool.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

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

int main(void)
{
    printf("memory_pool_test\n");

    test_basic_alloc_free();
    test_size_classes();
    test_thread_lane_lifecycle();
    test_span_growth();
    test_remote_free_reactivation();

    memop_shutdown();

    if (g_failed)
    {
        printf("\nRESULTADO: %d falhas\n", g_failed);
        return 1;
    }

    printf("\nRESULTADO: todos os testes passaram\n");
    return 0;
}
