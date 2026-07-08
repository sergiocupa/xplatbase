/*
 * thread_pool_v2.c — pool OFICIAL (ver thread_pool_v2.h).
 *   Deque Chase-Lev por worker + inbox MPMC + "core + reserva":
 *     - submit externo round-robin SO nos n_core workers (mantem-nos quentes);
 *     - workers core: ociosos fazem spin progressivo (baixa latencia);
 *     - workers reserva: ociosos fazem 1 varredura de steal e parqueiam (sem busy
 *       spin) -> so engajam quando ha backlog (spawn / rajada).
 *   Spawn usa push LOCAL (reentrante) em qualquer worker, e steal por todos.
 *   pending por worker (sem contencao global); funcoes quentes com __forceinline.
 */

#include "thread_pool_v2.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../include/xplatbase.h"
#include "atomics.h"
#include "ring_queue.h"
#include "thread_wait.h"

#ifndef V2_CORE_NUM
#define V2_CORE_NUM      7      /* n_core = cores * 7/10 (~11 em 16) */
#endif
#ifndef V2_CORE_DEN
#define V2_CORE_DEN      10
#endif
#ifndef V2_DEQUE_CAP
#define V2_DEQUE_CAP     4096
#endif
#ifndef V2_INBOX_CAP
#define V2_INBOX_CAP     1024
#endif
#ifndef V2_DRAIN_BATCH
#define V2_DRAIN_BATCH   64
#endif
#ifndef V2_SPIN_PAUSE
#define V2_SPIN_PAUSE    512
#endif
#ifndef V2_SPIN_YIELD
#define V2_SPIN_YIELD    64
#endif
#ifndef V2_SPIN_SLEEP0
#define V2_SPIN_SLEEP0   8
#endif
#ifndef V2_PARK_TIMEOUT_US
#define V2_PARK_TIMEOUT_US 1000
#endif

#ifdef _MSC_VER
#define V2_INLINE static __forceinline
#else
#define V2_INLINE static inline __attribute__((always_inline))
#endif

#ifdef XPLATBASE_WIN
    #include <intrin.h>
    typedef HANDLE       v2_thread_t;
    typedef DWORD WINAPI v2_fn_sig(void*);
    static bool v2_thread_start(v2_thread_t* o, v2_fn_sig* fn, void* a){ *o=CreateThread(NULL,0,fn,a,0,NULL); return *o!=NULL; }
    static void v2_thread_join(v2_thread_t h){ WaitForSingleObject(h,INFINITE); CloseHandle(h); }
    static void v2_yield(void){ SwitchToThread(); }
    static void v2_sleep0(void){ Sleep(0); }
    #define V2_FN   DWORD WINAPI
    #define V2_RET  return 0
    typedef volatile LONG64 v2_a64;
    #define v2_ld(p)        (*(p))
    #define v2_st(p,v)      (*(p) = (LONG64)(v))
    #define v2_fence()      MemoryBarrier()
    static int v2_cas(v2_a64* p, LONG64 e, LONG64 d){ return InterlockedCompareExchange64(p,d,e)==e; }
#else
    #include <pthread.h>
    #include <sched.h>
    #include <time.h>
    #include <stdatomic.h>
    typedef pthread_t v2_thread_t;
    typedef void*     v2_fn_sig(void*);
    static bool v2_thread_start(v2_thread_t* o, v2_fn_sig* fn, void* a){ memset(o,0,sizeof(*o)); return pthread_create(o,NULL,fn,a)==0; }
    static void v2_thread_join(v2_thread_t h){ pthread_join(h,NULL); }
    static void v2_yield(void){ sched_yield(); }
    static void v2_sleep0(void){ struct timespec z={0,0}; nanosleep(&z,NULL); }
    #define V2_FN   void*
    #define V2_RET  return NULL
    typedef _Atomic long long v2_a64;
    #define v2_ld(p)        atomic_load_explicit((p), memory_order_relaxed)
    #define v2_st(p,v)      atomic_store_explicit((p),(long long)(v), memory_order_relaxed)
    #define v2_fence()      atomic_thread_fence(memory_order_seq_cst)
    static int v2_cas(v2_a64* p, long long e, long long d){ return atomic_compare_exchange_strong_explicit(p,&e,d,memory_order_seq_cst,memory_order_relaxed); }
#endif

typedef struct { v2_task_fn fn; void* arg; } V2Task;

typedef struct XPL_ALIGN(XPL_CACHELINE) {
    v2_a64 top;
    char   _p0[XPL_CACHELINE - sizeof(v2_a64)];
    v2_a64 bottom;
    char   _p1[XPL_CACHELINE - sizeof(v2_a64)];
    V2Task* buf; int cap; int mask;
} V2Deque;

static bool v2_deque_init(V2Deque* d, int cap){
    d->buf=(V2Task*)malloc((size_t)cap*sizeof(V2Task)); if(!d->buf) return false;
    d->cap=cap; d->mask=cap-1; v2_st(&d->top,0); v2_st(&d->bottom,0); return true;
}
static void v2_deque_free(V2Deque* d){ free(d->buf); d->buf=NULL; }

V2_INLINE bool v2_deque_push(V2Deque* d, const V2Task* t){
    int64_t b=v2_ld(&d->bottom), tp=v2_ld(&d->top);
    if (b - tp >= d->cap) return false;
    d->buf[b & d->mask] = *t; v2_fence(); v2_st(&d->bottom, b+1); return true;
}
V2_INLINE bool v2_deque_take(V2Deque* d, V2Task* out){
    int64_t b=v2_ld(&d->bottom)-1; v2_st(&d->bottom,b); v2_fence();
    int64_t t=v2_ld(&d->top);
    if (t<=b){
        *out=d->buf[b & d->mask];
        if (t==b){ int ok=v2_cas(&d->top,t,t+1); v2_st(&d->bottom,b+1); return ok!=0; }
        return true;
    } else { v2_st(&d->bottom,b+1); return false; }
}
typedef enum { V2_OK, V2_EMPTY, V2_ABORT } V2Steal;
V2_INLINE V2Steal v2_deque_steal(V2Deque* d, V2Task* out){
    int64_t t=v2_ld(&d->top); v2_fence(); int64_t b=v2_ld(&d->bottom);
    if (t<b){ V2Task x=d->buf[t & d->mask]; if(!v2_cas(&d->top,t,t+1)) return V2_ABORT; *out=x; return V2_OK; }
    return V2_EMPTY;
}

typedef struct XPL_ALIGN(XPL_CACHELINE) {
    WSPoolV2*   pool;
    int         index;
    int         is_core;        /* 1 = core (spin), 0 = reserva (park-first) */
    v2_thread_t handle;
    xwait_t     wait;
    V2Deque     deque;
    RingQueue   inbox;
    void*       inbox_buf;
    uint32_t    steal_cursor;
    xatomic_int pending;
    char        _pad[XPL_CACHELINE];
} V2Worker;

struct WSPoolV2 {
    int            n_workers;
    int            n_core;
    V2Worker*      workers;
    xatomic_uint32 submit_rr;
    xatomic_int    stop;
};

#ifdef XPLATBASE_WIN
static __declspec(thread) V2Worker* g_v2_self;
#else
static __thread V2Worker* g_v2_self;
#endif

V2_INLINE void v2_run(V2Worker* self, V2Task* t){ t->fn(t->arg); atomic_sub_inline(&self->pending,1); }

V2_INLINE int v2_drain_inbox(V2Worker* self){
    int moved=0; V2Task tmp;
    for (int k=0;k<V2_DRAIN_BATCH;k++){
        if (!xring_pop_mc(&self->inbox, self->inbox_buf, &tmp)) break;
        if (!v2_deque_push(&self->deque, &tmp)) v2_run(self, &tmp);
        moved++;
    }
    return moved;
}

V2_INLINE bool v2_try_get(WSPoolV2* pool, V2Worker* self, V2Task* out){
    if (v2_deque_take(&self->deque, out)) return true;
    if (v2_drain_inbox(self) > 0 && v2_deque_take(&self->deque, out)) return true;
    int nw=pool->n_workers; uint32_t s=self->steal_cursor;
    for (int k=1;k<=nw;k++){
        int v=(int)((s+(uint32_t)k)%(uint32_t)nw);
        if (v==self->index) continue;
        V2Worker* victim=&pool->workers[v];
        if (v2_deque_steal(&victim->deque, out)==V2_OK){ self->steal_cursor=(uint32_t)v; return true; }
        if (!ring_queue_empty(&victim->inbox) && xring_pop_mc(&victim->inbox, victim->inbox_buf, out)){
            self->steal_cursor=(uint32_t)v; return true;
        }
    }
    return false;
}

static bool v2_spin(WSPoolV2* pool, V2Worker* self, V2Task* out){
    for (int i=0;i<V2_SPIN_PAUSE;i++){ if(v2_try_get(pool,self,out))return true; if(atomic_get_inline(&pool->stop))return false; xcpu_pause(); }
    for (int i=0;i<V2_SPIN_YIELD;i++){ if(v2_try_get(pool,self,out))return true; if(atomic_get_inline(&pool->stop))return false; v2_yield(); }
    for (int i=0;i<V2_SPIN_SLEEP0;i++){ if(v2_try_get(pool,self,out))return true; if(atomic_get_inline(&pool->stop))return false; v2_sleep0(); }
    return false;
}

static V2_FN v2_worker_fn(void* raw){
    V2Worker* self=(V2Worker*)raw; WSPoolV2* pool=self->pool; V2Task t;
    g_v2_self=self;
    int is_core = self->is_core;
    while (!atomic_get_inline(&pool->stop)){
        if (v2_try_get(pool,self,&t)){ v2_run(self,&t); continue; }
        thread_wait_prepare_inline(&self->wait);
        if (v2_try_get(pool,self,&t)){ v2_run(self,&t); continue; }   /* inclui 1 varredura de steal */

        if (is_core){                                  /* core: busy-spin (baixa latencia) */
            if (v2_spin(pool,self,&t)){ v2_run(self,&t); continue; }
        }
        /* reserva (ou core que nao achou): parqueia, sem queimar CPU spinando. */
        if (atomic_get_inline(&pool->stop)) break;
        thread_wait_sleep_for_inline(&self->wait, V2_PARK_TIMEOUT_US);
    }
    V2_RET;
}

WSPoolV2* v2_pool_create(int cores_override){
    int cores=cores_override>0?cores_override:xcpu_count(); if(cores<1)cores=1;
    int nw=cores; if(nw<1)nw=1;
    int nc=cores*V2_CORE_NUM/V2_CORE_DEN; if(nc<1)nc=1; if(nc>nw)nc=nw;

    WSPoolV2* pool=(WSPoolV2*)calloc(1,sizeof(WSPoolV2)); if(!pool) return NULL;
    pool->n_workers=nw; pool->n_core=nc;
    pool->workers=(V2Worker*)calloc((size_t)nw,sizeof(V2Worker)); if(!pool->workers){ free(pool); return NULL; }
    thread_wait_init(false);
    for (int i=0;i<nw;i++){
        V2Worker* w=&pool->workers[i];
        w->pool=pool; w->index=i; w->is_core=(i<nc)?1:0; w->steal_cursor=(uint32_t)i;
        thread_wait_prepare_inline(&w->wait);
        if (!v2_deque_init(&w->deque, V2_DEQUE_CAP)){ v2_pool_destroy(pool); return NULL; }
        ring_queue_init(&w->inbox, V2_INBOX_CAP);
        w->inbox_buf=malloc((size_t)V2_INBOX_CAP*sizeof(V2Task));
        if (w->inbox.capacity==0 || !w->inbox_buf){ v2_pool_destroy(pool); return NULL; }
    }
    for (int i=0;i<nw;i++){
        if (!v2_thread_start(&pool->workers[i].handle, v2_worker_fn, &pool->workers[i])){
            atomic_set_inline(&pool->stop,1);
            for (int j=0;j<i;j++){ thread_wait_wake_inline(&pool->workers[j].wait); v2_thread_join(pool->workers[j].handle); }
            v2_pool_destroy(pool); return NULL;
        }
    }
    return pool;
}

bool v2_pool_submit(WSPoolV2* pool, v2_task_fn fn, void* arg){
    if (!pool || !fn) return false;
    if (atomic_get_inline(&pool->stop)) return false;
    V2Task t = { fn, arg };

    /* reentrante (spawn): deque LOCAL de qualquer worker. Inline se cheio (evita self-deadlock). */
    V2Worker* me=g_v2_self;
    if (me && me->pool==pool){
        atomic_add_inline(&me->pending,1);
        if (v2_deque_push(&me->deque,&t)) return true;
        v2_run(me,&t);
        return true;
    }

    /* externo: round-robin SO nos n_core workers. */
    int nc=pool->n_core; int spins=0;
    for (;;){
        uint32_t start=atomic_u32_add_inline(&pool->submit_rr,1u);
        for (int probe=0;probe<nc;probe++){
            int idx=(int)((start+(uint32_t)probe)%(uint32_t)nc);
            V2Worker* w=&pool->workers[idx];
            atomic_add_inline(&w->pending,1);
            if (xring_push_mp(&w->inbox,w->inbox_buf,&t)){ thread_wait_wake_inline(&w->wait); return true; }
            atomic_sub_inline(&w->pending,1);
        }
        if (atomic_get_inline(&pool->stop)) return false;
        if      (spins<64)  xcpu_pause();
        else if (spins<256) v2_yield();
        else                v2_sleep0();
        spins++;
    }
}

static long v2_total_pending(WSPoolV2* pool){
    long s=0; for (int i=0;i<pool->n_workers;i++) s+=atomic_get_inline(&pool->workers[i].pending); return s;
}
void v2_pool_wait_idle(WSPoolV2* pool){ if(!pool)return; while (v2_total_pending(pool)>0) v2_sleep0(); }
void v2_pool_dims(WSPoolV2* pool, int* w, int* c){ if(!pool)return; if(w)*w=pool->n_workers; if(c)*c=pool->n_core; }

void v2_pool_destroy(WSPoolV2* pool){
    if (!pool) return;
    if (!atomic_get_inline(&pool->stop)){
        v2_pool_wait_idle(pool);
        atomic_set_inline(&pool->stop,1);
        if (pool->workers)
            for (int i=0;i<pool->n_workers;i++){
                thread_wait_wake_inline(&pool->workers[i].wait);
                if (pool->workers[i].handle) v2_thread_join(pool->workers[i].handle);
            }
    }
    if (pool->workers){
        for (int i=0;i<pool->n_workers;i++){
            v2_deque_free(&pool->workers[i].deque);
            free(pool->workers[i].inbox_buf);
            ring_queue_destroy(&pool->workers[i].inbox);
        }
        free(pool->workers);
    }
    free(pool);
    thread_wait_shutdown();
}
