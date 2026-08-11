# Xplatbase

A **C (C11)** library with a set of base types and fundamental functions for
**cross-platform** use (Windows / Linux, x86-64 and ARM). It centralizes memory
allocation, fault handling, thread creation and synchronization, lock-free data
structures, and a high-performance task pool.

It is the foundation layer other components are built on: no external
dependencies, auto-initialized on module load, and a stable API exported via
`XPLATBASE_API`.

> License: MIT with mandatory attribution — © 2025 Sergio Paludo
> ([github.com/sergiocupa](https://github.com/sergiocupa)).

---

## Contents

- [Initialization](#initialization)
- [Highlights](#highlights)
- [🧵 Thread Pool](#-thread-pool)
- [🧠 Memory Pool](#-memory-pool)
- [🔎 Mem Leak Watch](#-mem-leak-watch)
- [Supporting modules](#supporting-modules)
- [Base types](#base-types)
- [Techniques and credits](#techniques-and-credits)

---

## Initialization

The library **auto-initializes** on module load — you don't need to call
anything before using it. The entry point is `platform_init()`, registered to run
automatically:

- **Windows/MSVC**: pointer in `.CRT$XCU` (before `main`).
- **Linux/GCC-Clang**: `__attribute__((constructor))`.

`platform_init()` validates UTF-8, brings up the event system, initializes the
[memory pool](#-memory-pool), registers the thread life-cycle *hooks*, creates
the global [thread pool](#-thread-pool), and starts the
[mem leak watch](#-mem-leak-watch) with defaults.

```c
#include "xplatbase.h"
#include "thread_pool.h"
#include "memory_pool.h"

static void task(void* arg) { /* ... */ }

int main(void)
{
    /* no setup needed: the module already brought everything up on load */
    void* p = memop_alloc_raw(128);
    pool_submit(task, p);
    pool_wait_idle();
    memop_free_raw(p);
    return 0;
}
```

To **disable auto-initialization** and call `platform_init()` manually, define
`XPLATBASE_NO_AUTO_INIT` at compile time.

| Function | Description |
|---|---|
| `void platform_init(void)` | Initializes the platform (idempotent). Auto-called on load unless `XPLATBASE_NO_AUTO_INIT`. |

---

## Highlights

Three components hold most of the library's engineering work and have a
**complete benchmark** against industry references:

| Component | What it is | Compared against |
|---|---|---|
| [**Thread Pool**](#-thread-pool) | Work-stealing task pool with core/reserve and elastic workers | Intel TBB, Windows Thread Pool |
| [**Memory Pool**](#-memory-pool) | *Size-class* allocator with per-thread lanes and a global cache | rpmalloc, mimalloc, CRT malloc |
| [**Mem Leak Watch**](#-mem-leak-watch) | Leak monitor by reachability (GC-style, without collecting) | — (cost measured over the memory pool) |

All charts below are medians of multiple runs (thread pool: 10 reps; memory pool:
9 runs), on a 16-CPU host.

---

## 🧵 Thread Pool

### How it works

A *run-to-completion* task pool with **work-stealing** scheduling in an arena
style (consolidated design V2.05).

![Thread pool architecture](docs/img/arch_thread_pool.svg)

- **External submit** → *G* shared MPMC queues (`shards = cores/4`), round-robin.
  Any worker pulls from any shard; the consumer reserves up to `POOL_BATCH` tasks
  with a single CAS on `head`.
- **Spawn** (submit from inside a task) → the worker's **non-stealable LIFO
  slot** (the child runs next, cache-hot), overflowing into the **local Chase-Lev
  deque** (push/take without CAS). Workers steal between deques.
- **Core / reserve**: `cores*7/10` workers spin and are woken by submit; the rest
  are *park-first* and only engage under backlog → flat ~75% CPU with a low tail.
- **Elastic worker**: a monitor detects workers stuck in long tasks and wakes
  extras to drain the short ones; they retire when the load passes.

### API

```c
#include "thread_pool.h"
typedef void (*pool_task_fn)(void*);
```

| Function | Description |
|---|---|
| `boolean pool_submit(pool_task_fn fn, void* arg)` | Enqueues a task on the global pool. `false` if rejected. |
| `void pool_wait_idle(void)` | Blocks until all pending tasks drain. |
| `void pool_dims(int* workers, int* core)` | Returns the number of workers and active cores. |
| `ThreadPool* pool_create_relative(int cores_override)` | Creates a private pool (isolated from the global one). |
| `void pool_destroy_relative(ThreadPool* p)` | Destroys a pool created above. |
| `boolean pool_submit_relative(ThreadPool* p, pool_task_fn fn, void* arg)` | Submit to a specific pool. |
| `void pool_wait_idle_relative(ThreadPool* p)` | Drains a specific pool. |

Tunables via `-D`: `POOL_CORE_NUM/DEN` (7/10), `POOL_ELASTIC_NUM/DEN`,
`POOL_SHARD_DIV` (4), `POOL_SHARD_CAP`, `POOL_DEQUE_CAP`, `POOL_MON_MS` (5),
`POOL_BATCH` (2), `POOL_LIFO_CAP` (8).

### Example

```c
#include "thread_pool.h"
#include <stdint.h>

static void child(void* a)  { /* leaf */ }

static void parent(void* a)
{
    /* reentrant submit: goes to the local LIFO slot (cache-hot) */
    pool_submit(child, a);
}

int main(void)
{
    for (intptr_t i = 0; i < 100000; i++)
        pool_submit(parent, (void*)i);

    pool_wait_idle();        /* wait for the task tree to drain */
    return 0;
}
```

### Benchmark

16 CPUs, 1M tasks per scenario, **median of 10 repetitions**, against
**Intel TBB** and **Windows Thread Pool**.

![Thread pool throughput](docs/img/tp_throughput.svg)

Throughput **~2.1×–2.7× above TBB** and **~3.7×–4.4× above Windows TP** on
external submit; on recursive *spawn* it ties TBB (~1.04×) and beats Windows TP by
~9.6×.

![p99 tail latency](docs/img/tp_tail_latency.svg)

On external submit the **p99 stays sub-microsecond** (~0.8 µs) — about **10×
better than TBB** (~7–9 µs) and **40×–90× better than Windows TP** (33–77 µs).

> **Note on `mempool/media`:** it is the shortest-task scenario (each task does a
> memory-pool alloc/free), where the pool's *dispatch* advantage is largest —
> hence the highest relative throughput. Two caveats: (1) the `cpu%`/`cores`
> columns come out as `0` — an artifact of the Windows CPU-time counter
> granularity (~15.6 ms), since the run lasts ~10 ms; the **wall-clock throughput
> is stable** (10 reps between 4.9 and 6.4 Mtask/s). (2) In exchange for the
> throughput, this scenario has the pool's **worst tail** (p99 ~234 µs), which is
> why it is left out of the p99 chart.

Data: [`Tester/thread_pool/`](Tester/thread_pool/) — `bench_run_latest.log` and
`thread_pool_bench_results*.tsv`.

---

## 🧠 Memory Pool

### How it works

A general-purpose **size-class** allocator with a **per-thread heap** (fast path
with no lock and no atomics) and a chunk cache returned to the OS via purge.

![Memory pool architecture](docs/img/arch_memory_pool.svg)

- Each `size` maps to a **class** via an O(1) table (36 classes, ~25% step);
  blocks `> 16 KB` fall into the **LARGE** path (`malloc` passthrough).
- Each thread has its own **heap (TLS)** with per-class span lists. The **64 KB
  span** is aligned and its metadata is found by a **pointer mask** — zero header
  per object.
- The **local free-list** is touched only by the owner (no atomics). A `free`
  from another thread pushes onto the span's `remote_free` (**Treiber stack**);
  the owner drains it later.
- Synchronous per-span refill from a **two-level 64 KB chunk cache** (per-thread +
  global), fed by **4 MB segments** from the OS. Purge returns idle segments.

### API

```c
#include "memory_pool.h"
```

| Function | Description |
|---|---|
| `void memop_init(void)` / `void memop_shutdown(void)` | Brings the pool up / down (auto on load). |
| `void* memop_alloc_raw(uint64 size)` | Fast path: returns a raw `void*`. |
| `void memop_free_raw(void* ptr)` | Frees a `memop_alloc_raw`. |
| `MemBuffer memop_alloc(uint64 size)` | Variant returning `{Ptr, Size}`. |
| `void memop_free(MemBuffer* buf)` | Frees a `memop_alloc`. |
| `void memop_purge(void)` | Explicit trim: returns idle segments to the OS. |
| `void memop_get_stats(MemPoolStats* out)` | Stats snapshot (alloc/free, refills, reserved RAM…). |

### Example

```c
#include "memory_pool.h"
#include <string.h>

/* fast path */
void* p = memop_alloc_raw(256);
memset(p, 0, 256);
memop_free_raw(p);

/* variant with embedded size */
MemBuffer b = memop_alloc(4096);
memset(b.Ptr, 0, b.Size);
memop_free(&b);

/* observability + trim */
MemPoolStats st;
memop_get_stats(&st);
memop_purge();
```

### Benchmark

16 CPUs / 8 threads, **median of 9 runs**, against **CRT malloc**, **rpmalloc**,
and **mimalloc**. The `memop-lw-*` variants are the same allocator with the
[mem_leak_watch](#-mem-leak-watch) turned on top of it.

![Larson churn — throughput](docs/img/mp_larson.svg)

Under multi-threaded *churn* (Larson) it is a **technical tie** (~132–140 Mop/s).
Run-to-run variance is high (memop swung between 107 and 149 Mop/s across the 9
runs), hence the medians.

![64B fixed — cost per operation](docs/img/mp_smallfixed.svg)

On the fixed-64B hot path, alloc/free cost **~5 ns** per operation, competitive
with the best and clearly ahead of mimalloc on `free`.

![Per-call latency, hot working-set](docs/img/mp_latency.svg)

**Mem_leak_watch cost:** `memop-lw-dbg`/`memop-lw-prod` are practically
overlapping the plain `memop-pool` — the monitor **does not burden the hot path**
measurably.

Data: [`bench/mempool_bench_latest.log`](bench/mempool_bench_latest.log) and
[`bench/mempool_bench_medians.json`](bench/mempool_bench_medians.json).

---

## 🔎 Mem Leak Watch

### How it works

A leak monitor **without collection**, over the memory pool. It does the same
reachability tracing a GC would use to decide what to free (roots → reached
pointers → marked spans), but **never frees anything**: whatever isn't reached
becomes a log warning, with the mini-backtrace of where the span was created.

![Mem leak watch flow](docs/img/flow_mem_leak_watch.svg)

- **cheap check** (timer): only reads `memop_get_stats()`, without suspending any
  thread — runs at high frequency.
- **expensive scan** (only when RAM crosses the threshold): suspends each
  registered thread, one at a time, reads registers + stack as roots, walks the
  spans, and reports what's left.

### API

```c
#include "mem_leak_watch.h"
```

| Function | Description |
|---|---|
| `void mem_leak_watch_default_config(MemLeakWatchConfig* out)` | Fills the defaults (interval/threshold per build). |
| `boolean mem_leak_watch_start(const MemLeakWatchConfig* cfg)` | Starts the monitor (dedicated thread). `cfg=NULL` uses defaults. |
| `void mem_leak_watch_stop(void)` | Stops and joins the thread. Safe even if not started. |
| `void mem_leak_watch_scan_now(void)` | Forces a scan now (outside the timer). |

`MemLeakWatchConfig`: `enabled`, `interval_ms`, `warn_threshold_bytes`,
`crit_threshold_bytes`, `log_path` (NULL → `mem_leak_watch.log`).

### Example

```c
#include "mem_leak_watch.h"

MemLeakWatchConfig cfg;
mem_leak_watch_default_config(&cfg);
cfg.interval_ms          = 5000;
cfg.warn_threshold_bytes = 256ull * 1024 * 1024;
mem_leak_watch_start(&cfg);
/* ... app runs ... */
mem_leak_watch_scan_now();     /* on-demand manual scan */
mem_leak_watch_stop();
```

### Known limitations (v1, confirmed by testing)

- **Windows** only for now (`thread_activity_win` / `StackWalk64`).
- Tracks only *size-class* spans (≤ 16 KB); LARGE blocks not yet included.
- Does not scan `.data`/`.bss` as roots — a pointer whose only live reference is
  global/static shows up as a false "leak".
- Only sees threads created via this lib's `thread_create()`.
- Marks by **whole span**, not per block.
- Uses `SuspendThread`/`GetThreadContext` — mitigated by suspending **one thread
  at a time**, without allocating/calling `dbghelp` while a thread is suspended.

Production cost is ~zero on the hot path — see `memop-lw-prod` in the
[memory pool benchmark](#benchmark-1).

---

## Supporting modules

| Module | File | Description |
|---|---|---|
| **Atomics** | `atomics.h` | Per-platform inline atomics (`InterlockedXxx` / `__atomic_*`): load/store, add, CAS, pointers, fences. |
| **Thread handler** | `thread_handler.h` | `thread_create`/`thread_join`/`thread_enum`, mutex, yield, 64-bit atomics — thin wrapper over WinAPI/pthread. |
| **Thread wait** | `thread_wait.h` | Low-cost park/wake (`WaitOnAddress`/`WakeByAddress` on Windows, futex on Linux). |
| **Thread activity** | `thread_activity.h` | Samples per-thread CPU and classifies tasks (NORMAL / LONG_CPU / LONG_BLOCKED) — basis of the elastic worker. |
| **Ring queue** | `ring_queue.h` | Vyukov-style MPMC queue, `head`/`tail` on separate cache lines (no false sharing). |
| **Event handler** | `event_handler.h` | Captures error context and fires before shutting down. |
| **List handler** | `list_hander.c` | Generic `ListXPB` using CRT `malloc` (process-lifetime structures, outside the resettable pool). |

> `string_handler` is in the design phase and is **not covered** by this documentation.

### Thread handler — main functions

| Function | Description |
|---|---|
| `Thread* thread_create(xthread_func_t* func, void* arg, int* status)` | Creates a lib-tracked thread. |
| `void thread_join(Thread** t)` | Joins and frees. |
| `void thread_init(CreatedThread created, CreatedThread ended)` | Registers life-cycle hooks (used by the memory pool). |
| `void thread_enum(ThreadEnumCb cb, void* ctx)` | Enumerates, under lock, the live threads (used by mem leak watch). |

---

## Base types

In `include/xplatbase.h`: `boolean`, `byte`, `int16..int64`, `uint16..uint64`,
and the containers `BufferXPB` (typed buffer), `ListXPB` (generic list), and
`StringX` (string with capacity). Macros `xpb_allocate`, `xpb_list_add`, etc.
carry `__func__/__FILE__/__LINE__` for error tracking
(`CallContextGlobalEvent`).

---

## Techniques and credits

The design combines well-established techniques (credited below) with the
author's own integration and mechanisms. The annotations are also in each `.c`'s
comments.

### Thread Pool

| Technique | Origin / credit |
|---|---|
| Bounded MPMC queue with a per-slot *sequence number* | **Dmitry Vyukov** — bounded MPMC queue |
| Local work-stealing deque (push/take/steal) | **Chase & Lev (2005)**; correct memory model by **Lê, Pop, Cohen, Zappa Nardelli (2013)** |
| Batch reservation via CAS on `head` (`steal_batch`) | **Crossbeam** (Rust project) |
| Non-stealable LIFO slot for reentrant spawn | inspired by the **Tokio** runtime (Rust) |
| **core/reserve** (spin+wake vs park-first) + **elastic worker** (monitor wakes extras under backlog) | **original design** — Sergio Paludo (V2.05) |

### Memory Pool

| Technique | Origin / credit |
|---|---|
| 64 KB aligned span, metadata by **pointer mask** (zero header/object) | **mimalloc** (Daan Leijen, Microsoft Research) / **rpmalloc** (Mattias Jansson) |
| Fine size-classes + O(1) `size→class` mapping | **tcmalloc / mimalloc** lineage |
| Per-thread heap (TLS), local free-list without atomics | **mimalloc / rpmalloc** |
| Remote free via atomic stack (`remote_free`) | **Treiber stack** — R. Kent Treiber (IBM, 1986) |
| "Best of each lib" integration + **two-level chunk cache** + synchronous per-span refill | **original design** — Sergio Paludo |

### Mem Leak Watch

| Technique | Origin / credit |
|---|---|
| **Conservative** reachability scan (roots = registers + stack; interior pointers) | **Boehm–Demers–Weiser** conservative GC (Hans Boehm, Alan Demers, Mark Weiser) |
| Thread suspension/inspection and stack walking | **Win32** `SuspendThread`/`GetThreadContext` + `StackWalk64` (dbghelp); stack bounds via `NtQueryInformationThread` (TEB) |
| **"Watch without collecting"** application (reports instead of freeing), built over the memory pool with a decoupled span snapshot | **original design** — Sergio Paludo |

---

## Status

Project in the design and consolidation phase. The three highlights are
functional and benchmarked; the supporting modules provide the cross-platform
base. Linux portability is partial on some paths (mem_leak_watch is Windows-only
in v1).
