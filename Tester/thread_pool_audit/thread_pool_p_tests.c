#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "thread_pool.h"
#include "ring_queue.h"

typedef struct {
    int started;
    int timed_out;
    DWORD exit_code;
} ChildResult;

static int g_manifested;
static int g_run;

static void disable_error_dialogs(void)
{
    SetErrorMode(SEM_FAILCRITICALERRORS |
                 SEM_NOGPFAULTERRORBOX |
                 SEM_NOOPENFILEERRORBOX);
}

static void report(const char* id, int manifested, const char* detail)
{
    g_run++;
    if (manifested) g_manifested++;
    printf("  [%s] %s - %s\n",
           manifested ? "MANIFESTOU" : "NAO MANIFESTOU", id, detail);
}

static ChildResult run_child(const char* mode, DWORD timeout_ms)
{
    ChildResult result = {0};
    char exe[MAX_PATH];
    char command[MAX_PATH + 128];
    STARTUPINFOA startup = {0};
    PROCESS_INFORMATION process = {0};

    startup.cb = sizeof(startup);
    if (!GetModuleFileNameA(NULL, exe, MAX_PATH)) return result;
    snprintf(command, sizeof(command), "\"%s\" --child %s", exe, mode);
    if (!CreateProcessA(NULL, command, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, NULL, &startup, &process))
        return result;

    result.started = 1;
    if (WaitForSingleObject(process.hProcess, timeout_ms) == WAIT_TIMEOUT) {
        result.timed_out = 1;
        TerminateProcess(process.hProcess, 0xEE);
        WaitForSingleObject(process.hProcess, 2000);
    }
    GetExitCodeProcess(process.hProcess, &result.exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return result;
}

static double now_ms(void)
{
    static LARGE_INTEGER frequency;
    LARGE_INTEGER value;
    if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&value);
    return (double)value.QuadPart * 1000.0 / (double)frequency.QuadPart;
}

/* P01: submit reentrante entra em backpressure circular. */
static ShardedPool* p01_pool;
static volatile LONG p01_progress;
static volatile LONG p01_children;

static void p01_child(void* unused)
{
    (void)unused;
    InterlockedIncrement(&p01_children);
}

static void p01_parent(void* unused)
{
    int i;
    (void)unused;
    for (i = 0; i < 3; i++) {
        pool_submit(p01_pool, p01_child, NULL);
        InterlockedIncrement(&p01_progress);
    }
}

static int child_p01(void)
{
    PoolConfig cfg = pool_default_config();
    int i;
    cfg.shard_count = 1;
    cfg.max_shards = 1;
    cfg.max_auto_expand_lanes = 1;
    cfg.ring_capacity = 2;
    cfg.rescue_backlog_threshold = 1000000;
    cfg.long_task_threshold_ns = 10000000000ULL;
    p01_progress = p01_children = 0;
    p01_pool = pool_create(&cfg);
    if (!p01_pool) return 10;
    if (!pool_submit(p01_pool, p01_parent, NULL)) return 11;
    for (i = 0; i < 1000; i++) Sleep(1);
    return p01_progress == 2 && p01_children == 0 ? 31 : 0;
}

/* P02: submit/stats continuam usando o ponteiro enquanto shutdown libera. */
static ShardedPool* p02_pool;
static volatile LONG p02_stop;
static void p02_noop(void* unused) { (void)unused; }

static DWORD WINAPI p02_caller(void* raw)
{
    PoolStats stats;
    int submitter = (int)(intptr_t)raw;
    while (!InterlockedCompareExchange(&p02_stop, 0, 0)) {
        if (submitter) pool_submit(p02_pool, p02_noop, NULL);
        else pool_stats(p02_pool, &stats);
    }
    return 0;
}

static int child_p02(void)
{
    enum { COUNT = 12 };
    PoolConfig cfg = pool_default_config();
    HANDLE threads[COUNT];
    int i;

    cfg.shard_count = 2;
    cfg.max_shards = 2;
    cfg.ring_capacity = 32;
    cfg.shutdown_drain_timeout_ms = 40;
    cfg.shutdown_join_timeout_ms = 40;
    p02_stop = 0;
    p02_pool = pool_create(&cfg);
    if (!p02_pool) return 10;
    for (i = 0; i < COUNT; i++)
        threads[i] = CreateThread(NULL, 0, p02_caller,
                                  (void*)(intptr_t)(i & 1), 0, NULL);
    Sleep(10);
    pool_shutdown(p02_pool);
    for (i = 0; i < 10000; i++) {
        void* memory = malloc(4096);
        if (memory) {
            memset(memory, 0xA5, 4096);
            free(memory);
        }
    }
    Sleep(100);
    InterlockedExchange(&p02_stop, 1);
    WaitForMultipleObjects(COUNT, threads, TRUE, 2000);
    for (i = 0; i < COUNT; i++) CloseHandle(threads[i]);
    return 0;
}

/* P03: callback NULL e aceito e deixa pending_tasks sem consumidor. */
static void test_p03(void)
{
    PoolConfig cfg = pool_default_config();
    ShardedPool* pool;
    int accepted;
    double start;
    double elapsed;
    char detail[128];

    cfg.shard_count = 1;
    cfg.max_shards = 1;
    cfg.ring_capacity = 8;
    cfg.shutdown_drain_timeout_ms = 200;
    cfg.shutdown_join_timeout_ms = 200;
    pool = pool_create(&cfg);
    if (!pool) {
        report("P03 callback NULL", 0, "pool_create falhou");
        return;
    }
    accepted = pool_submit(pool, NULL, NULL) ? 1 : 0;
    Sleep(30);
    start = now_ms();
    pool_shutdown(pool);
    elapsed = now_ms() - start;
    snprintf(detail, sizeof(detail), "accepted=%d shutdown=%.0fms", accepted, elapsed);
    report("P03 callback NULL", accepted && elapsed >= 150.0, detail);
}

/* P04: shutdown chamado dentro do callback espera o proprio worker. */
static ShardedPool* p04_pool;
static volatile LONG p04_started;
static volatile LONG p04_returned;

static void p04_callback(void* unused)
{
    (void)unused;
    InterlockedExchange(&p04_started, 1);
    pool_shutdown(p04_pool);
    InterlockedExchange(&p04_returned, 1);
}

static int child_p04(void)
{
    PoolConfig cfg = pool_default_config();
    int i;
    cfg.shard_count = 1;
    cfg.max_shards = 1;
    cfg.shutdown_drain_timeout_ms = 100;
    cfg.shutdown_join_timeout_ms = 100;
    cfg.shutdown_force_kill = true;
    p04_started = p04_returned = 0;
    p04_pool = pool_create(&cfg);
    if (!p04_pool) return 10;
    pool_submit(p04_pool, p04_callback, NULL);
    for (i = 0; i < 3000 && !p04_started; i++) Sleep(1);
    Sleep(700);
    return p04_started && !p04_returned ? 41 : 0;
}

/* P08: force-kill pode deixar lock do usuario permanentemente retido. */
static CRITICAL_SECTION p08_lock;
static volatile LONG p08_started;

static void p08_callback(void* unused)
{
    (void)unused;
    EnterCriticalSection(&p08_lock);
    InterlockedExchange(&p08_started, 1);
    for (;;) YieldProcessor();
}

static int child_p08(void)
{
    PoolConfig cfg = pool_default_config();
    ShardedPool* pool;
    int i;
    int acquired;
    InitializeCriticalSection(&p08_lock);
    p08_started = 0;
    cfg.shard_count = 1;
    cfg.max_shards = 1;
    cfg.shutdown_drain_timeout_ms = 50;
    cfg.shutdown_join_timeout_ms = 50;
    cfg.shutdown_force_kill = true;
    pool = pool_create(&cfg);
    if (!pool) return 10;
    pool_submit(pool, p08_callback, NULL);
    for (i = 0; i < 2000 && !p08_started; i++) Sleep(1);
    pool_shutdown(pool);
    acquired = TryEnterCriticalSection(&p08_lock) ? 1 : 0;
    if (acquired) LeaveCriticalSection(&p08_lock);
    return acquired ? 0 : 48;
}

/* P12/P14: peek e count observam um ring MPMC em mutacao. */
typedef struct {
    uint64_t value[8];
    uint64_t inverse[8];
} PItem;

static RingQueue p_ring;
static PItem* p_buffer;
static volatile LONG p_ring_stop;
static volatile LONG p_bad_peek;
static volatile LONG p_bad_count;
static volatile LONG64 p_sequence;

static void make_item(PItem* item, uint64_t value)
{
    int i;
    for (i = 0; i < 8; i++) {
        item->value[i] = value;
        item->inverse[i] = ~value;
    }
}

static int item_valid(const PItem* item)
{
    int i;
    uint64_t value = item->value[0];
    for (i = 0; i < 8; i++)
        if (item->value[i] != value || item->inverse[i] != ~value) return 0;
    return 1;
}

static DWORD WINAPI p_ring_producer(void* unused)
{
    PItem item;
    (void)unused;
    while (!InterlockedCompareExchange(&p_ring_stop, 0, 0)) {
        make_item(&item, (uint64_t)InterlockedIncrement64(&p_sequence));
        xring_push_mp(&p_ring, p_buffer, &item);
    }
    return 0;
}

static DWORD WINAPI p_ring_consumer(void* unused)
{
    PItem item;
    (void)unused;
    while (!InterlockedCompareExchange(&p_ring_stop, 0, 0))
        xring_pop_mc(&p_ring, p_buffer, &item);
    return 0;
}

static DWORD WINAPI p_ring_observer(void* unused)
{
    PItem item;
    int count;
    (void)unused;
    while (!InterlockedCompareExchange(&p_ring_stop, 0, 0)) {
        count = ring_queue_count(&p_ring);
        if (count < 0 || count > p_ring.capacity)
            InterlockedIncrement(&p_bad_count);
        if (xring_peek(&p_ring, p_buffer, &item) && !item_valid(&item))
            InterlockedIncrement(&p_bad_peek);
    }
    return 0;
}

static void test_p12_p14(void)
{
    HANDLE threads[7];
    int i;
    char detail[128];

    p_buffer = (PItem*)calloc(2, sizeof(PItem));
    ring_queue_init(&p_ring, 2);
    if (!p_buffer || !p_ring.seqno) {
        report("P12 xring_peek racy", 0, "falha de alocacao");
        report("P14 ring_queue_count", 0, "falha de alocacao");
        return;
    }
    p_ring_stop = p_bad_peek = p_bad_count = 0;
    p_sequence = 0;
    threads[0] = CreateThread(NULL, 0, p_ring_producer, NULL, 0, NULL);
    threads[1] = CreateThread(NULL, 0, p_ring_producer, NULL, 0, NULL);
    threads[2] = CreateThread(NULL, 0, p_ring_consumer, NULL, 0, NULL);
    threads[3] = CreateThread(NULL, 0, p_ring_consumer, NULL, 0, NULL);
    threads[4] = CreateThread(NULL, 0, p_ring_observer, NULL, 0, NULL);
    threads[5] = CreateThread(NULL, 0, p_ring_observer, NULL, 0, NULL);
    threads[6] = CreateThread(NULL, 0, p_ring_observer, NULL, 0, NULL);
    for (i = 0; i < 500 && (!p_bad_peek || !p_bad_count); i++) Sleep(10);
    InterlockedExchange(&p_ring_stop, 1);
    WaitForMultipleObjects(7, threads, TRUE, 5000);
    for (i = 0; i < 7; i++) CloseHandle(threads[i]);
    snprintf(detail, sizeof(detail), "peeks invalidos=%ld", p_bad_peek);
    report("P12 xring_peek racy", p_bad_peek > 0, detail);
    snprintf(detail, sizeof(detail), "counts fora da faixa=%ld", p_bad_count);
    report("P14 ring_queue_count", p_bad_count > 0, detail);
    ring_queue_destroy(&p_ring);
    free(p_buffer);
}

/* P13: indices signed atravessam INT_MAX. */
static void test_p13(void)
{
    RingQueue ring;
    int buffer[2] = {0};
    int first = 1;
    int second = 2;
    int ok1;
    int ok2;
    int tail;
    char detail[128];

    ring_queue_init(&ring, 2);
    if (!ring.seqno) {
        report("P13 overflow signed do ring", 0, "ring_queue_init falhou");
        return;
    }
    atomic_set(&ring.head, INT_MAX - 1);
    atomic_set(&ring.tail, INT_MAX - 1);
    atomic_set(&ring.seqno[0], INT_MAX - 1);
    atomic_set(&ring.seqno[1], INT_MAX);
    ok1 = xring_push_mp(&ring, buffer, &first);
    ok2 = xring_push_mp(&ring, buffer, &second);
    tail = atomic_get(&ring.tail);
    snprintf(detail, sizeof(detail), "push=%d/%d tail=%d", ok1, ok2, tail);
    report("P13 overflow signed do ring", ok1 && ok2 && tail < 0, detail);
    ring_queue_destroy(&ring);
}

/* P16: dispatches de rescue concorrentes podem ultrapassar ncores. */
static volatile LONG p16_running;
static volatile LONG p16_max;
static volatile LONG p16_done;
static HANDLE p16_gate;

static void p16_task(void* unused)
{
    LONG running = InterlockedIncrement(&p16_running);
    LONG current = InterlockedCompareExchange(&p16_max, 0, 0);
    (void)unused;
    while (current < running) {
        LONG old = InterlockedCompareExchange(&p16_max, running, current);
        if (old == current) break;
        current = old;
    }
    WaitForSingleObject(p16_gate, 5000);
    InterlockedDecrement(&p16_running);
    InterlockedIncrement(&p16_done);
}

typedef struct {
    ShardedPool* pool;
    HANDLE start;
} P16Submitter;

static DWORD WINAPI p16_submitter(void* raw)
{
    P16Submitter* submitter = (P16Submitter*)raw;
    int i;
    WaitForSingleObject(submitter->start, INFINITE);
    for (i = 0; i < 4; i++) pool_submit(submitter->pool, p16_task, NULL);
    return 0;
}

static void test_p16(void)
{
    enum { COUNT = 32, TOTAL = COUNT * 4 };
    PoolConfig cfg = pool_default_config();
    HANDLE threads[COUNT];
    P16Submitter args[COUNT];
    HANDLE start;
    ShardedPool* pool;
    int i;
    int cores = xcpu_count();
    LONG maximum;
    char detail[128];

    cfg.shard_count = 1;
    cfg.max_shards = 1;
    cfg.max_auto_expand_lanes = 1;
    cfg.ring_capacity = 256;
    cfg.reserve_size = 128;
    cfg.rescue_max_helpers_per_lane = 128;
    cfg.long_task_threshold_ns = 10000000000ULL;
    cfg.shutdown_drain_timeout_ms = 5000;
    p16_running = p16_max = p16_done = 0;
    p16_gate = CreateEventW(NULL, TRUE, FALSE, NULL);
    start = CreateEventW(NULL, TRUE, FALSE, NULL);
    pool = pool_create(&cfg);
    if (!pool) {
        report("P16 rescue oversubscription", 0, "pool_create falhou");
        return;
    }
    for (i = 0; i < COUNT; i++) {
        args[i].pool = pool;
        args[i].start = start;
        threads[i] = CreateThread(NULL, 0, p16_submitter, &args[i], 0, NULL);
    }
    SetEvent(start);
    WaitForMultipleObjects(COUNT, threads, TRUE, 10000);
    Sleep(300);
    maximum = InterlockedCompareExchange(&p16_max, 0, 0);
    SetEvent(p16_gate);
    for (i = 0; i < 5000 && p16_done < TOTAL; i++) Sleep(1);
    pool_shutdown(pool);
    for (i = 0; i < COUNT; i++) CloseHandle(threads[i]);
    CloseHandle(start);
    CloseHandle(p16_gate);
    snprintf(detail, sizeof(detail), "max_running=%ld cores=%d", maximum, cores);
    report("P16 rescue oversubscription", maximum > cores, detail);
}

/* P17: ponteiros publicos invalidos encerram o processo. */
static int child_p17(const char* mode)
{
    PoolStats stats;
    PoolConfig cfg;
    ShardedPool* pool;
    if (!strcmp(mode, "p17-submit-null-pool")) {
        pool_submit(NULL, p02_noop, NULL);
        return 0;
    }
    if (!strcmp(mode, "p17-stats-null-pool")) {
        pool_stats(NULL, &stats);
        return 0;
    }
    cfg = pool_default_config();
    cfg.shard_count = 1;
    cfg.max_shards = 1;
    pool = pool_create(&cfg);
    if (!pool) return 10;
    pool_stats(pool, NULL);
    pool_shutdown(pool);
    return 0;
}

/* P18: trocar o hook global muda pools que ja existem. */
static volatile LONG p18_hook_b;
static volatile LONG p18_stuck;
static void p18_log_a(int severity, const char* msg)
{
    (void)severity;
    (void)msg;
}
static void p18_log_b(int severity, const char* msg)
{
    (void)severity;
    if (strstr(msg, "shutdown:")) InterlockedIncrement(&p18_hook_b);
}
static void p18_stuck_task(void* unused)
{
    (void)unused;
    InterlockedExchange(&p18_stuck, 1);
    for (;;) YieldProcessor();
}

static int child_p18(void)
{
    PoolConfig cfg = pool_default_config();
    ShardedPool* pool;
    int i;
    cfg.shard_count = 1;
    cfg.max_shards = 1;
    cfg.shutdown_drain_timeout_ms = 50;
    cfg.shutdown_join_timeout_ms = 50;
    cfg.shutdown_force_kill = true;
    p18_hook_b = p18_stuck = 0;
    pool_set_log_hook(p18_log_a);
    pool = pool_create(&cfg);
    if (!pool) return 10;
    pool_set_log_hook(p18_log_b);
    pool_submit(pool, p18_stuck_task, NULL);
    for (i = 0; i < 2000 && !p18_stuck; i++) Sleep(1);
    pool_shutdown(pool);
    return p18_hook_b > 0 ? 58 : 0;
}

static void test_child_cases(int include_crash)
{
    ChildResult result;
    char detail[160];

    result = run_child("p01", 3000);
    snprintf(detail, sizeof(detail), "timeout=%d exit=0x%08lx",
             result.timed_out, (unsigned long)result.exit_code);
    report("P01 submit reentrante",
           result.started && result.exit_code == 31, detail);

    if (include_crash) {
        result = run_child("p02", 5000);
        snprintf(detail, sizeof(detail), "timeout=%d exit=0x%08lx",
                 result.timed_out, (unsigned long)result.exit_code);
        report("P02 lifetime concorrente",
               result.started && (result.timed_out || result.exit_code != 0), detail);
    } else {
        printf("  [PULADO] P02 lifetime concorrente - use --dangerous\n");
    }

    result = run_child("p04", 5000);
    snprintf(detail, sizeof(detail), "timeout=%d exit=0x%08lx",
             result.timed_out, (unsigned long)result.exit_code);
    report("P04 shutdown no callback",
           result.started && (result.timed_out || result.exit_code == 41), detail);

    result = run_child("p08", 5000);
    snprintf(detail, sizeof(detail), "exit=0x%08lx",
             (unsigned long)result.exit_code);
    report("P08 force-kill retendo lock",
           result.started && result.exit_code == 48, detail);

    if (include_crash) {
        ChildResult a = run_child("p17-submit-null-pool", 3000);
        ChildResult b = run_child("p17-stats-null-pool", 3000);
        ChildResult c = run_child("p17-stats-null-out", 3000);
        snprintf(detail, sizeof(detail), "exit submit=%08lx stats_pool=%08lx stats_out=%08lx",
                 (unsigned long)a.exit_code, (unsigned long)b.exit_code,
                 (unsigned long)c.exit_code);
        report("P17 ponteiros publicos invalidos",
               a.started && b.started && c.started &&
               a.exit_code != 0 && b.exit_code != 0 && c.exit_code != 0,
               detail);
    } else {
        printf("  [PULADO] P17 ponteiros invalidos - use --dangerous\n");
    }

    result = run_child("p18", 5000);
    snprintf(detail, sizeof(detail), "exit=0x%08lx",
             (unsigned long)result.exit_code);
    report("P18 hook global",
           result.started && result.exit_code == 58, detail);
}

static int child_dispatch(const char* mode)
{
    if (!strcmp(mode, "p01")) return child_p01();
    if (!strcmp(mode, "p02")) return child_p02();
    if (!strcmp(mode, "p04")) return child_p04();
    if (!strcmp(mode, "p08")) return child_p08();
    if (!strcmp(mode, "p18")) return child_p18();
    if (!strncmp(mode, "p17-", 4)) return child_p17(mode);
    return 2;
}

int main(int argc, char** argv)
{
    int dangerous = 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    disable_error_dialogs();
    if (argc >= 3 && !strcmp(argv[1], "--child"))
        return child_dispatch(argv[2]);
    if (argc >= 2 && !strcmp(argv[1], "--dangerous"))
        dangerous = 1;

    printf("================================================================\n");
    printf("  THREAD POOL - MANIFESTACAO P01-P21 (DINAMICOS WINDOWS)\n");
    printf("================================================================\n");
    test_child_cases(dangerous);
    test_p03();
    test_p12_p14();
    test_p13();
    test_p16();
    printf("----------------------------------------------------------------\n");
    printf("  manifestados: %d / %d testes dinamicos executados\n",
           g_manifested, g_run);
    printf("----------------------------------------------------------------\n");
    return g_manifested == g_run ? 0 : 1;
}
