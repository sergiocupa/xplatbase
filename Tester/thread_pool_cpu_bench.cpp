/*
 * thread_pool_cpu_bench.cpp
 *
 * Benchmark standalone para comparar:
 *   - atual-v16: Xplatbase thread_pool.c atual
 *   - WinTP    : Windows Thread Pool
 *   - TBB      : Intel oneTBB
 *
 * Mede latencia submit->inicio de execucao e uso medio de CPU do processo.
 * O uso de CPU e calculado via GetProcessTimes no intervalo do cenario:
 *
 *   cpu_cores = (kernel+user CPU seconds) / wall seconds
 *   cpu_pct_machine = cpu_cores / logical_processors * 100
 *
 * A execucao deve ser feita por adaptador/cenario para isolar a amostra de CPU.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "../Xplatbase/Xplatbase/src/thread_pool.h"
}

#ifdef TBB_AVAILABLE
#include <tbb/global_control.h>
#include <tbb/task_arena.h>
#include <tbb/task_group.h>
#endif

struct Scenario {
    const char* name;
    int tasks;
    int producers;
    int work_min_us;
    int work_max_us;
    int long_every;
    int long_us;
};

struct BenchTask {
    LARGE_INTEGER enqueue_qpc;
    LARGE_INTEGER start_qpc;
    int work_us;
    std::atomic<int>* done;
};

struct CpuSnap {
    uint64_t proc_100ns;
    LARGE_INTEGER qpc;
};

struct RunResult {
    const char* adapter;
    const char* scenario;
    int rep;
    int tasks;
    int workers;
    double wall_ms;
    double cpu_ms;
    double cpu_cores;
    double cpu_pct_machine;
    double p50_ms;
    double p99_ms;
    double p999_ms;
    double max_ms;
    double throughput_mtask_s;
    int timeout;
};

static LARGE_INTEGER g_qpf;

static LARGE_INTEGER qpc_now(void)
{
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return v;
}

static double qpc_delta_ms(LARGE_INTEGER a, LARGE_INTEGER b)
{
    return (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)g_qpf.QuadPart;
}

static uint64_t filetime_100ns(FILETIME ft)
{
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}

static CpuSnap cpu_snap(void)
{
    FILETIME create_ft, exit_ft, kernel_ft, user_ft;
    CpuSnap s{};
    GetProcessTimes(GetCurrentProcess(), &create_ft, &exit_ft, &kernel_ft, &user_ft);
    s.proc_100ns = filetime_100ns(kernel_ft) + filetime_100ns(user_ft);
    s.qpc = qpc_now();
    return s;
}

static int logical_processors(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors > 0 ? (int)si.dwNumberOfProcessors : 1;
}

static void precise_sleep_us(int us)
{
    if (us <= 0) return;
    LARGE_INTEGER start = qpc_now();
    double target_ms = (double)us / 1000.0;
    for (;;) {
        LARGE_INTEGER now = qpc_now();
        if (qpc_delta_ms(start, now) >= target_ms) return;
        YieldProcessor();
    }
}

static uint32_t lcg(uint32_t* state)
{
    *state = (*state * 1664525u) + 1013904223u;
    return *state;
}

static void fill_tasks(const Scenario& sc, std::vector<BenchTask>& tasks, std::atomic<int>& done)
{
    uint32_t rnd = 0x12345678u;
    for (int i = 0; i < sc.tasks; ++i) {
        int work = sc.work_min_us;
        if (sc.work_max_us > sc.work_min_us) {
            int span = sc.work_max_us - sc.work_min_us + 1;
            work = sc.work_min_us + (int)(lcg(&rnd) % (uint32_t)span);
        }
        if (sc.long_every > 0 && (i % sc.long_every) == 0)
            work = sc.long_us;
        tasks[i].work_us = work;
        tasks[i].done = &done;
        tasks[i].enqueue_qpc.QuadPart = 0;
        tasks[i].start_qpc.QuadPart = 0;
    }
}

static void task_body(BenchTask* t)
{
    t->start_qpc = qpc_now();
    precise_sleep_us(t->work_us);
    t->done->fetch_add(1, std::memory_order_release);
}

static void xp_task_fn(void* raw)
{
    task_body((BenchTask*)raw);
}

static VOID CALLBACK wintp_task_fn(PTP_CALLBACK_INSTANCE, void* raw, PTP_WORK)
{
    task_body((BenchTask*)raw);
}

static void compute_latency(std::vector<BenchTask>& tasks,
                            double* p50, double* p99, double* p999, double* maxv)
{
    std::vector<double> lat;
    lat.reserve(tasks.size());
    for (size_t i = 0; i < tasks.size(); ++i) {
        if (tasks[i].enqueue_qpc.QuadPart && tasks[i].start_qpc.QuadPart)
            lat.push_back(qpc_delta_ms(tasks[i].enqueue_qpc, tasks[i].start_qpc));
    }
    if (lat.empty()) {
        *p50 = *p99 = *p999 = *maxv = 0.0;
        return;
    }
    std::sort(lat.begin(), lat.end());
    auto pick = [&](double q) {
        size_t idx = (size_t)floor(q * (double)(lat.size() - 1));
        if (idx >= lat.size()) idx = lat.size() - 1;
        return lat[idx];
    };
    *p50 = pick(0.50);
    *p99 = pick(0.99);
    *p999 = pick(0.999);
    *maxv = lat.back();
}

static bool wait_done(std::atomic<int>& done, int expected, int timeout_ms)
{
    DWORD start = GetTickCount();
    while (done.load(std::memory_order_acquire) < expected) {
        if ((int)(GetTickCount() - start) > timeout_ms)
            return false;
        Sleep(1);
    }
    return true;
}

static RunResult finish_result(const char* adapter, const Scenario& sc, int rep, int workers,
                               const CpuSnap& c0, const CpuSnap& c1,
                               std::vector<BenchTask>& tasks, int timeout)
{
    RunResult r{};
    r.adapter = adapter;
    r.scenario = sc.name;
    r.rep = rep;
    r.tasks = sc.tasks;
    r.workers = workers;
    r.wall_ms = qpc_delta_ms(c0.qpc, c1.qpc);
    r.cpu_ms = (double)(c1.proc_100ns - c0.proc_100ns) / 10000.0;
    r.cpu_cores = r.wall_ms > 0.0 ? r.cpu_ms / r.wall_ms : 0.0;
    r.cpu_pct_machine = r.cpu_cores * 100.0 / (double)logical_processors();
    r.throughput_mtask_s = r.wall_ms > 0.0 ? ((double)sc.tasks / (r.wall_ms / 1000.0)) / 1000000.0 : 0.0;
    r.timeout = timeout;
    compute_latency(tasks, &r.p50_ms, &r.p99_ms, &r.p999_ms, &r.max_ms);
    return r;
}

static RunResult run_xplat(const Scenario& sc, int rep, int workers)
{
    std::atomic<int> done{0};
    std::vector<BenchTask> tasks((size_t)sc.tasks);
    fill_tasks(sc, tasks, done);

    PoolConfig cfg = pool_default_config();
    cfg.shard_count = workers;
    cfg.max_shards = workers;
    cfg.max_auto_expand_lanes = workers;
    cfg.ring_capacity = 65536;
    cfg.reserve_size = 0;
    ShardedPool* pool = pool_create(&cfg);
    pool_init(pool);

    CpuSnap c0 = cpu_snap();
    for (int i = 0; i < sc.tasks; ++i) {
        tasks[i].enqueue_qpc = qpc_now();
        while (!pool_submit(pool, xp_task_fn, &tasks[i]))
            SwitchToThread();
    }
    int timeout = wait_done(done, sc.tasks, 120000) ? 0 : 1;
    CpuSnap c1 = cpu_snap();

    pool_shutdown(pool);
    pool_destroy(pool);
    return finish_result("atual-v16", sc, rep, workers, c0, c1, tasks, timeout);
}

static RunResult run_wintp(const Scenario& sc, int rep, int workers)
{
    std::atomic<int> done{0};
    std::vector<BenchTask> tasks((size_t)sc.tasks);
    std::vector<PTP_WORK> works((size_t)sc.tasks);
    fill_tasks(sc, tasks, done);

    PTP_POOL pool = CreateThreadpool(NULL);
    SetThreadpoolThreadMaximum(pool, (DWORD)workers);
    SetThreadpoolThreadMinimum(pool, (DWORD)workers);
    TP_CALLBACK_ENVIRON env;
    InitializeThreadpoolEnvironment(&env);
    SetThreadpoolCallbackPool(&env, pool);

    CpuSnap c0 = cpu_snap();
    for (int i = 0; i < sc.tasks; ++i) {
        tasks[i].enqueue_qpc = qpc_now();
        works[i] = CreateThreadpoolWork(wintp_task_fn, &tasks[i], &env);
        SubmitThreadpoolWork(works[i]);
    }
    int timeout = wait_done(done, sc.tasks, 120000) ? 0 : 1;
    CpuSnap c1 = cpu_snap();

    for (int i = 0; i < sc.tasks; ++i) {
        if (works[i]) {
            WaitForThreadpoolWorkCallbacks(works[i], FALSE);
            CloseThreadpoolWork(works[i]);
        }
    }
    DestroyThreadpoolEnvironment(&env);
    CloseThreadpool(pool);
    return finish_result("WinTP", sc, rep, workers, c0, c1, tasks, timeout);
}

static RunResult run_tbb(const Scenario& sc, int rep, int workers)
{
#ifndef TBB_AVAILABLE
    (void)sc; (void)rep; (void)workers;
    fprintf(stderr, "TBB_AVAILABLE nao definido no build.\n");
    exit(2);
#else
    std::atomic<int> done{0};
    std::vector<BenchTask> tasks((size_t)sc.tasks);
    fill_tasks(sc, tasks, done);

    tbb::global_control gc(tbb::global_control::max_allowed_parallelism, (size_t)workers);
    tbb::task_arena arena(workers);
    arena.initialize();

    CpuSnap c0 = cpu_snap();
    for (int i = 0; i < sc.tasks; ++i) {
        tasks[i].enqueue_qpc = qpc_now();
        arena.enqueue([&, i]() { task_body(&tasks[i]); });
    }
    int timeout = wait_done(done, sc.tasks, 120000) ? 0 : 1;
    CpuSnap c1 = cpu_snap();
    return finish_result("TBB", sc, rep, workers, c0, c1, tasks, timeout);
#endif
}

static Scenario scenarios[] = {
    {"rapida/baixa",          20000,  1,  0,  0,   0,   0},
    {"rapida/media",          60000,  4,  0,  0,   0,   0},
    {"rapida/alta",          120000,  8,  0,  0,   0,   0},
    {"rapida/ultra",         180000, 16,  0,  0,   0,   0},
    {"media/baixa",           20000,  1, 15, 25,   0,   0},
    {"media/media",           60000,  4, 15, 35,   0,   0},
    {"media/alta",           120000,  8, 15, 45,   0,   0},
    {"media/ultra",          180000, 16, 15, 55,   0,   0},
    {"mista/baixa",           20000,  1,  0,  5,  16, 250},
    {"mista/media",           60000,  4,  0,  5,  16, 300},
    {"mista/alta",           120000,  8,  0,  5,  16, 350},
    {"mista/ultra",          180000, 16,  0,  5,  16, 400},
    {"satur/alta",           160000, 16,  0,  0,   0,   0},
    {"satur/ultra",          240000, 32,  0,  0,   0,   0},
    {"rajada-curta/media",    60000, 16,  0,  0,   0,   0},
    {"rajada-mista/media",    60000, 16,  0,  5,  12, 300},
    {"mista-massiva/alta",   180000, 16,  0,  8,  24, 500},
    {"mista-massiva/ultra",  240000, 32,  0,  8,  24, 600},
};

static const Scenario* find_scenario(const char* name)
{
    for (size_t i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); ++i)
        if (strcmp(scenarios[i].name, name) == 0)
            return &scenarios[i];
    return NULL;
}

static double median(std::vector<double> v)
{
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

static RunResult summarize(const std::vector<RunResult>& reps)
{
    RunResult s = reps[0];
    std::vector<double> wall, cpu_ms, cpu_cores, cpu_pct, p50, p99, p999, maxv, thr;
    int timeout = 0;
    for (const RunResult& r : reps) {
        wall.push_back(r.wall_ms);
        cpu_ms.push_back(r.cpu_ms);
        cpu_cores.push_back(r.cpu_cores);
        cpu_pct.push_back(r.cpu_pct_machine);
        p50.push_back(r.p50_ms);
        p99.push_back(r.p99_ms);
        p999.push_back(r.p999_ms);
        maxv.push_back(r.max_ms);
        thr.push_back(r.throughput_mtask_s);
        timeout += r.timeout;
    }
    s.rep = (int)reps.size();
    s.wall_ms = median(wall);
    s.cpu_ms = median(cpu_ms);
    s.cpu_cores = median(cpu_cores);
    s.cpu_pct_machine = median(cpu_pct);
    s.p50_ms = median(p50);
    s.p99_ms = median(p99);
    s.p999_ms = median(p999);
    s.max_ms = median(maxv);
    s.throughput_mtask_s = median(thr);
    s.timeout = timeout;
    return s;
}

static void write_csv_row(const char* path, const RunResult& r)
{
    bool exists = false;
    DWORD attr = GetFileAttributesA(path);
    exists = (attr != INVALID_FILE_ATTRIBUTES);
    FILE* f = fopen(path, "ab");
    if (!f) {
        fprintf(stderr, "Nao consegui abrir CSV: %s\n", path);
        exit(3);
    }
    if (!exists) {
        fprintf(f, "adapter,scenario,reps,tasks,workers,wall_ms,cpu_ms,cpu_cores,cpu_pct_machine,p50_ms,p99_ms,p999_ms,max_ms,throughput_mtask_s,timeouts\n");
    }
    fprintf(f, "%s,%s,%d,%d,%d,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%d\n",
            r.adapter, r.scenario, r.rep, r.tasks, r.workers,
            r.wall_ms, r.cpu_ms, r.cpu_cores, r.cpu_pct_machine,
            r.p50_ms, r.p99_ms, r.p999_ms, r.max_ms, r.throughput_mtask_s,
            r.timeout);
    fclose(f);
}

static void print_row(const RunResult& r)
{
    printf("%-10s %-22s reps=%d tasks=%d cpu=%.2f cores %.2f%% wall=%.1fms p50=%.6f p99=%.6f p999=%.6f max=%.6f thr=%.6f timeout=%d\n",
           r.adapter, r.scenario, r.rep, r.tasks, r.cpu_cores, r.cpu_pct_machine,
           r.wall_ms, r.p50_ms, r.p99_ms, r.p999_ms, r.max_ms,
           r.throughput_mtask_s, r.timeout);
}

static void usage(void)
{
    printf("Uso:\n");
    printf("  thread_pool_cpu_bench.exe --adapter atual-v16|WinTP|TBB --scenario nome --reps N --csv arquivo.csv [--workers N]\n");
    printf("Cenarios:\n");
    for (size_t i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); ++i)
        printf("  %s\n", scenarios[i].name);
}

int main(int argc, char** argv)
{
    QueryPerformanceFrequency(&g_qpf);
    const char* adapter = NULL;
    const char* scenario_name = NULL;
    const char* csv = "thread_pool_cpu_bench_results.csv";
    int reps = 3;
    int workers = logical_processors();

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--adapter") == 0 && i + 1 < argc) adapter = argv[++i];
        else if (strcmp(argv[i], "--scenario") == 0 && i + 1 < argc) scenario_name = argv[++i];
        else if (strcmp(argv[i], "--reps") == 0 && i + 1 < argc) reps = atoi(argv[++i]);
        else if (strcmp(argv[i], "--csv") == 0 && i + 1 < argc) csv = argv[++i];
        else if (strcmp(argv[i], "--workers") == 0 && i + 1 < argc) workers = atoi(argv[++i]);
        else if (strcmp(argv[i], "--help") == 0) { usage(); return 0; }
        else { usage(); return 1; }
    }

    if (!adapter || !scenario_name || reps < 1 || workers < 1) {
        usage();
        return 1;
    }

    const Scenario* sc = find_scenario(scenario_name);
    if (!sc) {
        fprintf(stderr, "Cenario desconhecido: %s\n", scenario_name);
        usage();
        return 1;
    }

    std::vector<RunResult> rr;
    rr.reserve((size_t)reps);
    for (int rep = 0; rep < reps; ++rep) {
        RunResult r{};
        if (strcmp(adapter, "atual-v16") == 0 || strcmp(adapter, "xplat") == 0)
            r = run_xplat(*sc, rep, workers);
        else if (strcmp(adapter, "WinTP") == 0 || strcmp(adapter, "wintp") == 0)
            r = run_wintp(*sc, rep, workers);
        else if (strcmp(adapter, "TBB") == 0 || strcmp(adapter, "tbb") == 0)
            r = run_tbb(*sc, rep, workers);
        else {
            fprintf(stderr, "Adapter desconhecido: %s\n", adapter);
            return 1;
        }
        print_row(r);
        rr.push_back(r);
        Sleep(250);
    }

    RunResult s = summarize(rr);
    write_csv_row(csv, s);
    printf("CSV atualizado: %s\n", csv);
    return s.timeout ? 4 : 0;
}
