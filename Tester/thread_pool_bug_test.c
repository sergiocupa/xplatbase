#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "../Xplatbase/Xplatbase/src/thread_pool.h"
#include "../Xplatbase/Xplatbase/src/thread_pool_test_hooks.h"

#ifdef POOL_TEST_HOOKS

typedef struct {
    int passed;
    int total;
} BugTestSummary;

static BugTestSummary g_bug_summary;

static void bug_result(int id, int healthy, const char* name, const char* detail)
{
    g_bug_summary.total++;
    if (healthy) {
        g_bug_summary.passed++;
        printf("  [CORRIGIDO] BUG %02d - %s: %s\n", id, name, detail);
    } else {
        printf("  [FALHOU] BUG %02d - %s: %s\n", id, name, detail);
    }
}

static int wait_long(volatile LONG* value, LONG target, int timeout_ms)
{
    int elapsed = 0;
    while (InterlockedCompareExchange(value, 0, 0) < target) {
        if (elapsed >= timeout_ms) return 0;
        Sleep(1);
        elapsed++;
    }
    return 1;
}

typedef struct {
    int started;
    int timed_out;
    DWORD exit_code;
} ChildResult;

static ChildResult run_child(const char* mode, DWORD timeout_ms)
{
    ChildResult result;
    memset(&result, 0, sizeof(result));

    char exe[MAX_PATH];
    char command[MAX_PATH + 128];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);

    if (!GetModuleFileNameA(NULL, exe, MAX_PATH)) return result;
    snprintf(command, sizeof(command), "\"%s\" --bug-child %s", exe, mode);

    if (!CreateProcessA(NULL, command, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi))
        return result;

    result.started = 1;
    DWORD wait = WaitForSingleObject(pi.hProcess, timeout_ms);
    if (wait == WAIT_TIMEOUT) {
        result.timed_out = 1;
        TerminateProcess(pi.hProcess, 0xEE);
        WaitForSingleObject(pi.hProcess, 2000);
    }
    GetExitCodeProcess(pi.hProcess, &result.exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return result;
}

static volatile LONG g_long_started;
static volatile LONG g_long_done;
static volatile LONG g_short_done;

static void sleep_task_100ms(void* unused)
{
    (void)unused;
    InterlockedExchange(&g_long_started, 1);
    Sleep(100);
    InterlockedIncrement(&g_long_done);
}

static void sleep_task_200ms(void* unused)
{
    (void)unused;
    InterlockedExchange(&g_long_started, 1);
    Sleep(200);
    InterlockedIncrement(&g_long_done);
}

static void fast_count_task(void* unused)
{
    (void)unused;
    InterlockedIncrement(&g_short_done);
}

static void cpu_task_150ms(void* unused)
{
    (void)unused;
    InterlockedExchange(&g_long_started, 1);
    ULONGLONG deadline = GetTickCount64() + 150;
    volatile uint64_t value = 1;
    while (GetTickCount64() < deadline)
        value = value * 1664525u + 1013904223u;
    (void)value;
    InterlockedIncrement(&g_long_done);
}

typedef struct {
    ShardedPool* pool;
    HANDLE gate;
} HandoffSubmitter;

static DWORD WINAPI handoff_submitter(void* raw)
{
    HandoffSubmitter* submitter = (HandoffSubmitter*)raw;
    WaitForSingleObject(submitter->gate, INFINITE);
    pool_submit(submitter->pool, fast_count_task, NULL);
    return 0;
}

static void test_bug_01_duplicate_handoff(void)
{
    enum { SUBMITTERS = 32 };
    PoolConfig cfg = pool_default_config();
    cfg.shard_count = 1;
    cfg.max_shards = 1;
    cfg.max_auto_expand_lanes = 1;
    cfg.reserve_size = SUBMITTERS + 4;
    cfg.long_task_threshold_ns = 1000000ULL;
    cfg.rescue_backlog_threshold = 1000000;
    cfg.spin_budget_us = 0;

    ShardedPool* pool = pool_create(&cfg);
    if (!pool) {
        bug_result(1, 1, "handoff duplicado", "pool_create falhou");
        return;
    }

    g_long_started = 0;
    g_long_done = 0;
    g_short_done = 0;
    pool_submit(pool, sleep_task_100ms, NULL);
    wait_long(&g_long_started, 1, 2000);
    Sleep(5);

    pool_test_set_handoff_barrier(2);
    HANDLE gate = CreateEventW(NULL, TRUE, FALSE, NULL);
    HANDLE threads[SUBMITTERS];
    HandoffSubmitter submitters[SUBMITTERS];

    for (int i = 0; i < SUBMITTERS; i++) {
        submitters[i].pool = pool;
        submitters[i].gate = gate;
        threads[i] = CreateThread(NULL, 0, handoff_submitter, &submitters[i], 0, NULL);
    }
    SetEvent(gate);
    WaitForMultipleObjects(SUBMITTERS, threads, TRUE, INFINITE);
    for (int i = 0; i < SUBMITTERS; i++) CloseHandle(threads[i]);
    CloseHandle(gate);

    wait_long(&g_short_done, SUBMITTERS, 5000);
    wait_long(&g_long_done, 1, 5000);

    PoolStats stats;
    pool_stats(pool, &stats);
    char detail[128];
    snprintf(detail, sizeof(detail), "handoffs=%llu active_workers=%d",
             (unsigned long long)stats.total_handoffs, stats.active_worker_count);
    bug_result(1, stats.total_handoffs <= 1 && stats.active_worker_count <= 1,
               "handoff duplicado", detail);
    pool_shutdown(pool);
}

static void test_bug_02_partial_init_shutdown(void)
{
    ChildResult child = run_child("partial-init", 3000);
    char detail[128];
    snprintf(detail, sizeof(detail), "started=%d timeout=%d exit=0x%08lx",
             child.started, child.timed_out, (unsigned long)child.exit_code);
    bug_result(2, child.started && !child.timed_out && child.exit_code == 0,
               "shutdown em inicializacao parcial", detail);
}

static void test_bug_03_orphan_lane(void)
{
    PoolConfig cfg = pool_default_config();
    cfg.shard_count = 1;
    cfg.max_shards = 2;
    cfg.max_auto_expand_lanes = 2;
    cfg.reserve_size = 2;
    cfg.ring_capacity = 32;
    cfg.park_idle_threshold_ms = 0;

    ShardedPool* pool = pool_create(&cfg);
    if (!pool) {
        bug_result(3, 1, "lane orfa", "pool_create falhou");
        return;
    }

    int expanded = pool_test_activate_next_lane(pool);
    pool_test_enable_lane_release_barrier();
    int owner_waiting = pool_test_wait_lane_release_barrier(3000);
    int reactivated = owner_waiting ? pool_test_activate_next_lane(pool) : 0;
    pool_test_release_lane_barrier();
    Sleep(10);

    int transition = pool_test_orphan_transition_observed();
    int orphans = pool_test_orphan_lane_count(pool);
    char detail[128];
    snprintf(detail, sizeof(detail),
             "expanded=%d owner_waiting=%d reactivated=%d transition=%d orphans_agora=%d",
             expanded, owner_waiting, reactivated, transition, orphans);
    bug_result(3, expanded && owner_waiting && reactivated && !transition,
               "lane orfa na contracao/reativacao", detail);
    pool_shutdown(pool);
}

static void test_bug_04_invalid_ring_capacity(void)
{
    PoolConfig cfg = pool_default_config();
    cfg.shard_count = 1;
    cfg.max_shards = 1;
    cfg.ring_capacity = 3;

    ShardedPool* pool = pool_create(&cfg);
    bug_result(4, pool == NULL, "ring_capacity invalida",
               pool ? "capacidade 3 foi aceita" : "capacidade 3 foi rejeitada");
    if (pool) pool_shutdown(pool);
}

static void test_bug_05_shutdown_drops_tasks(void)
{
    PoolConfig cfg = pool_default_config();
    cfg.shard_count = 1;
    cfg.max_shards = 1;
    cfg.max_auto_expand_lanes = 1;
    cfg.ring_capacity = 32;
    cfg.rescue_backlog_threshold = 1000000;
    cfg.long_task_threshold_ns = 10000000000ULL;

    ShardedPool* pool = pool_create(&cfg);
    if (!pool) {
        bug_result(5, 1, "shutdown descarta tasks", "pool_create falhou");
        return;
    }

    g_long_started = 0;
    g_long_done = 0;
    g_short_done = 0;
    pool_submit(pool, sleep_task_100ms, NULL);
    wait_long(&g_long_started, 1, 2000);
    for (int i = 0; i < 10; i++) pool_submit(pool, fast_count_task, NULL);
    pool_shutdown(pool);

    LONG completed = InterlockedCompareExchange(&g_long_done, 0, 0) +
                     InterlockedCompareExchange(&g_short_done, 0, 0);
    char detail[96];
    snprintf(detail, sizeof(detail), "executadas=%ld aceitas=11", completed);
    bug_result(5, completed == 11, "shutdown descarta tasks aceitas", detail);
}

static void test_bug_06_thread_creation_failure(void)
{
    ChildResult child = run_child("thread-start", 3000);
    char detail[128];
    snprintf(detail, sizeof(detail), "started=%d timeout=%d exit=0x%08lx",
             child.started, child.timed_out, (unsigned long)child.exit_code);
    bug_result(6, child.started && !child.timed_out && child.exit_code == 0,
               "falha de CreateThread nao tratada", detail);
}

static void test_bug_07_seqno_allocation_failure(void)
{
    ChildResult child = run_child("seqno-allocation", 3000);
    char detail[128];
    snprintf(detail, sizeof(detail), "started=%d timeout=%d exit=0x%08lx",
             child.started, child.timed_out, (unsigned long)child.exit_code);
    bug_result(7, child.started && !child.timed_out && child.exit_code == 0,
               "falha de malloc do seqno", detail);
}

static void test_bug_08_monitor_interval_ignored(void)
{
    PoolConfig cfg = pool_default_config();
    cfg.shard_count = 1;
    cfg.max_shards = 1;
    cfg.max_auto_expand_lanes = 1;
    cfg.reserve_size = 2;
    cfg.monitor_interval_ms = 1000;
    cfg.long_task_threshold_ns = 5000000ULL;
    cfg.rescue_backlog_threshold = 1000000;

    ShardedPool* pool = pool_create(&cfg);
    if (!pool) {
        bug_result(8, 1, "monitor_interval_ms ignorado", "pool_create falhou");
        return;
    }

    g_long_started = 0;
    g_long_done = 0;
    pool_submit(pool, sleep_task_200ms, NULL);
    wait_long(&g_long_started, 1, 2000);
    Sleep(100);

    PoolStats stats;
    pool_stats(pool, &stats);
    char detail[96];
    snprintf(detail, sizeof(detail), "intervalo=1000ms handoffs_em_100ms=%llu",
             (unsigned long long)stats.total_handoffs);
    bug_result(8, stats.total_handoffs == 0, "monitor_interval_ms ignorado", detail);
    pool_shutdown(pool);
}

static void test_bug_09_spin_iterations_ignored(void)
{
    PoolConfig cfg = pool_default_config();
    cfg.shard_count = 1;
    cfg.max_shards = 1;
    cfg.max_auto_expand_lanes = 1;
    cfg.spin_budget_us = 0;
    cfg.spin_iterations = 1;
    cfg.park_idle_threshold_ms = 0;

    ShardedPool* pool = pool_create(&cfg);
    if (!pool) {
        bug_result(9, 1, "spin_iterations ignorado", "pool_create falhou");
        return;
    }

    pool_test_reset_spin_observation();
    Sleep(30);
    int observed = pool_test_max_spin_iterations_observed();
    char detail[96];
    snprintf(detail, sizeof(detail), "configurado=1 observado=%d", observed);
    bug_result(9, observed <= 1, "spin_iterations ignorado", detail);
    pool_shutdown(pool);
}

static void test_bug_10_task_thresholds_ignored(void)
{
    PoolConfig cfg = pool_default_config();
    cfg.shard_count = 1;
    cfg.max_shards = 1;
    cfg.max_auto_expand_lanes = 1;
    cfg.reserve_size = 2;
    cfg.long_task_threshold_ns = 5000000ULL;
    cfg.rescue_backlog_threshold = 1000000;
    cfg.task_thresholds_set = true;
    cfg.task_thresholds.long_threshold_ns = 5000000ULL;
    cfg.task_thresholds.blocked_ratio_max = 0.20;
    cfg.task_thresholds.cpu_ratio_min = 0.70;

    ShardedPool* pool = pool_create(&cfg);
    if (!pool) {
        bug_result(10, 1, "task_thresholds ignorado", "pool_create falhou");
        return;
    }

    g_long_started = 0;
    g_long_done = 0;
    pool_submit(pool, cpu_task_150ms, NULL);
    wait_long(&g_long_started, 1, 2000);
    Sleep(80);

    PoolStats stats;
    pool_stats(pool, &stats);
    char detail[96];
    snprintf(detail, sizeof(detail), "task CPU-bound handoffs=%llu",
             (unsigned long long)stats.total_handoffs);
    bug_result(10, stats.total_handoffs == 0, "task_thresholds ignorado", detail);
    pool_shutdown(pool);
}

static int child_partial_init(void)
{
    pool_test_fail_alloc_after(2);
    PoolConfig cfg = pool_default_config();
    cfg.shard_count = 1;
    cfg.max_shards = 2;
    ShardedPool* pool = pool_create(&cfg);
    if (pool) {
        pool_shutdown(pool);
        return 1;
    }
    return 0;
}

static int child_thread_start(void)
{
    pool_test_fail_thread_start_after(0);
    PoolConfig cfg = pool_default_config();
    cfg.shard_count = 1;
    cfg.max_shards = 1;
    cfg.reserve_size = 1;
    ShardedPool* pool = pool_create(&cfg);
    if (pool) {
        pool_shutdown(pool);
        return 1;
    }
    return 0;
}

static int child_seqno_allocation(void)
{
    ring_queue_test_fail_alloc_after(0);
    PoolConfig cfg = pool_default_config();
    cfg.shard_count = 1;
    cfg.max_shards = 1;
    ShardedPool* pool = pool_create(&cfg);
    if (pool) {
        pool_shutdown(pool);
        return 1;
    }
    return 0;
}

static int run_child_mode(const char* mode)
{
    if (strcmp(mode, "partial-init") == 0) return child_partial_init();
    if (strcmp(mode, "thread-start") == 0) return child_thread_start();
    if (strcmp(mode, "seqno-allocation") == 0) return child_seqno_allocation();
    return 2;
}

static int run_bug_tests(void)
{
    memset(&g_bug_summary, 0, sizeof(g_bug_summary));
    printf("\n");
    printf("================================================================================\n");
    printf("  THREAD POOL - REGRESSAO EXCLUSIVA DAS 10 CORRECOES\n");
    printf("================================================================================\n");

    test_bug_01_duplicate_handoff();
    test_bug_02_partial_init_shutdown();
    test_bug_03_orphan_lane();
    test_bug_04_invalid_ring_capacity();
    test_bug_05_shutdown_drops_tasks();
    test_bug_06_thread_creation_failure();
    test_bug_07_seqno_allocation_failure();
    test_bug_08_monitor_interval_ignored();
    test_bug_09_spin_iterations_ignored();
    test_bug_10_task_thresholds_ignored();

    printf("================================================================================\n");
    printf("  CORRECOES CONFIRMADAS: %d / %d\n",
           g_bug_summary.passed, g_bug_summary.total);
    printf("================================================================================\n\n");
    return g_bug_summary.passed == g_bug_summary.total ? 0 : 1;
}

int thread_pool_bug_test_dispatch(int argc, char** argv)
{
    if (argc >= 3 && strcmp(argv[1], "--bug-child") == 0)
        return run_child_mode(argv[2]);
    if (argc >= 2 && strcmp(argv[1], "--bugs") == 0)
        return run_bug_tests();

    return 2;
}

#endif
