#include "thread_activity.h"

#ifdef XPLATBASE_WIN


static LARGE_INTEGER g_qpc_freq;          /* counts per second */
static double        g_cycles_per_ns = 0.0; /* estimated; used only for normalization */
static LONG          g_initialized = 0;

/* -------------------------------------------------------------------------
 * Cycles to nanoseconds conversion is approximate on Windows because
 * CPU frequency varies (turbo boost, SpeedStep, etc). We don't need
 * absolute accuracy — only a consistent mapping so cpu_ratio is
 * comparable across platforms.
 *
 * Strategy: at init, measure how many cycles accumulate in a short
 * busy loop of known QPC duration. This gives a base frequency that's
 * "good enough" for ratio purposes.
 *
 * Alternative: hardcode assumption (e.g., 2.4 GHz). Less portable but
 * zero-calibration. We pick calibration for accuracy.
 * ------------------------------------------------------------------------- */
static void calibrate_cycles_per_ns(void)
{
    HANDLE self = GetCurrentThread();
    ULONG64 cycles_start, cycles_end;
    LARGE_INTEGER qpc_start, qpc_end;

    /* Pin to current core during calibration to avoid TSC drift between cores. */
    DWORD_PTR old_affinity = SetThreadAffinityMask(self, 1);

    QueryThreadCycleTime(self, &cycles_start);
    QueryPerformanceCounter(&qpc_start);

    /* Busy-wait ~10 ms */
    LARGE_INTEGER target;
    target.QuadPart = qpc_start.QuadPart + (g_qpc_freq.QuadPart / 100);
    LARGE_INTEGER now;
    do {
        QueryPerformanceCounter(&now);
    } while (now.QuadPart < target.QuadPart);

    QueryThreadCycleTime(self, &cycles_end);
    QueryPerformanceCounter(&qpc_end);

    if (old_affinity) SetThreadAffinityMask(self, old_affinity);

    uint64_t cycles_delta = cycles_end - cycles_start;
    uint64_t qpc_delta = qpc_end.QuadPart - qpc_start.QuadPart;
    double   ns_delta = ((double)qpc_delta * 1e9) / (double)g_qpc_freq.QuadPart;

    if (ns_delta > 0.0 && cycles_delta > 0) {
        g_cycles_per_ns = (double)cycles_delta / ns_delta;
    }
    else {
        g_cycles_per_ns = 2.4; /* fallback: assume 2.4 GHz */
    }
}

void xthread_activity_init(void) 
{
    if (InterlockedCompareExchange(&g_initialized, 1, 0) != 0) return;
    QueryPerformanceFrequency(&g_qpc_freq);
    calibrate_cycles_per_ns();
}

static inline uint64_t qpc_to_ns(LARGE_INTEGER c) 
{
    /* Avoid overflow: split the multiplication. */
    uint64_t secs = c.QuadPart / g_qpc_freq.QuadPart;
    uint64_t rem = c.QuadPart % g_qpc_freq.QuadPart;
    return secs * 1000000000ULL + (rem * 1000000000ULL) / g_qpc_freq.QuadPart;
}

xthread_sample_t xthread_sample_self(void) 
{
    xthread_sample_t s = { 0, 0 };
    LARGE_INTEGER qpc;
    ULONG64 cycles = 0;

    QueryPerformanceCounter(&qpc);
    QueryThreadCycleTime(GetCurrentThread(), &cycles);

    s.wall_ns = qpc_to_ns(qpc);
    s.activity = cycles;
    return s;
}

xthread_sample_t xthread_sample_of(xthread_handle_t h) 
{
    xthread_sample_t s = { 0, 0 };
    LARGE_INTEGER qpc;
    ULONG64 cycles = 0;

    QueryPerformanceCounter(&qpc);
    if (!QueryThreadCycleTime(h, &cycles)) 
    {
        /* thread may have exited or handle lacks rights */
        return s;
    }

    s.wall_ns = qpc_to_ns(qpc);
    s.activity = cycles;
    return s;
}


xtask_eval_t xthread_evaluate_task(const xthread_sample_t* start, const xthread_sample_t* now, const xtask_thresholds_t* t)
{
    xtask_eval_t eval = { XTASK_STATE_NORMAL, 0, 0, 0.0 };

    if (start->wall_ns == 0 || now->wall_ns <= start->wall_ns)
        return eval;

    eval.wall_elapsed_ns = now->wall_ns - start->wall_ns;
    eval.activity_delta  = now->activity - start->activity;

    double cpu_ns = (g_cycles_per_ns > 0.0)
        ? (double)eval.activity_delta / g_cycles_per_ns
        : 0.0;

    eval.cpu_ratio = cpu_ns / (double)eval.wall_elapsed_ns;
    if (eval.cpu_ratio > 1.0) eval.cpu_ratio = 1.0;

    if (eval.wall_elapsed_ns < t->long_threshold_ns)
        return eval;

    if (eval.cpu_ratio < t->blocked_ratio_max)
        eval.state = XTASK_STATE_LONG_BLOCKED;
    else if (eval.cpu_ratio >= t->cpu_ratio_min)
        eval.state = XTASK_STATE_LONG_CPU;
    else
        eval.state = XTASK_STATE_LONG_UNCLEAR;

    return eval;
}

bool xthread_should_handoff(const xtask_eval_t* eval)
{
    return eval->state == XTASK_STATE_LONG_BLOCKED;
}

#endif