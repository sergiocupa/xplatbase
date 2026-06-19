#include "../../Xplatbase/Xplatbase/src/memory_pool.h"

#include <stdio.h>
#include <string.h>

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

static void test_async_refill(void)
{
    enum { N = 80 };
    MemBuffer buffers[N];
    MemPoolStats s;
    int i;
    int all_allocs_ok = 1;
    int saw_async = 0;

    printf("\nTESTE 4: refill assincrono por threshold\n");
    reset_pool();
    memset(buffers, 0, sizeof(buffers));

    for (i = 0; i < N; i++)
    {
        buffers[i] = memop_alloc(32);
        if (!buffers[i].Ptr) all_allocs_ok = 0;
    }
    CHECK("allocs durante consumo", all_allocs_ok);

    for (i = 0; i < 200; i++)
    {
        memop_get_stats(&s);
        if (s.async_refills > 0)
        {
            saw_async = 1;
            break;
        }
        thread_sleep0();
    }

    memop_get_stats(&s);
    CHECK("threshold gerou pedido assincrono", s.async_requests > 0);
    CHECK("worker executou refill assincrono", saw_async || s.async_refills > 0);

    for (i = 0; i < N; i++)
    {
        memop_free(&buffers[i]);
    }
}

int main(void)
{
    printf("memory_pool_test\n");

    test_basic_alloc_free();
    test_size_classes();
    test_thread_lane_lifecycle();
    test_async_refill();

    memop_shutdown();

    if (g_failed)
    {
        printf("\nRESULTADO: %d falhas\n", g_failed);
        return 1;
    }

    printf("\nRESULTADO: todos os testes passaram\n");
    return 0;
}
