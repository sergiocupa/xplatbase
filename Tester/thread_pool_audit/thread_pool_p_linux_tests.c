#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <x86intrin.h>

#include "thread_pool.h"

/* Satisfaz o construtor Linux da copia temporaria usada pelo teste. */
void platform_init(void) {}

static atomic_int fail_pthread_create;
static atomic_int pthread_create_calls;

int __real_pthread_create(pthread_t*, const pthread_attr_t*,
                          void* (*)(void*), void*);

int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                          void* (*start)(void*), void* arg)
{
    atomic_fetch_add(&pthread_create_calls, 1);
    if (atomic_load(&fail_pthread_create)) {
        /* POSIX leaves *thread undefined on failure. This legal nonzero value
           makes the ignored return code deterministic. */
        *thread = (pthread_t)(uintptr_t)0x1234;
        return EAGAIN;
    }
    return __real_pthread_create(thread, attr, start, arg);
}

static uint64_t monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int test_p05(void)
{
    struct timespec sleep_time = {0, 100000000L};
    uint64_t ns0;
    uint64_t ns1;
    uint64_t tsc0;
    uint64_t tsc1;
    double measured;
    double declared;

    declared = xthread_cycles_per_ns();
    ns0 = monotonic_ns();
    tsc0 = __rdtsc();
    nanosleep(&sleep_time, NULL);
    tsc1 = __rdtsc();
    ns1 = monotonic_ns();
    measured = (double)(tsc1 - tsc0) / (double)(ns1 - ns0);

    printf("P05 declared_cycles_per_ns=%.3f measured_tsc_per_ns=%.3f\n",
           declared, measured);
    return measured > declared * 1.20 || measured < declared * 0.80 ? 0 : 1;
}

static void p06_alarm(int signal_number)
{
    (void)signal_number;
    _exit(66);
}

static int test_p06(void)
{
    PoolConfig cfg = pool_default_config();
    ShardedPool* pool;

    signal(SIGALRM, p06_alarm);
    alarm(3);
    atomic_store(&fail_pthread_create, 1);
    cfg.shard_count = 1;
    cfg.max_shards = 1;
    cfg.reserve_size = 1;
    pool = pool_create(&cfg);
    alarm(0);

    printf("P06 pthread_create_calls=%d pool=%p\n",
           atomic_load(&pthread_create_calls), (void*)pool);
    if (pool) pool_shutdown(pool);
    return 1;
}

static atomic_int p07_started;
static atomic_ullong p07_heartbeat;

static void p07_busy(void* unused)
{
    (void)unused;
    atomic_store(&p07_started, 1);
    for (;;) atomic_fetch_add(&p07_heartbeat, 1);
}

static int test_p07(void)
{
    PoolConfig cfg = pool_default_config();
    ShardedPool* pool;
    unsigned long long before;
    unsigned long long after;
    int i;

    cfg.shard_count = 1;
    cfg.max_shards = 1;
    cfg.reserve_size = 1;
    cfg.shutdown_drain_timeout_ms = 20;
    cfg.shutdown_join_timeout_ms = 20;
    cfg.shutdown_force_kill = true;
    atomic_store(&p07_started, 0);
    atomic_store(&p07_heartbeat, 0);

    pool = pool_create(&cfg);
    if (!pool) return 2;
    pool_submit(pool, p07_busy, NULL);
    for (i = 0; i < 2000 && !atomic_load(&p07_started); i++) usleep(1000);
    if (!atomic_load(&p07_started)) return 3;

    pool_shutdown(pool);
    before = atomic_load(&p07_heartbeat);
    usleep(100000);
    after = atomic_load(&p07_heartbeat);
    printf("P07 heartbeat_after_shutdown=%llu->%llu\n", before, after);
    fflush(stdout);
    _Exit(after > before ? 0 : 1);
}

int main(int argc, char** argv)
{
    atomic_store(&fail_pthread_create, 0);
    atomic_store(&pthread_create_calls, 0);
    if (argc != 2) {
        fprintf(stderr, "usage: %s p05|p06|p07\n", argv[0]);
        return 2;
    }
    if (!strcmp(argv[1], "p05")) return test_p05();
    if (!strcmp(argv[1], "p06")) return test_p06();
    if (!strcmp(argv[1], "p07")) return test_p07();
    return 2;
}
