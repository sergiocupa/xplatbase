/*
 * thread_pool_v3.c — deque Chase-Lev por worker + inbox MPMC (ver .h).
 *
 * Chase-Lev (Lê et al., 2013): bottom so o dono escreve (push/take); top os
 * ladroes (steal) via CAS. take() do dono nao usa CAS no caso comum (apenas
 * uma fence seq_cst), so precisa de CAS ao disputar o ultimo elemento.
 */

#include "thread_pool_v3.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../include/xplatbase.h"   /* xcpu_count, xcpu_pause */
#include "atomics.h"
#include "ring_queue.h"
#include "thread_wait.h"

/* ───────────── Config (override -D) ───────────── */
#ifndef V3_WORKER_NUM
#define V3_WORKER_NUM    1
#endif
#ifndef V3_WORKER_DEN
#define V3_WORKER_DEN    1
#endif
#ifndef V3_DEQUE_CAP
#define V3_DEQUE_CAP     4096
#endif
#ifndef V3_INBOX_CAP
#define V3_INBOX_CAP     1024
#endif
#ifndef V3_DRAIN_BATCH
#define V3_DRAIN_BATCH   64
#endif
#ifndef V3_SPIN_PAUSE
#define V3_SPIN_PAUSE    512
#endif
#ifndef V3_SPIN_YIELD
#define V3_SPIN_YIELD    64
#endif
#ifndef V3_SPIN_SLEEP0
#define V3_SPIN_SLEEP0   8
#endif
#ifndef V3_PARK_TIMEOUT_US
#define V3_PARK_TIMEOUT_US 1000
#endif

/* ───────────── Plataforma: thread + atomics64 do deque ───────────── */
#ifdef XPLATBASE_WIN
    #include <intrin.h>
    typedef HANDLE       v3_thread_t;
    typedef DWORD WINAPI v3_fn_sig(void*);
    static bool v3_thread_start(v3_thread_t* o, v3_fn_sig* fn, void* a){ *o=CreateThread(NULL,0,fn,a,0,NULL); return *o!=NULL; }
    static void v3_thread_join(v3_thread_t h){ WaitForSingleObject(h,INFINITE); CloseHandle(h); }
    static void v3_yield(void){ SwitchToThread(); }
    static void v3_sleep0(void){ Sleep(0); }
    static uint64_t v3_rdtscp(void){ unsigned int aux; return (uint64_t)__rdtscp(&aux); }
    #define V3_FN   DWORD WINAPI
    #define V3_RET  return 0

    /* atomics de 64 bits do deque: stores RELAXED (baratos), CAS so no top. */
    typedef volatile LONG64 v3_a64;
    #define v3_ld(p)        (*(p))                 /* x86: load com ordem TSO    */
    #define v3_st(p,v)      (*(p) = (LONG64)(v))   /* x86: store relaxed (TSO)   */
    #define v3_fence()      MemoryBarrier()        /* seq_cst (a fence do CL)    */
    static int v3_cas(v3_a64* p, LONG64 exp, LONG64 des){ return InterlockedCompareExchange64(p,des,exp)==exp; }
#else
    #include <pthread.h>
    #include <sched.h>
    #include <time.h>
    #include <stdatomic.h>
    typedef pthread_t v3_thread_t;
    typedef void*     v3_fn_sig(void*);
    static bool v3_thread_start(v3_thread_t* o, v3_fn_sig* fn, void* a){ memset(o,0,sizeof(*o)); return pthread_create(o,NULL,fn,a)==0; }
    static void v3_thread_join(v3_thread_t h){ pthread_join(h,NULL); }
    static void v3_yield(void){ sched_yield(); }
    static void v3_sleep0(void){ struct timespec z={0,0}; nanosleep(&z,NULL); }
    static uint64_t v3_rdtscp(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return (uint64_t)ts.tv_sec*1000000000ULL+(uint64_t)ts.tv_nsec; }
    #define V3_FN   void*
    #define V3_RET  return NULL

    typedef _Atomic long long v3_a64;
    #define v3_ld(p)        atomic_load_explicit((p), memory_order_relaxed)
    #define v3_st(p,v)      atomic_store_explicit((p),(long long)(v), memory_order_relaxed)
    #define v3_fence()      atomic_thread_fence(memory_order_seq_cst)
    static int v3_cas(v3_a64* p, long long exp, long long des){
        return atomic_compare_exchange_strong_explicit(p,&exp,des,memory_order_seq_cst,memory_order_relaxed);
    }
#endif

/* ───────────── Task + Deque Chase-Lev ───────────── */
typedef struct { v3_task_fn fn; void* arg; uint64_t enqueue_tsc; } V3Task;

typedef struct XPL_ALIGN(XPL_CACHELINE) {
    v3_a64 top;
    char   _p0[XPL_CACHELINE - sizeof(v3_a64)];
    v3_a64 bottom;
    char   _p1[XPL_CACHELINE - sizeof(v3_a64)];
    V3Task* buf;        /* V3Task[cap] */
    int     cap;
    int     mask;
} V3Deque;

static bool v3_deque_init(V3Deque* d, int cap){
    d->buf = (V3Task*)malloc((size_t)cap*sizeof(V3Task));
    if (!d->buf) return false;
    d->cap=cap; d->mask=cap-1; v3_st(&d->top,0); v3_st(&d->bottom,0);
    return true;
}
static void v3_deque_free(V3Deque* d){ free(d->buf); d->buf=NULL; }

/* dono empurra no bottom (sem CAS). false = cheio. */
static bool v3_deque_push(V3Deque* d, const V3Task* t){
    int64_t b = v3_ld(&d->bottom);
    int64_t tp= v3_ld(&d->top);
    if (b - tp >= d->cap) return false;
    d->buf[b & d->mask] = *t;
    v3_fence();                 /* publica o slot antes de avancar bottom */
    v3_st(&d->bottom, b+1);
    return true;
}

/* dono tira do bottom (LIFO). Sem CAS, exceto disputa do ultimo item. */
static bool v3_deque_take(V3Deque* d, V3Task* out){
    int64_t b = v3_ld(&d->bottom) - 1;
    v3_st(&d->bottom, b);
    v3_fence();                 /* a fence seq_cst classica do Chase-Lev */
    int64_t t = v3_ld(&d->top);
    if (t <= b) {
        *out = d->buf[b & d->mask];
        if (t == b) {           /* ultimo: disputa com steal */
            int ok = v3_cas(&d->top, t, t+1);
            v3_st(&d->bottom, b+1);
            return ok != 0;
        }
        return true;            /* caso comum: sem CAS */
    } else {
        v3_st(&d->bottom, b+1); /* vazio */
        return false;
    }
}

typedef enum { V3_OK, V3_EMPTY, V3_ABORT } V3Steal;
/* ladrao tira do top (FIFO) via CAS. */
static V3Steal v3_deque_steal(V3Deque* d, V3Task* out){
    int64_t t = v3_ld(&d->top);
    v3_fence();
    int64_t b = v3_ld(&d->bottom);
    if (t < b) {
        V3Task x = d->buf[t & d->mask];   /* le antes do CAS */
        if (!v3_cas(&d->top, t, t+1)) return V3_ABORT;
        *out = x;
        return V3_OK;
    }
    return V3_EMPTY;
}

/* ───────────── Worker / Pool ───────────── */
typedef struct XPL_ALIGN(XPL_CACHELINE) {
    WSPoolV3*   pool;
    int         index;
    v3_thread_t handle;
    xwait_t     wait;
    V3Deque     deque;
    RingQueue   inbox;       /* MPMC: submit externo */
    void*       inbox_buf;   /* V3Task[V3_INBOX_CAP] */
    uint32_t    steal_cursor;
} V3Worker;

struct WSPoolV3 {
    int            n_workers;
    V3Worker*      workers;
    xatomic_uint32 submit_rr;
    xatomic_int    pending;
    xatomic_int    stop;
};

/* worker corrente nesta thread (submit reentrante: task dentro de task). */
#ifdef XPLATBASE_WIN
static __declspec(thread) V3Worker* g_v3_self;
#else
static __thread V3Worker* g_v3_self;
#endif

static void v3_run(WSPoolV3* pool, V3Task* t){ t->fn(t->arg); atomic_sub_inline(&pool->pending,1); }

/* move ate BATCH itens do inbox proprio para o deque proprio. retorna nº movido. */
static int v3_drain_inbox(V3Worker* self){
    int moved=0; V3Task tmp;
    for (int k=0;k<V3_DRAIN_BATCH;k++){
        if (!xring_pop_mc(&self->inbox, self->inbox_buf, &tmp)) break;
        if (!v3_deque_push(&self->deque, &tmp)) {           /* deque cheio: roda direto */
            v3_run(self->pool, &tmp);
        }
        moved++;
    }
    return moved;
}

/* tenta obter UMA task: deque proprio -> drena inbox+deque -> steal alheio. */
static bool v3_try_get(WSPoolV3* pool, V3Worker* self, V3Task* out){
    if (v3_deque_take(&self->deque, out)) return true;

    if (v3_drain_inbox(self) > 0) {
        if (v3_deque_take(&self->deque, out)) return true;
    }

    int nw = pool->n_workers;
    uint32_t s = self->steal_cursor;
    for (int k=1;k<=nw;k++){
        int v = (int)((s + (uint32_t)k) % (uint32_t)nw);
        if (v == self->index) continue;
        V3Worker* victim = &pool->workers[v];
        V3Steal r = v3_deque_steal(&victim->deque, out);
        if (r == V3_OK)    { self->steal_cursor=(uint32_t)v; return true; }
        /* deque vazio/abort: tenta o inbox da vitima (dono pode estar ocupado) */
        if (xring_pop_mc(&victim->inbox, victim->inbox_buf, out)) {
            self->steal_cursor=(uint32_t)v; return true;
        }
    }
    return false;
}

static bool v3_spin(WSPoolV3* pool, V3Worker* self, V3Task* out){
    for (int i=0;i<V3_SPIN_PAUSE;i++){ if (v3_try_get(pool,self,out)) return true; if (atomic_get_inline(&pool->stop)) return false; xcpu_pause(); }
    for (int i=0;i<V3_SPIN_YIELD;i++){ if (v3_try_get(pool,self,out)) return true; if (atomic_get_inline(&pool->stop)) return false; v3_yield(); }
    for (int i=0;i<V3_SPIN_SLEEP0;i++){ if (v3_try_get(pool,self,out)) return true; if (atomic_get_inline(&pool->stop)) return false; v3_sleep0(); }
    return false;
}

static V3_FN v3_worker_fn(void* raw){
    V3Worker* self=(V3Worker*)raw; WSPoolV3* pool=self->pool; V3Task t;
    g_v3_self = self;   /* marca esta thread como worker (submit reentrante) */
    while (!atomic_get_inline(&pool->stop)){
        if (v3_try_get(pool,self,&t)){ v3_run(pool,&t); continue; }
        thread_wait_prepare_inline(&self->wait);
        if (v3_try_get(pool,self,&t)){ v3_run(pool,&t); continue; }
        if (v3_spin(pool,self,&t))   { v3_run(pool,&t); continue; }
        if (atomic_get_inline(&pool->stop)) break;
        thread_wait_sleep_for_inline(&self->wait, V3_PARK_TIMEOUT_US);
    }
    V3_RET;
}

WSPoolV3* v3_pool_create(int cores_override){
    int cores = cores_override>0 ? cores_override : xcpu_count();
    if (cores<1) cores=1;
    int nw = cores * V3_WORKER_NUM / V3_WORKER_DEN; if (nw<1) nw=1;

    WSPoolV3* pool=(WSPoolV3*)calloc(1,sizeof(WSPoolV3));
    if (!pool) return NULL;
    pool->n_workers=nw;
    pool->workers=(V3Worker*)calloc((size_t)nw,sizeof(V3Worker));
    if (!pool->workers){ free(pool); return NULL; }

    thread_wait_init(false);

    for (int i=0;i<nw;i++){
        V3Worker* w=&pool->workers[i];
        w->pool=pool; w->index=i; w->steal_cursor=(uint32_t)i;
        thread_wait_prepare_inline(&w->wait);
        if (!v3_deque_init(&w->deque, V3_DEQUE_CAP)){ v3_pool_destroy(pool); return NULL; }
        ring_queue_init(&w->inbox, V3_INBOX_CAP);
        w->inbox_buf = malloc((size_t)V3_INBOX_CAP*sizeof(V3Task));
        if (w->inbox.capacity==0 || !w->inbox_buf){ v3_pool_destroy(pool); return NULL; }
    }
    for (int i=0;i<nw;i++){
        if (!v3_thread_start(&pool->workers[i].handle, v3_worker_fn, &pool->workers[i])){
            atomic_set_inline(&pool->stop,1);
            for (int j=0;j<i;j++){ thread_wait_wake_inline(&pool->workers[j].wait); v3_thread_join(pool->workers[j].handle); }
            v3_pool_destroy(pool); return NULL;
        }
    }
    return pool;
}

bool v3_pool_submit(WSPoolV3* pool, v3_task_fn fn, void* arg){
    if (!pool || !fn) return false;
    if (atomic_get_inline(&pool->stop)) return false;
    V3Task t = { fn, arg, v3_rdtscp() };
    int nw = pool->n_workers;
    atomic_add_inline(&pool->pending,1);

    /* reentrante (task dentro de task): empurra no deque LOCAL (Chase-Lev) — push
     * do dono sem CAS, sem wake; o proprio worker faz take() LIFO em seguida e os
     * ladroes pegam o topo. E aqui que o Chase-Lev deve abrir vantagem. */
    V3Worker* me = g_v3_self;
    if (me && me->pool == pool) {
        if (v3_deque_push(&me->deque, &t)) return true;
        /* deque local cheio: executa INLINE (evita self-deadlock do submit
         * reentrante; com Chase-Lev/LIFO o deque raramente enche). */
        atomic_sub_inline(&pool->pending, 1);
        fn(arg);
        return true;
    }

    int spins=0;
    for (;;){
        uint32_t start = atomic_u32_add_inline(&pool->submit_rr,1u);
        for (int probe=0;probe<nw;probe++){
            int idx=(int)((start+(uint32_t)probe)%(uint32_t)nw);
            V3Worker* w=&pool->workers[idx];
            if (xring_push_mp(&w->inbox, w->inbox_buf, &t)){
                thread_wait_wake_inline(&w->wait);
                return true;
            }
        }
        if (atomic_get_inline(&pool->stop)){ atomic_sub_inline(&pool->pending,1); return false; }
        if      (spins<64)  xcpu_pause();
        else if (spins<256) v3_yield();
        else                v3_sleep0();
        spins++;
    }
}

void v3_pool_wait_idle(WSPoolV3* pool){ if (!pool) return; while (atomic_get_inline(&pool->pending)>0) v3_sleep0(); }

void v3_pool_dims(WSPoolV3* pool, int* w, int* l){ if(!pool)return; if(w)*w=pool->n_workers; if(l)*l=pool->n_workers; }

void v3_pool_destroy(WSPoolV3* pool){
    if (!pool) return;
    if (!atomic_get_inline(&pool->stop)){
        v3_pool_wait_idle(pool);
        atomic_set_inline(&pool->stop,1);
        if (pool->workers)
            for (int i=0;i<pool->n_workers;i++){
                thread_wait_wake_inline(&pool->workers[i].wait);
                if (pool->workers[i].handle) v3_thread_join(pool->workers[i].handle);
            }
    }
    if (pool->workers){
        for (int i=0;i<pool->n_workers;i++){
            v3_deque_free(&pool->workers[i].deque);
            free(pool->workers[i].inbox_buf);
            ring_queue_destroy(&pool->workers[i].inbox);
        }
        free(pool->workers);
    }
    free(pool);
    thread_wait_shutdown();
}
