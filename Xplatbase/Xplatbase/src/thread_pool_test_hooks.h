#ifndef THREAD_POOL_TEST_HOOKS_H
#define THREAD_POOL_TEST_HOOKS_H

#ifdef POOL_TEST_HOOKS

#include "thread_pool.h"

void pool_test_fail_alloc_after(int successful_allocations);
void pool_test_fail_thread_start_after(int successful_starts);
void pool_test_fail_reserve_return_after(int successful_returns);
void ring_queue_test_fail_alloc_after(int successful_allocations);

void pool_test_set_handoff_barrier(int target);

void pool_test_enable_lane_release_barrier(void);
int  pool_test_wait_lane_release_barrier(int timeout_ms);
void pool_test_release_lane_barrier(void);

void pool_test_enable_orphan_window_barrier(void);
int  pool_test_wait_orphan_window_arrived(int timeout_ms);
void pool_test_release_orphan_window_barrier(void);

int pool_test_activate_next_lane(ShardedPool* pool);
int pool_test_orphan_lane_count(ShardedPool* pool);
int pool_test_orphan_transition_observed(void);

/* Janela de dono-duplo (BUG 01) — barreiras coordenadas + detector. */
void pool_test_enable_double_owner_window(void);
int  pool_test_wait_double_owner_owner(int timeout_ms);
int  pool_test_wait_double_owner_activate(int timeout_ms);
void pool_test_release_double_owner_owner(void);
void pool_test_release_double_owner_activate(void);
int  pool_test_max_owners_per_lane(ShardedPool* pool);
void pool_test_dump_workers(ShardedPool* pool);
void pool_test_freeze_autoscale(int on);

void pool_test_reset_spin_observation(void);
int  pool_test_max_spin_iterations_observed(void);

#endif

#endif
