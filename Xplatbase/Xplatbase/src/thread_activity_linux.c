#include "thread_activity.h"

#ifndef XPLATBASE_WIN


#define _GNU_SOURCE
#include "xthread_activity.h"
#include <time.h>
#include <pthread.h>

void xthread_activity_init(void)
{
    /* Nothing to calibrate — clocks are already in ns. */
}

double xthread_cycles_per_ns(void)
{
    return 1.0;  /* Linux: activity ja e em ns, sem conversao necessaria */
}

static inline uint64_t ts_to_ns(struct timespec ts) 
{
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

xthread_sample_t xthread_sample_self(void) 
{
    xthread_sample_t s = { 0, 0 };
    struct timespec wall, cpu;

    clock_gettime(CLOCK_MONOTONIC, &wall);
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu);

    s.wall_ns = ts_to_ns(wall);
    s.activity = ts_to_ns(cpu);  /* CPU ns; same unit as wall */
    return s;
}

xthread_sample_t xthread_sample_of(xthread_handle_t h) 
{
    xthread_sample_t s = { 0, 0 };
    struct timespec wall, cpu;
    clockid_t cid;

    clock_gettime(CLOCK_MONOTONIC, &wall);

    if (pthread_getcpuclockid(h, &cid) != 0) return s;
    if (clock_gettime(cid, &cpu) != 0)       return s;

    s.wall_ns = ts_to_ns(wall);
    s.activity = ts_to_ns(cpu);
    return s;
}


xtask_eval_t xthread_evaluate_task(const xthread_sample_t* start, const xthread_sample_t* now, const xtask_thresholds_t* t)
{
    xtask_eval_t eval = { XTASK_STATE_NORMAL, 0, 0, 0.0 };

    if (start->wall_ns == 0 || now->wall_ns <= start->wall_ns)
        return eval;

    eval.wall_elapsed_ns = now->wall_ns - start->wall_ns;
    eval.activity_delta  = now->activity - start->activity;

    /* On Linux, activity is already CPU nanoseconds — direct ratio. */
    eval.cpu_ratio = (double)eval.activity_delta / (double)eval.wall_elapsed_ns;
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