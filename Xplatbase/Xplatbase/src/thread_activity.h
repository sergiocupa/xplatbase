#ifndef XTHREAD_ACTIVITY_H
#define XTHREAD_ACTIVITY_H

#include "../include/xplatbase.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef XPLATBASE_WIN
#include <windows.h>
typedef HANDLE xthread_handle_t;
#else
#include <pthread.h>
typedef pthread_t xthread_handle_t;
#endif


typedef struct 
{
    uint64_t wall_ns;      /* wall-clock time, nanoseconds */
    uint64_t activity;     /* platform-specific: cycles (Win) or CPU ns (Linux) */
} 
xthread_sample_t;


typedef enum 
{
    XTASK_STATE_NORMAL,     
    XTASK_STATE_LONG_CPU, 
    XTASK_STATE_LONG_BLOCKED,  
    XTASK_STATE_LONG_UNCLEAR  
} 
xtask_state_t;


typedef struct 
{
    xtask_state_t state;
    uint64_t      wall_elapsed_ns;
    uint64_t      activity_delta;
    double        cpu_ratio;      /* 0.0 .. 1.0 (normalized across platforms) */
} 
xtask_eval_t;


typedef struct 
{
    uint64_t long_threshold_ns;   /* task considered "long" beyond this */
    double   blocked_ratio_max;   /* cpu_ratio below this => BLOCKED */
    double   cpu_ratio_min;       /* cpu_ratio above this => CPU_BOUND */
} 
xtask_thresholds_t;


/* Sensible defaults. */
#define XTASK_DEFAULT_LONG_NS        ((uint64_t)30 * 1000 * 1000)  /* 30 ms */
#define XTASK_DEFAULT_BLOCKED_RATIO  0.30
#define XTASK_DEFAULT_CPU_RATIO      0.70



void xthread_activity_init(void);
xthread_sample_t xthread_sample_self(void);
xthread_sample_t xthread_sample_of(xthread_handle_t h);
xtask_eval_t xthread_evaluate_task(const xthread_sample_t* start, const xthread_sample_t* now, const xtask_thresholds_t* t);
bool xthread_should_handoff(const xtask_eval_t* eval);
    


#ifdef __cplusplus
}
#endif

#endif /* XTHREAD_ACTIVITY_H */
