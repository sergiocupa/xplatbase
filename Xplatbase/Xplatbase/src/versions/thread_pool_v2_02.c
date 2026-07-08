/*
 * thread_pool_v2_02.c — pool OFICIAL (V2.02): arena + core/reserva.
 * Ver thread_pool_v2_02.h. Consolidacao do design vencedor ("V4"):
 *   - submit externo -> G shards MPMC compartilhadas (round-robin); qualquer
 *     worker puxa de qualquer shard;
 *   - spawn (reentrante) -> deque Chase-Lev LOCAL; steal entre deques;
 *   - core/reserva: n_core core spinam e sao acordados; reserva e park-first e
 *     so engaja sob backlog;
 *   - pending por contador-indexado-na-task (baixa contencao, correto com steal).
 */

#include "thread_pool_v2_02.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../include/xplatbase.h"
#include "atomics.h"
#include "ring_queue.h"
#include "thread_wait.h"

#ifndef V2_CORE_NUM
#define V2_CORE_NUM      7      /* n_core = cores * 7/10 */
#endif
#ifndef V2_CORE_DEN
#define V2_CORE_DEN      10
#endif
#ifndef V2_SHARD_DIV
#define V2_SHARD_DIV     4      /* nº de shards = cores / 4 (min 1) */
#endif
#ifndef V2_SHARD_CAP
#define V2_SHARD_CAP     4096
#endif
#ifndef V2_DEQUE_CAP
#define V2_DEQUE_CAP     4096
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

/* task carrega 'ctr' = indice do contador de pending a decrementar ao rodar. */
typedef struct { v2_task_fn fn; void* arg; int ctr; } V2Task;

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
V2_INLINE int v2_deque_steal(V2Deque* d, V2Task* out){
    int64_t t=v2_ld(&d->top); v2_fence(); int64_t b=v2_ld(&d->bottom);
    if (t<b){ V2Task x=d->buf[t & d->mask]; if(!v2_cas(&d->top,t,t+1)) return 0; *out=x; return 1; }
    return 0;
}

typedef struct XPL_ALIGN(XPL_CACHELINE) {
    RingQueue ring;
    void*     buf;       /* V2Task[V2_SHARD_CAP] */
    char      _pad[XPL_CACHELINE];
} V2Shard;

typedef struct XPL_ALIGN(XPL_CACHELINE) {
    WSPoolV2*   pool;
    int         index;
    int         is_core;
    v2_thread_t handle;
    xwait_t     wait;
    V2Deque     deque;
    uint32_t    shard_cursor;
    uint32_t    steal_cursor;
    char        _pad[XPL_CACHELINE];
} V2Worker;

struct WSPoolV2 {
    int            n_workers;
    int            n_core;
    int            n_shards;
    V2Shard*       shards;
    V2Worker*      workers;
    xatomic_int*   ctrs;          /* [0..n_shards-1] externos; [n_shards..+n_workers-1] locais */
    int            n_ctrs;
    xatomic_uint32 submit_rr;
    xatomic_uint32 wake_rr;
    xatomic_int    n_parked_core;
    xatomic_int    stop;
};

#ifdef XPLATBASE_WIN
static __declspec(thread) V2Worker* g_v2_self;
#else
static __thread V2Worker* g_v2_self;
#endif

V2_INLINE void v2_run(WSPoolV2* pool, V2Task* t){ t->fn(t->arg); atomic_sub_inline(&pool->ctrs[t->ctr],1); }

V2_INLINE bool v2_try_get(WSPoolV2* pool, V2Worker* self, V2Task* out){
    if (v2_deque_take(&self->deque, out)) return true;
    int G=pool->n_shards; uint32_t sc=self->shard_cursor;
    for (int g=0;g<G;g++){
        int s=(int)((sc+(uint32_t)g)%(uint32_t)G);
        if (xring_pop_mc(&pool->shards[s].ring, pool->shards[s].buf, out)){ self->shard_cursor=(uint32_t)s; return true; }
    }
    int nw=pool->n_workers; uint32_t s2=self->steal_cursor;
    for (int k=1;k<=nw;k++){
        int v=(int)((s2+(uint32_t)k)%(uint32_t)nw);
        if (v==self->index) continue;
        if (v2_deque_steal(&pool->workers[v].deque, out)){ self->steal_cursor=(uint32_t)v; return true; }
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
    int is_core=self->is_core;
    while (!atomic_get_inline(&pool->stop)){
        if (v2_try_get(pool,self,&t)){ v2_run(pool,&t); continue; }
        thread_wait_prepare_inline(&self->wait);
        if (v2_try_get(pool,self,&t)){ v2_run(pool,&t); continue; }

        if (is_core){
            if (v2_spin(pool,self,&t)){ v2_run(pool,&t); continue; }
        }
        if (atomic_get_inline(&pool->stop)) break;
        if (is_core) atomic_add_inline(&pool->n_parked_core,1);
        thread_wait_sleep_for_inline(&self->wait, V2_PARK_TIMEOUT_US);
        if (is_core) atomic_sub_inline(&pool->n_parked_core,1);
    }
    V2_RET;
}

WSPoolV2* v2_pool_create(int cores_override){
    int cores=cores_override>0?cores_override:xcpu_count(); if(cores<1)cores=1;
    int nw=cores; if(nw<1)nw=1;
    int nc=cores*V2_CORE_NUM/V2_CORE_DEN; if(nc<1)nc=1; if(nc>nw)nc=nw;
    int G=cores/V2_SHARD_DIV; if(G<1)G=1;

    WSPoolV2* pool=(WSPoolV2*)calloc(1,sizeof(WSPoolV2)); if(!pool) return NULL;
    pool->n_workers=nw; pool->n_core=nc; pool->n_shards=G; pool->n_ctrs=G+nw;
    pool->ctrs=(xatomic_int*)calloc((size_t)pool->n_ctrs,sizeof(xatomic_int));
    pool->shards=(V2Shard*)calloc((size_t)G,sizeof(V2Shard));
    pool->workers=(V2Worker*)calloc((size_t)nw,sizeof(V2Worker));
    if(!pool->ctrs||!pool->shards||!pool->workers){ v2_pool_destroy(pool); return NULL; }
    thread_wait_init(false);
    for (int s=0;s<G;s++){
        ring_queue_init(&pool->shards[s].ring, V2_SHARD_CAP);
        pool->shards[s].buf=malloc((size_t)V2_SHARD_CAP*sizeof(V2Task));
        if (pool->shards[s].ring.capacity==0 || !pool->shards[s].buf){ v2_pool_destroy(pool); return NULL; }
    }
    for (int i=0;i<nw;i++){
        V2Worker* w=&pool->workers[i];
        w->pool=pool; w->index=i; w->is_core=(i<nc)?1:0;
        w->shard_cursor=(uint32_t)(i%G); w->steal_cursor=(uint32_t)i;
        thread_wait_prepare_inline(&w->wait);
        if (!v2_deque_init(&w->deque, V2_DEQUE_CAP)){ v2_pool_destroy(pool); return NULL; }
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

    /* reentrante (spawn): deque LOCAL; contador local = n_shards + index. */
    V2Worker* me=g_v2_self;
    if (me && me->pool==pool){
        V2Task t = { fn, arg, pool->n_shards + me->index };
        atomic_add_inline(&pool->ctrs[t.ctr],1);
        if (v2_deque_push(&me->deque,&t)) return true;
        v2_run(pool,&t);                       /* deque cheio: inline (sem self-deadlock) */
        return true;
    }

    /* externo: round-robin nas shards COMPARTILHADAS; contador = shard. */
    int G=pool->n_shards; int spins=0;
    for (;;){
        int s=(int)(atomic_u32_add_inline(&pool->submit_rr,1u)%(uint32_t)G);
        V2Task t = { fn, arg, s };
        atomic_add_inline(&pool->ctrs[s],1);
        if (xring_push_mp(&pool->shards[s].ring, pool->shards[s].buf, &t)){
            if (atomic_get_inline(&pool->n_parked_core) > 0){
                uint32_t k=atomic_u32_add_inline(&pool->wake_rr,1u)%(uint32_t)pool->n_core;
                thread_wait_wake_inline(&pool->workers[k].wait);
            }
            return true;
        }
        atomic_sub_inline(&pool->ctrs[s],1);
        if (atomic_get_inline(&pool->stop)) return false;
        if      (spins<64)  xcpu_pause();
        else if (spins<256) v2_yield();
        else                v2_sleep0();
        spins++;
    }
}

static long v2_total_pending(WSPoolV2* pool){
    long s=0; for (int i=0;i<pool->n_ctrs;i++) s+=atomic_get_inline(&pool->ctrs[i]); return s;
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
        for (int i=0;i<pool->n_workers;i++) v2_deque_free(&pool->workers[i].deque);
        free(pool->workers);
    }
    if (pool->shards){
        for (int s=0;s<pool->n_shards;s++){ free(pool->shards[s].buf); ring_queue_destroy(&pool->shards[s].ring); }
        free(pool->shards);
    }
    free(pool->ctrs);
    free(pool);
    thread_wait_shutdown();
}
