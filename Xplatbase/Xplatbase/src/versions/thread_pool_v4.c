/*
 * thread_pool_v4.c — V3 otimizado (ver .h). Mudancas vs V3:
 *   1) submit sem rdtscp (V4Task = {fn,arg}).
 *   2) steal do inbox so quando nao-vazio.
 *   3) pending POR WORKER (soma sob demanda no wait_idle) -> sem contencao global.
 *   4) __forceinline nas funcoes do hot path.
 */

#include "thread_pool_v4.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../include/xplatbase.h"
#include "atomics.h"
#include "ring_queue.h"
#include "thread_wait.h"

#ifndef V4_WORKER_NUM
#define V4_WORKER_NUM    1
#endif
#ifndef V4_WORKER_DEN
#define V4_WORKER_DEN    1
#endif
#ifndef V4_WORKER_SUB
#define V4_WORKER_SUB    0       /* workers = cores*NUM/DEN - SUB */
#endif
#ifndef V4_WORKER_ABS
#define V4_WORKER_ABS    0       /* >0: nº ABSOLUTO de workers (override do calculo) */
#endif
#ifndef V4_DEQUE_CAP
#define V4_DEQUE_CAP     4096
#endif
#ifndef V4_INBOX_CAP
#define V4_INBOX_CAP     1024
#endif
#ifndef V4_DRAIN_BATCH
#define V4_DRAIN_BATCH   64
#endif
#ifndef V4_SPIN_PAUSE
#define V4_SPIN_PAUSE    512
#endif
#ifndef V4_SPIN_YIELD
#define V4_SPIN_YIELD    64
#endif
#ifndef V4_SPIN_SLEEP0
#define V4_SPIN_SLEEP0   8
#endif
#ifndef V4_PARK_TIMEOUT_US
#define V4_PARK_TIMEOUT_US 1000
#endif

#ifdef _MSC_VER
#define V4_INLINE static __forceinline
#else
#define V4_INLINE static inline __attribute__((always_inline))
#endif

#ifdef XPLATBASE_WIN
    #include <intrin.h>
    typedef HANDLE       v4_thread_t;
    typedef DWORD WINAPI v4_fn_sig(void*);
    static bool v4_thread_start(v4_thread_t* o, v4_fn_sig* fn, void* a){ *o=CreateThread(NULL,0,fn,a,0,NULL); return *o!=NULL; }
    static void v4_thread_join(v4_thread_t h){ WaitForSingleObject(h,INFINITE); CloseHandle(h); }
    static void v4_yield(void){ SwitchToThread(); }
    static void v4_sleep0(void){ Sleep(0); }
    #define V4_FN   DWORD WINAPI
    #define V4_RET  return 0
    typedef volatile LONG64 v4_a64;
    #define v4_ld(p)        (*(p))
    #define v4_st(p,v)      (*(p) = (LONG64)(v))
    #define v4_fence()      MemoryBarrier()
    static int v4_cas(v4_a64* p, LONG64 e, LONG64 d){ return InterlockedCompareExchange64(p,d,e)==e; }
#else
    #include <pthread.h>
    #include <sched.h>
    #include <time.h>
    #include <stdatomic.h>
    typedef pthread_t v4_thread_t;
    typedef void*     v4_fn_sig(void*);
    static bool v4_thread_start(v4_thread_t* o, v4_fn_sig* fn, void* a){ memset(o,0,sizeof(*o)); return pthread_create(o,NULL,fn,a)==0; }
    static void v4_thread_join(v4_thread_t h){ pthread_join(h,NULL); }
    static void v4_yield(void){ sched_yield(); }
    static void v4_sleep0(void){ struct timespec z={0,0}; nanosleep(&z,NULL); }
    #define V4_FN   void*
    #define V4_RET  return NULL
    typedef _Atomic long long v4_a64;
    #define v4_ld(p)        atomic_load_explicit((p), memory_order_relaxed)
    #define v4_st(p,v)      atomic_store_explicit((p),(long long)(v), memory_order_relaxed)
    #define v4_fence()      atomic_thread_fence(memory_order_seq_cst)
    static int v4_cas(v4_a64* p, long long e, long long d){ return atomic_compare_exchange_strong_explicit(p,&e,d,memory_order_seq_cst,memory_order_relaxed); }
#endif

/* ───── Task + Deque Chase-Lev (sem timestamp) ───── */
typedef struct { v4_task_fn fn; void* arg; } V4Task;

typedef struct XPL_ALIGN(XPL_CACHELINE) {
    v4_a64 top;
    char   _p0[XPL_CACHELINE - sizeof(v4_a64)];
    v4_a64 bottom;
    char   _p1[XPL_CACHELINE - sizeof(v4_a64)];
    V4Task* buf; int cap; int mask;
} V4Deque;

static bool v4_deque_init(V4Deque* d, int cap){
    d->buf=(V4Task*)malloc((size_t)cap*sizeof(V4Task)); if(!d->buf) return false;
    d->cap=cap; d->mask=cap-1; v4_st(&d->top,0); v4_st(&d->bottom,0); return true;
}
static void v4_deque_free(V4Deque* d){ free(d->buf); d->buf=NULL; }

V4_INLINE bool v4_deque_push(V4Deque* d, const V4Task* t){
    int64_t b=v4_ld(&d->bottom), tp=v4_ld(&d->top);
    if (b - tp >= d->cap) return false;
    d->buf[b & d->mask] = *t;
    v4_fence();
    v4_st(&d->bottom, b+1);
    return true;
}
V4_INLINE bool v4_deque_take(V4Deque* d, V4Task* out){
    int64_t b=v4_ld(&d->bottom)-1; v4_st(&d->bottom,b); v4_fence();
    int64_t t=v4_ld(&d->top);
    if (t<=b){
        *out=d->buf[b & d->mask];
        if (t==b){ int ok=v4_cas(&d->top,t,t+1); v4_st(&d->bottom,b+1); return ok!=0; }
        return true;
    } else { v4_st(&d->bottom,b+1); return false; }
}
typedef enum { V4_OK, V4_EMPTY, V4_ABORT } V4Steal;
V4_INLINE V4Steal v4_deque_steal(V4Deque* d, V4Task* out){
    int64_t t=v4_ld(&d->top); v4_fence(); int64_t b=v4_ld(&d->bottom);
    if (t<b){ V4Task x=d->buf[t & d->mask]; if(!v4_cas(&d->top,t,t+1)) return V4_ABORT; *out=x; return V4_OK; }
    return V4_EMPTY;
}

/* ───── Worker / Pool ───── */
typedef struct XPL_ALIGN(XPL_CACHELINE) {
    WSPoolV4*   pool;
    int         index;
    v4_thread_t handle;
    xwait_t     wait;
    V4Deque     deque;
    RingQueue   inbox;
    void*       inbox_buf;
    uint32_t    steal_cursor;
    xatomic_int pending;        /* pending POR WORKER */
    char        _pad[XPL_CACHELINE];
} V4Worker;

struct WSPoolV4 {
    int            n_workers;
    V4Worker*      workers;
    xatomic_uint32 submit_rr;
    xatomic_int    stop;
};

#ifdef XPLATBASE_WIN
static __declspec(thread) V4Worker* g_v4_self;
#else
static __thread V4Worker* g_v4_self;
#endif

V4_INLINE void v4_run(V4Worker* self, V4Task* t){ t->fn(t->arg); atomic_sub(&self->pending,1); }

V4_INLINE int v4_drain_inbox(V4Worker* self){
    int moved=0; V4Task tmp;
    for (int k=0;k<V4_DRAIN_BATCH;k++){
        if (!xring_pop_mc(&self->inbox, self->inbox_buf, &tmp)) break;
        if (!v4_deque_push(&self->deque, &tmp)) v4_run(self, &tmp);
        moved++;
    }
    return moved;
}

V4_INLINE bool v4_try_get(WSPoolV4* pool, V4Worker* self, V4Task* out){
    if (v4_deque_take(&self->deque, out)) return true;
    if (v4_drain_inbox(self) > 0 && v4_deque_take(&self->deque, out)) return true;

    int nw=pool->n_workers; uint32_t s=self->steal_cursor;
    for (int k=1;k<=nw;k++){
        int v=(int)((s+(uint32_t)k)%(uint32_t)nw);
        if (v==self->index) continue;
        V4Worker* victim=&pool->workers[v];
        if (v4_deque_steal(&victim->deque, out)==V4_OK){ self->steal_cursor=(uint32_t)v; return true; }
        /* inbox alheio: so se nao-vazio (evita pop_mc desperdicado) */
        if (!ring_queue_empty(&victim->inbox) && xring_pop_mc(&victim->inbox, victim->inbox_buf, out)){
            self->steal_cursor=(uint32_t)v; return true;
        }
    }
    return false;
}

static bool v4_spin(WSPoolV4* pool, V4Worker* self, V4Task* out){
    for (int i=0;i<V4_SPIN_PAUSE;i++){ if(v4_try_get(pool,self,out))return true; if(atomic_get(&pool->stop))return false; xcpu_pause(); }
    for (int i=0;i<V4_SPIN_YIELD;i++){ if(v4_try_get(pool,self,out))return true; if(atomic_get(&pool->stop))return false; v4_yield(); }
    for (int i=0;i<V4_SPIN_SLEEP0;i++){ if(v4_try_get(pool,self,out))return true; if(atomic_get(&pool->stop))return false; v4_sleep0(); }
    return false;
}

static V4_FN v4_worker_fn(void* raw){
    V4Worker* self=(V4Worker*)raw; WSPoolV4* pool=self->pool; V4Task t;
    g_v4_self=self;
    while (!atomic_get(&pool->stop)){
        if (v4_try_get(pool,self,&t)){ v4_run(self,&t); continue; }
        thread_wait_prepare(&self->wait);
        if (v4_try_get(pool,self,&t)){ v4_run(self,&t); continue; }
        if (v4_spin(pool,self,&t))   { v4_run(self,&t); continue; }
        if (atomic_get(&pool->stop)) break;
        thread_wait_sleep_for(&self->wait, V4_PARK_TIMEOUT_US);
    }
    V4_RET;
}

WSPoolV4* v4_pool_create(int cores_override){
    int cores=cores_override>0?cores_override:xcpu_count(); if(cores<1)cores=1;
    int nw=cores*V4_WORKER_NUM/V4_WORKER_DEN - V4_WORKER_SUB;
#if V4_WORKER_ABS > 0
    nw = V4_WORKER_ABS;
#endif
    if(nw<1)nw=1;
    WSPoolV4* pool=(WSPoolV4*)calloc(1,sizeof(WSPoolV4)); if(!pool) return NULL;
    pool->n_workers=nw;
    pool->workers=(V4Worker*)calloc((size_t)nw,sizeof(V4Worker)); if(!pool->workers){ free(pool); return NULL; }
    thread_wait_init(false);
    for (int i=0;i<nw;i++){
        V4Worker* w=&pool->workers[i];
        w->pool=pool; w->index=i; w->steal_cursor=(uint32_t)i;
        thread_wait_prepare(&w->wait);
        if (!v4_deque_init(&w->deque, V4_DEQUE_CAP)){ v4_pool_destroy(pool); return NULL; }
        ring_queue_init(&w->inbox, V4_INBOX_CAP);
        w->inbox_buf=malloc((size_t)V4_INBOX_CAP*sizeof(V4Task));
        if (w->inbox.capacity==0 || !w->inbox_buf){ v4_pool_destroy(pool); return NULL; }
    }
    for (int i=0;i<nw;i++){
        if (!v4_thread_start(&pool->workers[i].handle, v4_worker_fn, &pool->workers[i])){
            atomic_set(&pool->stop,1);
            for (int j=0;j<i;j++){ thread_wait_wake(&pool->workers[j].wait); v4_thread_join(pool->workers[j].handle); }
            v4_pool_destroy(pool); return NULL;
        }
    }
    return pool;
}

bool v4_pool_submit(WSPoolV4* pool, v4_task_fn fn, void* arg){
    if (!pool || !fn) return false;
    if (atomic_get(&pool->stop)) return false;
    V4Task t = { fn, arg };                 /* sem rdtscp */

    /* reentrante: deque local (Chase-Lev). inline se cheio (evita self-deadlock). */
    V4Worker* me=g_v4_self;
    if (me && me->pool==pool){
        atomic_add(&me->pending,1);
        if (v4_deque_push(&me->deque,&t)) return true;
        v4_run(me,&t);                      /* inline: roda agora e decrementa */
        return true;
    }

    int nw=pool->n_workers; int spins=0;
    for (;;){
        uint32_t start=atomic_u32_add(&pool->submit_rr,1u);
        for (int probe=0;probe<nw;probe++){
            int idx=(int)((start+(uint32_t)probe)%(uint32_t)nw);
            V4Worker* w=&pool->workers[idx];
            atomic_add(&w->pending,1);
            if (xring_push_mp(&w->inbox,w->inbox_buf,&t)){ thread_wait_wake(&w->wait); return true; }
            atomic_sub(&w->pending,1);       /* desfaz se nao coube */
        }
        if (atomic_get(&pool->stop)) return false;
        if      (spins<64)  xcpu_pause();
        else if (spins<256) v4_yield();
        else                v4_sleep0();
        spins++;
    }
}

static long v4_total_pending(WSPoolV4* pool){
    long s=0; for (int i=0;i<pool->n_workers;i++) s+=atomic_get(&pool->workers[i].pending); return s;
}
void v4_pool_wait_idle(WSPoolV4* pool){ if(!pool)return; while (v4_total_pending(pool)>0) v4_sleep0(); }
void v4_pool_dims(WSPoolV4* pool, int* w, int* l){ if(!pool)return; if(w)*w=pool->n_workers; if(l)*l=pool->n_workers; }

void v4_pool_destroy(WSPoolV4* pool){
    if (!pool) return;
    if (!atomic_get(&pool->stop)){
        v4_pool_wait_idle(pool);
        atomic_set(&pool->stop,1);
        if (pool->workers)
            for (int i=0;i<pool->n_workers;i++){
                thread_wait_wake(&pool->workers[i].wait);
                if (pool->workers[i].handle) v4_thread_join(pool->workers[i].handle);
            }
    }
    if (pool->workers){
        for (int i=0;i<pool->n_workers;i++){
            v4_deque_free(&pool->workers[i].deque);
            free(pool->workers[i].inbox_buf);
            ring_queue_destroy(&pool->workers[i].inbox);
        }
        free(pool->workers);
    }
    free(pool);
    thread_wait_shutdown();
}
