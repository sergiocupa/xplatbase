/*
 * thread_pool_v2_03.c — V2.02 + worker elastico (ver .h).
 * Base identica ao V2.02 (arena shards + deque local + core/reserva), mais:
 *   - done_count / in_task por worker (single-writer, ~zero custo);
 *   - thread monitor que detecta presos + backlog e acorda elasticos;
 *   - workers elasticos (alem dos cores) que drenam e se aposentam.
 */

#include "thread_pool_v2_03.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../include/xplatbase.h"
#include "atomics.h"
#include "ring_queue.h"
#include "thread_wait.h"

#ifndef V203_CORE_NUM
#define V203_CORE_NUM      7
#endif
#ifndef V203_CORE_DEN
#define V203_CORE_DEN      10
#endif
#ifndef V203_ELASTIC_NUM
#define V203_ELASTIC_NUM   1      /* nº de elasticos = cores * 1/1 */
#endif
#ifndef V203_ELASTIC_DEN
#define V203_ELASTIC_DEN   1
#endif
#ifndef V203_SHARD_DIV
#define V203_SHARD_DIV     4
#endif
#ifndef V203_SHARD_CAP
#define V203_SHARD_CAP     4096
#endif
#ifndef V203_DEQUE_CAP
#define V203_DEQUE_CAP     4096
#endif
#ifndef V203_SPIN_PAUSE
#define V203_SPIN_PAUSE    512
#endif
#ifndef V203_SPIN_YIELD
#define V203_SPIN_YIELD    64
#endif
#ifndef V203_SPIN_SLEEP0
#define V203_SPIN_SLEEP0   8
#endif
#ifndef V203_PARK_TIMEOUT_US
#define V203_PARK_TIMEOUT_US 1000
#endif
#ifndef V203_MON_MS
#define V203_MON_MS        5       /* intervalo do monitor */
#endif
#ifndef V203_STUCK_MIN
#define V203_STUCK_MIN     2       /* min workers presos p/ acordar elastico */
#endif
#ifndef V203_ELASTIC_PARK_US
#define V203_ELASTIC_PARK_US 100000
#endif
#ifndef V203_ELASTIC_RETIRE_SPINS
#define V203_ELASTIC_RETIRE_SPINS 200000  /* polls vazios antes de aposentar */
#endif

#ifdef _MSC_VER
#define V203_INLINE static __forceinline
#else
#define V203_INLINE static inline __attribute__((always_inline))
#endif

#ifdef XPLATBASE_WIN
    #include <intrin.h>
    typedef HANDLE       v203_thread_t;
    typedef DWORD WINAPI v203_fn_sig(void*);
    static bool v203_thread_start(v203_thread_t* o, v203_fn_sig* fn, void* a){ *o=CreateThread(NULL,0,fn,a,0,NULL); return *o!=NULL; }
    static void v203_thread_join(v203_thread_t h){ WaitForSingleObject(h,INFINITE); CloseHandle(h); }
    static void v203_yield(void){ SwitchToThread(); }
    static void v203_sleep0(void){ Sleep(0); }
    #define V203_FN   DWORD WINAPI
    #define V203_RET  return 0
    typedef volatile LONG64 v203_a64;
    #define v203_ld(p)        (*(p))
    #define v203_st(p,v)      (*(p) = (LONG64)(v))
    #define v203_fence()      MemoryBarrier()
    static int v203_cas(v203_a64* p, LONG64 e, LONG64 d){ return InterlockedCompareExchange64(p,d,e)==e; }
#else
    #include <pthread.h>
    #include <sched.h>
    #include <time.h>
    #include <stdatomic.h>
    typedef pthread_t v203_thread_t;
    typedef void*     v203_fn_sig(void*);
    static bool v203_thread_start(v203_thread_t* o, v203_fn_sig* fn, void* a){ memset(o,0,sizeof(*o)); return pthread_create(o,NULL,fn,a)==0; }
    static void v203_thread_join(v203_thread_t h){ pthread_join(h,NULL); }
    static void v203_yield(void){ sched_yield(); }
    static void v203_sleep0(void){ struct timespec z={0,0}; nanosleep(&z,NULL); }
    #define V203_FN   void*
    #define V203_RET  return NULL
    typedef _Atomic long long v203_a64;
    #define v203_ld(p)        atomic_load_explicit((p), memory_order_relaxed)
    #define v203_st(p,v)      atomic_store_explicit((p),(long long)(v), memory_order_relaxed)
    #define v203_fence()      atomic_thread_fence(memory_order_seq_cst)
    static int v203_cas(v203_a64* p, long long e, long long d){ return atomic_compare_exchange_strong_explicit(p,&e,d,memory_order_seq_cst,memory_order_relaxed); }
#endif

typedef struct { v203_task_fn fn; void* arg; int ctr; } V203Task;

typedef struct XPL_ALIGN(XPL_CACHELINE) {
    v203_a64 top;
    char     _p0[XPL_CACHELINE - sizeof(v203_a64)];
    v203_a64 bottom;
    char     _p1[XPL_CACHELINE - sizeof(v203_a64)];
    V203Task* buf; int cap; int mask;
} V203Deque;

static bool v203_deque_init(V203Deque* d, int cap){
    d->buf=(V203Task*)malloc((size_t)cap*sizeof(V203Task)); if(!d->buf) return false;
    d->cap=cap; d->mask=cap-1; v203_st(&d->top,0); v203_st(&d->bottom,0); return true;
}
static void v203_deque_free(V203Deque* d){ free(d->buf); d->buf=NULL; }
V203_INLINE bool v203_deque_push(V203Deque* d, const V203Task* t){
    int64_t b=v203_ld(&d->bottom), tp=v203_ld(&d->top);
    if (b - tp >= d->cap) return false;
    d->buf[b & d->mask] = *t; v203_fence(); v203_st(&d->bottom, b+1); return true;
}
V203_INLINE bool v203_deque_take(V203Deque* d, V203Task* out){
    int64_t b=v203_ld(&d->bottom)-1; v203_st(&d->bottom,b); v203_fence();
    int64_t t=v203_ld(&d->top);
    if (t<=b){ *out=d->buf[b & d->mask];
        if (t==b){ int ok=v203_cas(&d->top,t,t+1); v203_st(&d->bottom,b+1); return ok!=0; } return true;
    } else { v203_st(&d->bottom,b+1); return false; }
}
V203_INLINE int v203_deque_steal(V203Deque* d, V203Task* out){
    int64_t t=v203_ld(&d->top); v203_fence(); int64_t b=v203_ld(&d->bottom);
    if (t<b){ V203Task x=d->buf[t & d->mask]; if(!v203_cas(&d->top,t,t+1)) return 0; *out=x; return 1; }
    return 0;
}

typedef struct XPL_ALIGN(XPL_CACHELINE) {
    RingQueue ring; void* buf; char _pad[XPL_CACHELINE];
} V203Shard;

typedef struct XPL_ALIGN(XPL_CACHELINE) {
    WSPoolV203*   pool;
    int           index;
    int           is_core;
    int           is_elastic;
    v203_thread_t handle;
    xwait_t       wait;
    V203Deque     deque;
    uint32_t      shard_cursor;
    uint32_t      steal_cursor;
    volatile long done_count;     /* progresso (single-writer = este worker) */
    volatile int  in_task;        /* 1 enquanto roda fn */
    long          mon_last;       /* uso exclusivo do monitor */
    xatomic_int   parked;         /* elastico: 1 = parqueado */
    char          _pad[XPL_CACHELINE];
} V203Worker;

struct WSPoolV203 {
    int            n_core;        /* workers que recebem submit externo / spinam */
    int            n_workers;     /* core + reserva (sem elasticos) */
    int            n_total;       /* n_workers + n_elastic */
    int            n_elastic;
    int            n_shards;
    V203Shard*     shards;
    V203Worker*    workers;       /* [0..n_total): [0,n_core)=core [n_core,n_workers)=reserva [n_workers,n_total)=elastico */
    xatomic_int*   ctrs;
    int            n_ctrs;        /* n_shards + n_total */
    xatomic_uint32 submit_rr;
    xatomic_uint32 wake_rr;
    xatomic_int    n_parked_core;
    xatomic_int    stop;
    v203_thread_t  mon_handle;
    xwait_t        mon_wait;
};

#ifdef XPLATBASE_WIN
static __declspec(thread) V203Worker* g_v203_self;
#else
static __thread V203Worker* g_v203_self;
#endif

V203_INLINE void v203_run(WSPoolV203* pool, V203Worker* self, V203Task* t){
    self->in_task=1;
    t->fn(t->arg);
    self->in_task=0;
    self->done_count++;            /* progresso */
    atomic_sub_inline(&pool->ctrs[t->ctr],1);
}

V203_INLINE bool v203_try_get(WSPoolV203* pool, V203Worker* self, V203Task* out){
    if (v203_deque_take(&self->deque, out)) return true;
    int G=pool->n_shards; uint32_t sc=self->shard_cursor;
    for (int g=0;g<G;g++){
        int s=(int)((sc+(uint32_t)g)%(uint32_t)G);
        if (xring_pop_mc(&pool->shards[s].ring, pool->shards[s].buf, out)){ self->shard_cursor=(uint32_t)s; return true; }
    }
    int nt=pool->n_total; uint32_t s2=self->steal_cursor;
    for (int k=1;k<=nt;k++){
        int v=(int)((s2+(uint32_t)k)%(uint32_t)nt);
        if (v==self->index) continue;
        if (v203_deque_steal(&pool->workers[v].deque, out)){ self->steal_cursor=(uint32_t)v; return true; }
    }
    return false;
}

static bool v203_spin(WSPoolV203* pool, V203Worker* self, V203Task* out){
    for (int i=0;i<V203_SPIN_PAUSE;i++){ if(v203_try_get(pool,self,out))return true; if(atomic_get_inline(&pool->stop))return false; xcpu_pause(); }
    for (int i=0;i<V203_SPIN_YIELD;i++){ if(v203_try_get(pool,self,out))return true; if(atomic_get_inline(&pool->stop))return false; v203_yield(); }
    for (int i=0;i<V203_SPIN_SLEEP0;i++){ if(v203_try_get(pool,self,out))return true; if(atomic_get_inline(&pool->stop))return false; v203_sleep0(); }
    return false;
}

static V203_FN v203_worker_fn(void* raw){
    V203Worker* self=(V203Worker*)raw; WSPoolV203* pool=self->pool; V203Task t;
    g_v203_self=self;

    if (self->is_elastic){
        while (!atomic_get_inline(&pool->stop)){
            atomic_set_inline(&self->parked,1);
            thread_wait_prepare_inline(&self->wait);
            if (atomic_get_inline(&self->parked)) thread_wait_sleep_for_inline(&self->wait, V203_ELASTIC_PARK_US);
            if (atomic_get_inline(&pool->stop)) break;
            if (atomic_get_inline(&self->parked)) continue;     /* timeout, nao ativado */
            int idle=0;                                  /* ativado: drena ate ociar */
            while (!atomic_get_inline(&pool->stop)){
                if (v203_try_get(pool,self,&t)){ v203_run(pool,self,&t); idle=0; continue; }
                if (++idle > V203_ELASTIC_RETIRE_SPINS) break;
                xcpu_pause();
            }
        }
        V203_RET;
    }

    int is_core=self->is_core;
    while (!atomic_get_inline(&pool->stop)){
        if (v203_try_get(pool,self,&t)){ v203_run(pool,self,&t); continue; }
        thread_wait_prepare_inline(&self->wait);
        if (v203_try_get(pool,self,&t)){ v203_run(pool,self,&t); continue; }
        if (is_core){ if (v203_spin(pool,self,&t)){ v203_run(pool,self,&t); continue; } }
        if (atomic_get_inline(&pool->stop)) break;
        if (is_core) atomic_add_inline(&pool->n_parked_core,1);
        thread_wait_sleep_for_inline(&self->wait, V203_PARK_TIMEOUT_US);
        if (is_core) atomic_sub_inline(&pool->n_parked_core,1);
    }
    V203_RET;
}

static V203_FN v203_monitor_fn(void* raw){
    WSPoolV203* pool=(WSPoolV203*)raw;
    thread_wait_prepare_inline(&pool->mon_wait);
    while (!atomic_get_inline(&pool->stop)){
        thread_wait_sleep_for_inline(&pool->mon_wait, V203_MON_MS*1000);
        if (atomic_get_inline(&pool->stop)) break;
        /* presos: in_task e sem progresso desde o ultimo tick */
        int stuck=0;
        for (int i=0;i<pool->n_workers;i++){
            V203Worker* w=&pool->workers[i];
            long dc=w->done_count;
            if (w->in_task && dc==w->mon_last) stuck++;
            w->mon_last=dc;
        }
        if (stuck < V203_STUCK_MIN) continue;
        int backlog=0;
        for (int s=0;s<pool->n_shards;s++) if(!ring_queue_empty(&pool->shards[s].ring)){ backlog=1; break; }
        if (!backlog) continue;
        for (int e=pool->n_workers;e<pool->n_total;e++){   /* acorda 1 elastico parqueado */
            V203Worker* w=&pool->workers[e];
            int exp=1;
            if (atomic_cas_inline(&w->parked,&exp,0)){ thread_wait_wake_inline(&w->wait); break; }
        }
    }
    V203_RET;
}

WSPoolV203* v203_pool_create(int cores_override){
    int cores=cores_override>0?cores_override:xcpu_count(); if(cores<1)cores=1;
    int nw=cores; if(nw<1)nw=1;
    int nc=cores*V203_CORE_NUM/V203_CORE_DEN; if(nc<1)nc=1; if(nc>nw)nc=nw;
    int ne=cores*V203_ELASTIC_NUM/V203_ELASTIC_DEN; if(ne<0)ne=0;
    int nt=nw+ne;
    int G=cores/V203_SHARD_DIV; if(G<1)G=1;

    WSPoolV203* pool=(WSPoolV203*)calloc(1,sizeof(WSPoolV203)); if(!pool) return NULL;
    pool->n_core=nc; pool->n_workers=nw; pool->n_elastic=ne; pool->n_total=nt; pool->n_shards=G;
    pool->n_ctrs=G+nt;
    pool->ctrs=(xatomic_int*)calloc((size_t)pool->n_ctrs,sizeof(xatomic_int));
    pool->shards=(V203Shard*)calloc((size_t)G,sizeof(V203Shard));
    pool->workers=(V203Worker*)calloc((size_t)nt,sizeof(V203Worker));
    if(!pool->ctrs||!pool->shards||!pool->workers){ v203_pool_destroy(pool); return NULL; }
    thread_wait_init(false);
    for (int s=0;s<G;s++){
        ring_queue_init(&pool->shards[s].ring, V203_SHARD_CAP);
        pool->shards[s].buf=malloc((size_t)V203_SHARD_CAP*sizeof(V203Task));
        if (pool->shards[s].ring.capacity==0 || !pool->shards[s].buf){ v203_pool_destroy(pool); return NULL; }
    }
    for (int i=0;i<nt;i++){
        V203Worker* w=&pool->workers[i];
        w->pool=pool; w->index=i; w->is_core=(i<nc)?1:0; w->is_elastic=(i>=nw)?1:0;
        w->shard_cursor=(uint32_t)(i%G); w->steal_cursor=(uint32_t)i;
        thread_wait_prepare_inline(&w->wait);
        atomic_set_inline(&w->parked, w->is_elastic?1:0);
        if (!v203_deque_init(&w->deque, V203_DEQUE_CAP)){ v203_pool_destroy(pool); return NULL; }
    }
    for (int i=0;i<nt;i++){
        if (!v203_thread_start(&pool->workers[i].handle, v203_worker_fn, &pool->workers[i])){
            atomic_set_inline(&pool->stop,1);
            for (int j=0;j<i;j++){ thread_wait_wake_inline(&pool->workers[j].wait); v203_thread_join(pool->workers[j].handle); }
            v203_pool_destroy(pool); return NULL;
        }
    }
    thread_wait_prepare_inline(&pool->mon_wait);
    if (!v203_thread_start(&pool->mon_handle, v203_monitor_fn, pool)){
        atomic_set_inline(&pool->stop,1);
        for (int i=0;i<nt;i++){ thread_wait_wake_inline(&pool->workers[i].wait); v203_thread_join(pool->workers[i].handle); }
        v203_pool_destroy(pool); return NULL;
    }
    return pool;
}

bool v203_pool_submit(WSPoolV203* pool, v203_task_fn fn, void* arg){
    if (!pool || !fn) return false;
    if (atomic_get_inline(&pool->stop)) return false;

    V203Worker* me=g_v203_self;
    if (me && me->pool==pool){       /* reentrante: deque local */
        V203Task t = { fn, arg, pool->n_shards + me->index };
        atomic_add_inline(&pool->ctrs[t.ctr],1);
        if (v203_deque_push(&me->deque,&t)) return true;
        v203_run(pool,me,&t);
        return true;
    }
    int G=pool->n_shards; int spins=0;
    for (;;){
        int s=(int)(atomic_u32_add_inline(&pool->submit_rr,1u)%(uint32_t)G);
        V203Task t = { fn, arg, s };
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
        else if (spins<256) v203_yield();
        else                v203_sleep0();
        spins++;
    }
}

static long v203_total_pending(WSPoolV203* pool){
    long s=0; for (int i=0;i<pool->n_ctrs;i++) s+=atomic_get_inline(&pool->ctrs[i]); return s;
}
void v203_pool_wait_idle(WSPoolV203* pool){ if(!pool)return; while (v203_total_pending(pool)>0) v203_sleep0(); }
void v203_pool_dims(WSPoolV203* pool, int* w, int* c){ if(!pool)return; if(w)*w=pool->n_workers; if(c)*c=pool->n_core; }

void v203_pool_destroy(WSPoolV203* pool){
    if (!pool) return;
    if (!atomic_get_inline(&pool->stop)){
        v203_pool_wait_idle(pool);
        atomic_set_inline(&pool->stop,1);
        thread_wait_wake_inline(&pool->mon_wait);
        if (pool->mon_handle) v203_thread_join(pool->mon_handle);
        if (pool->workers)
            for (int i=0;i<pool->n_total;i++){
                thread_wait_wake_inline(&pool->workers[i].wait);
                if (pool->workers[i].handle) v203_thread_join(pool->workers[i].handle);
            }
    }
    if (pool->workers){
        for (int i=0;i<pool->n_total;i++) v203_deque_free(&pool->workers[i].deque);
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
