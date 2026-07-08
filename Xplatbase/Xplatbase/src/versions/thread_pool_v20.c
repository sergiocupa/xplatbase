/*
 * thread_pool_v20.c — V4 + spinner cap dinamico (ver .h).
 * Igual ao V4, mas no caminho OCIOSO o worker so faz spin progressivo se houver
 * vaga no cap de spinners; senao parqueia direto (poupa CPU e corta a
 * oversubscription que gera a cauda no flat). Execucao de task nunca e limitada.
 */

#include "thread_pool_v20.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../include/xplatbase.h"
#include "atomics.h"
#include "ring_queue.h"
#include "thread_wait.h"

#ifndef V20_WORKER_NUM
#define V20_WORKER_NUM   1
#endif
#ifndef V20_WORKER_DEN
#define V20_WORKER_DEN   1
#endif
#ifndef V20_SPIN_CAP_NUM
#define V20_SPIN_CAP_NUM 3      /* spin_cap = cores * 3/4 */
#endif
#ifndef V20_SPIN_CAP_DEN
#define V20_SPIN_CAP_DEN 4
#endif
#ifndef V20_DEQUE_CAP
#define V20_DEQUE_CAP    4096
#endif
#ifndef V20_INBOX_CAP
#define V20_INBOX_CAP    1024
#endif
#ifndef V20_DRAIN_BATCH
#define V20_DRAIN_BATCH  64
#endif
#ifndef V20_SPIN_PAUSE
#define V20_SPIN_PAUSE   512
#endif
#ifndef V20_SPIN_YIELD
#define V20_SPIN_YIELD   64
#endif
#ifndef V20_SPIN_SLEEP0
#define V20_SPIN_SLEEP0  8
#endif
#ifndef V20_PARK_TIMEOUT_US
#define V20_PARK_TIMEOUT_US 1000
#endif

#ifdef _MSC_VER
#define V20_INLINE static __forceinline
#else
#define V20_INLINE static inline __attribute__((always_inline))
#endif

#ifdef XPLATBASE_WIN
    #include <intrin.h>
    typedef HANDLE       v20_thread_t;
    typedef DWORD WINAPI v20_fn_sig(void*);
    static bool v20_thread_start(v20_thread_t* o, v20_fn_sig* fn, void* a){ *o=CreateThread(NULL,0,fn,a,0,NULL); return *o!=NULL; }
    static void v20_thread_join(v20_thread_t h){ WaitForSingleObject(h,INFINITE); CloseHandle(h); }
    static void v20_yield(void){ SwitchToThread(); }
    static void v20_sleep0(void){ Sleep(0); }
    #define V20_FN   DWORD WINAPI
    #define V20_RET  return 0
    typedef volatile LONG64 v20_a64;
    #define v20_ld(p)        (*(p))
    #define v20_st(p,v)      (*(p) = (LONG64)(v))
    #define v20_fence()      MemoryBarrier()
    static int v20_cas(v20_a64* p, LONG64 e, LONG64 d){ return InterlockedCompareExchange64(p,d,e)==e; }
#else
    #include <pthread.h>
    #include <sched.h>
    #include <time.h>
    #include <stdatomic.h>
    typedef pthread_t v20_thread_t;
    typedef void*     v20_fn_sig(void*);
    static bool v20_thread_start(v20_thread_t* o, v20_fn_sig* fn, void* a){ memset(o,0,sizeof(*o)); return pthread_create(o,NULL,fn,a)==0; }
    static void v20_thread_join(v20_thread_t h){ pthread_join(h,NULL); }
    static void v20_yield(void){ sched_yield(); }
    static void v20_sleep0(void){ struct timespec z={0,0}; nanosleep(&z,NULL); }
    #define V20_FN   void*
    #define V20_RET  return NULL
    typedef _Atomic long long v20_a64;
    #define v20_ld(p)        atomic_load_explicit((p), memory_order_relaxed)
    #define v20_st(p,v)      atomic_store_explicit((p),(long long)(v), memory_order_relaxed)
    #define v20_fence()      atomic_thread_fence(memory_order_seq_cst)
    static int v20_cas(v20_a64* p, long long e, long long d){ return atomic_compare_exchange_strong_explicit(p,&e,d,memory_order_seq_cst,memory_order_relaxed); }
#endif

typedef struct { v20_task_fn fn; void* arg; } V20Task;

typedef struct XPL_ALIGN(XPL_CACHELINE) {
    v20_a64 top;
    char    _p0[XPL_CACHELINE - sizeof(v20_a64)];
    v20_a64 bottom;
    char    _p1[XPL_CACHELINE - sizeof(v20_a64)];
    V20Task* buf; int cap; int mask;
} V20Deque;

static bool v20_deque_init(V20Deque* d, int cap){
    d->buf=(V20Task*)malloc((size_t)cap*sizeof(V20Task)); if(!d->buf) return false;
    d->cap=cap; d->mask=cap-1; v20_st(&d->top,0); v20_st(&d->bottom,0); return true;
}
static void v20_deque_free(V20Deque* d){ free(d->buf); d->buf=NULL; }

V20_INLINE bool v20_deque_push(V20Deque* d, const V20Task* t){
    int64_t b=v20_ld(&d->bottom), tp=v20_ld(&d->top);
    if (b - tp >= d->cap) return false;
    d->buf[b & d->mask] = *t;
    v20_fence();
    v20_st(&d->bottom, b+1);
    return true;
}
V20_INLINE bool v20_deque_take(V20Deque* d, V20Task* out){
    int64_t b=v20_ld(&d->bottom)-1; v20_st(&d->bottom,b); v20_fence();
    int64_t t=v20_ld(&d->top);
    if (t<=b){
        *out=d->buf[b & d->mask];
        if (t==b){ int ok=v20_cas(&d->top,t,t+1); v20_st(&d->bottom,b+1); return ok!=0; }
        return true;
    } else { v20_st(&d->bottom,b+1); return false; }
}
typedef enum { V20_OK, V20_EMPTY, V20_ABORT } V20Steal;
V20_INLINE V20Steal v20_deque_steal(V20Deque* d, V20Task* out){
    int64_t t=v20_ld(&d->top); v20_fence(); int64_t b=v20_ld(&d->bottom);
    if (t<b){ V20Task x=d->buf[t & d->mask]; if(!v20_cas(&d->top,t,t+1)) return V20_ABORT; *out=x; return V20_OK; }
    return V20_EMPTY;
}

typedef struct XPL_ALIGN(XPL_CACHELINE) {
    WSPoolV20*   pool;
    int          index;
    v20_thread_t handle;
    xwait_t      wait;
    V20Deque     deque;
    RingQueue    inbox;
    void*        inbox_buf;
    uint32_t     steal_cursor;
    xatomic_int  pending;
    char         _pad[XPL_CACHELINE];
} V20Worker;

struct WSPoolV20 {
    int            n_workers;
    int            spin_cap;        /* teto de spinners ociosos simultaneos */
    xatomic_int    spinners;        /* spinners ociosos correntes */
    V20Worker*     workers;
    xatomic_uint32 submit_rr;
    xatomic_int    stop;
};

#ifdef XPLATBASE_WIN
static __declspec(thread) V20Worker* g_v20_self;
#else
static __thread V20Worker* g_v20_self;
#endif

V20_INLINE void v20_run(V20Worker* self, V20Task* t){ t->fn(t->arg); atomic_sub_inline(&self->pending,1); }

V20_INLINE int v20_drain_inbox(V20Worker* self){
    int moved=0; V20Task tmp;
    for (int k=0;k<V20_DRAIN_BATCH;k++){
        if (!xring_pop_mc(&self->inbox, self->inbox_buf, &tmp)) break;
        if (!v20_deque_push(&self->deque, &tmp)) v20_run(self, &tmp);
        moved++;
    }
    return moved;
}

V20_INLINE bool v20_try_get(WSPoolV20* pool, V20Worker* self, V20Task* out){
    if (v20_deque_take(&self->deque, out)) return true;
    if (v20_drain_inbox(self) > 0 && v20_deque_take(&self->deque, out)) return true;
    int nw=pool->n_workers; uint32_t s=self->steal_cursor;
    for (int k=1;k<=nw;k++){
        int v=(int)((s+(uint32_t)k)%(uint32_t)nw);
        if (v==self->index) continue;
        V20Worker* victim=&pool->workers[v];
        if (v20_deque_steal(&victim->deque, out)==V20_OK){ self->steal_cursor=(uint32_t)v; return true; }
        if (!ring_queue_empty(&victim->inbox) && xring_pop_mc(&victim->inbox, victim->inbox_buf, out)){
            self->steal_cursor=(uint32_t)v; return true;
        }
    }
    return false;
}

static bool v20_spin(WSPoolV20* pool, V20Worker* self, V20Task* out){
    for (int i=0;i<V20_SPIN_PAUSE;i++){ if(v20_try_get(pool,self,out))return true; if(atomic_get_inline(&pool->stop))return false; xcpu_pause(); }
    for (int i=0;i<V20_SPIN_YIELD;i++){ if(v20_try_get(pool,self,out))return true; if(atomic_get_inline(&pool->stop))return false; v20_yield(); }
    for (int i=0;i<V20_SPIN_SLEEP0;i++){ if(v20_try_get(pool,self,out))return true; if(atomic_get_inline(&pool->stop))return false; v20_sleep0(); }
    return false;
}

/* spinner cap: limita quantos workers OCIOSOS spinam ao mesmo tempo. */
V20_INLINE bool v20_spinner_enter(WSPoolV20* pool){
    for(;;){
        int cur=atomic_get_inline(&pool->spinners);
        if (cur >= pool->spin_cap) return false;
        int exp=cur;
        if (atomic_cas_inline(&pool->spinners, &exp, cur+1)) return true;
    }
}
V20_INLINE void v20_spinner_leave(WSPoolV20* pool){ atomic_sub_inline(&pool->spinners,1); }

static V20_FN v20_worker_fn(void* raw){
    V20Worker* self=(V20Worker*)raw; WSPoolV20* pool=self->pool; V20Task t;
    g_v20_self=self;
    while (!atomic_get_inline(&pool->stop)){
        if (v20_try_get(pool,self,&t)){ v20_run(self,&t); continue; }
        thread_wait_prepare_inline(&self->wait);
        if (v20_try_get(pool,self,&t)){ v20_run(self,&t); continue; }

        if (v20_spinner_enter(pool)){
            bool got = v20_spin(pool,self,&t);
            v20_spinner_leave(pool);
            if (got){ v20_run(self,&t); continue; }
        }
        /* sem vaga de spin (ou spin nao achou): parqueia; acorda por demanda. */
        if (atomic_get_inline(&pool->stop)) break;
        thread_wait_sleep_for_inline(&self->wait, V20_PARK_TIMEOUT_US);
    }
    V20_RET;
}

WSPoolV20* v20_pool_create(int cores_override){
    int cores=cores_override>0?cores_override:xcpu_count(); if(cores<1)cores=1;
    int nw=cores*V20_WORKER_NUM/V20_WORKER_DEN; if(nw<1)nw=1;
    int cap=cores*V20_SPIN_CAP_NUM/V20_SPIN_CAP_DEN; if(cap<1)cap=1; if(cap>nw)cap=nw;

    WSPoolV20* pool=(WSPoolV20*)calloc(1,sizeof(WSPoolV20)); if(!pool) return NULL;
    pool->n_workers=nw; pool->spin_cap=cap;
    pool->workers=(V20Worker*)calloc((size_t)nw,sizeof(V20Worker)); if(!pool->workers){ free(pool); return NULL; }
    thread_wait_init(false);
    for (int i=0;i<nw;i++){
        V20Worker* w=&pool->workers[i];
        w->pool=pool; w->index=i; w->steal_cursor=(uint32_t)i;
        thread_wait_prepare_inline(&w->wait);
        if (!v20_deque_init(&w->deque, V20_DEQUE_CAP)){ v20_pool_destroy(pool); return NULL; }
        ring_queue_init(&w->inbox, V20_INBOX_CAP);
        w->inbox_buf=malloc((size_t)V20_INBOX_CAP*sizeof(V20Task));
        if (w->inbox.capacity==0 || !w->inbox_buf){ v20_pool_destroy(pool); return NULL; }
    }
    for (int i=0;i<nw;i++){
        if (!v20_thread_start(&pool->workers[i].handle, v20_worker_fn, &pool->workers[i])){
            atomic_set_inline(&pool->stop,1);
            for (int j=0;j<i;j++){ thread_wait_wake_inline(&pool->workers[j].wait); v20_thread_join(pool->workers[j].handle); }
            v20_pool_destroy(pool); return NULL;
        }
    }
    return pool;
}

bool v20_pool_submit(WSPoolV20* pool, v20_task_fn fn, void* arg){
    if (!pool || !fn) return false;
    if (atomic_get_inline(&pool->stop)) return false;
    V20Task t = { fn, arg };

    V20Worker* me=g_v20_self;
    if (me && me->pool==pool){
        atomic_add_inline(&me->pending,1);
        if (v20_deque_push(&me->deque,&t)) return true;
        v20_run(me,&t);
        return true;
    }

    int nw=pool->n_workers; int spins=0;
    for (;;){
        uint32_t start=atomic_u32_add_inline(&pool->submit_rr,1u);
        for (int probe=0;probe<nw;probe++){
            int idx=(int)((start+(uint32_t)probe)%(uint32_t)nw);
            V20Worker* w=&pool->workers[idx];
            atomic_add_inline(&w->pending,1);
            if (xring_push_mp(&w->inbox,w->inbox_buf,&t)){ thread_wait_wake_inline(&w->wait); return true; }
            atomic_sub_inline(&w->pending,1);
        }
        if (atomic_get_inline(&pool->stop)) return false;
        if      (spins<64)  xcpu_pause();
        else if (spins<256) v20_yield();
        else                v20_sleep0();
        spins++;
    }
}

static long v20_total_pending(WSPoolV20* pool){
    long s=0; for (int i=0;i<pool->n_workers;i++) s+=atomic_get_inline(&pool->workers[i].pending); return s;
}
void v20_pool_wait_idle(WSPoolV20* pool){ if(!pool)return; while (v20_total_pending(pool)>0) v20_sleep0(); }
void v20_pool_dims(WSPoolV20* pool, int* w, int* l){ if(!pool)return; if(w)*w=pool->n_workers; if(l)*l=pool->spin_cap; }

void v20_pool_destroy(WSPoolV20* pool){
    if (!pool) return;
    if (!atomic_get_inline(&pool->stop)){
        v20_pool_wait_idle(pool);
        atomic_set_inline(&pool->stop,1);
        if (pool->workers)
            for (int i=0;i<pool->n_workers;i++){
                thread_wait_wake_inline(&pool->workers[i].wait);
                if (pool->workers[i].handle) v20_thread_join(pool->workers[i].handle);
            }
    }
    if (pool->workers){
        for (int i=0;i<pool->n_workers;i++){
            v20_deque_free(&pool->workers[i].deque);
            free(pool->workers[i].inbox_buf);
            ring_queue_destroy(&pool->workers[i].inbox);
        }
        free(pool->workers);
    }
    free(pool);
    thread_wait_shutdown();
}
