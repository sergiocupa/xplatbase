/*
 * thread_pool_v3.c — pool estilo ARENA (fila de submit COMPARTILHADA).
 *
 *   - Submit EXTERNO vai para G filas MPMC COMPARTILHADAS (shards), round-robin.
 *     Qualquer worker puxa de qualquer shard -> a task nao fica presa a um dono.
 *     Em regime quente, os workers se auto-servem das shards e o produtor so
 *     ENFILEIRA (acorda alguem apenas se houver core parqueado) -> submit barato.
 *   - Spawn (reentrante) usa deque Chase-Lev LOCAL do worker (push/take sem CAS).
 *   - Steal: entre deques dos workers.
 *
 *   Flag de compilacao V3_CORE_RESERVE:
 *     0 (V3): todos os workers sao "core" (spinam ociosos, auto-servem).
 *     1 (V4): n_core = cores*7/10 spinam e sao acordados pelo submit; os demais
 *             (reserva) sao park-first e so engajam quando acham backlog (spawn).
 *
 *   pending: contadores indexados pela task (cada task lembra qual contador
 *   decrementar) -> baixa contencao e correto com steal.
 */

#include "thread_pool_v3.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../include/xplatbase.h"
#include "atomics.h"
#include "ring_queue.h"
#include "thread_wait.h"

#ifndef V3_CORE_RESERVE
#define V3_CORE_RESERVE  0      /* 0 = V3 (todos core); 1 = V4 (core + reserva) */
#endif
#ifndef V3_CORE_NUM
#define V3_CORE_NUM      7      /* se CORE_RESERVE: n_core = cores * 7/10 */
#endif
#ifndef V3_CORE_DEN
#define V3_CORE_DEN      10
#endif
#ifndef V3_SHARD_DIV
#define V3_SHARD_DIV     4      /* nº de shards = cores / 4 (min 1) */
#endif
#ifndef V3_SHARD_CAP
#define V3_SHARD_CAP     4096
#endif
#ifndef V3_DEQUE_CAP
#define V3_DEQUE_CAP     4096
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

#ifdef _MSC_VER
#define V3_INLINE static __forceinline
#else
#define V3_INLINE static inline __attribute__((always_inline))
#endif

#ifdef XPLATBASE_WIN
    #include <intrin.h>
    typedef HANDLE       v3_thread_t;
    typedef DWORD WINAPI v3_fn_sig(void*);
    static bool v3_thread_start(v3_thread_t* o, v3_fn_sig* fn, void* a){ *o=CreateThread(NULL,0,fn,a,0,NULL); return *o!=NULL; }
    static void v3_thread_join(v3_thread_t h){ WaitForSingleObject(h,INFINITE); CloseHandle(h); }
    static void v3_yield(void){ SwitchToThread(); }
    static void v3_sleep0(void){ Sleep(0); }
    #define V3_FN   DWORD WINAPI
    #define V3_RET  return 0
    typedef volatile LONG64 v3_a64;
    #define v3_ld(p)        (*(p))
    #define v3_st(p,v)      (*(p) = (LONG64)(v))
    #define v3_fence()      MemoryBarrier()
    static int v3_cas(v3_a64* p, LONG64 e, LONG64 d){ return InterlockedCompareExchange64(p,d,e)==e; }
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
    #define V3_FN   void*
    #define V3_RET  return NULL
    typedef _Atomic long long v3_a64;
    #define v3_ld(p)        atomic_load_explicit((p), memory_order_relaxed)
    #define v3_st(p,v)      atomic_store_explicit((p),(long long)(v), memory_order_relaxed)
    #define v3_fence()      atomic_thread_fence(memory_order_seq_cst)
    static int v3_cas(v3_a64* p, long long e, long long d){ return atomic_compare_exchange_strong_explicit(p,&e,d,memory_order_seq_cst,memory_order_relaxed); }
#endif

/* task carrega 'ctr' = indice do contador de pending a decrementar quando rodar. */
typedef struct { v3_task_fn fn; void* arg; int ctr; } V3Task;

typedef struct XPL_ALIGN(XPL_CACHELINE) {
    v3_a64 top;
    char   _p0[XPL_CACHELINE - sizeof(v3_a64)];
    v3_a64 bottom;
    char   _p1[XPL_CACHELINE - sizeof(v3_a64)];
    V3Task* buf; int cap; int mask;
} V3Deque;

static bool v3_deque_init(V3Deque* d, int cap){
    d->buf=(V3Task*)malloc((size_t)cap*sizeof(V3Task)); if(!d->buf) return false;
    d->cap=cap; d->mask=cap-1; v3_st(&d->top,0); v3_st(&d->bottom,0); return true;
}
static void v3_deque_free(V3Deque* d){ free(d->buf); d->buf=NULL; }

V3_INLINE bool v3_deque_push(V3Deque* d, const V3Task* t){
    int64_t b=v3_ld(&d->bottom), tp=v3_ld(&d->top);
    if (b - tp >= d->cap) return false;
    d->buf[b & d->mask] = *t; v3_fence(); v3_st(&d->bottom, b+1); return true;
}
V3_INLINE bool v3_deque_take(V3Deque* d, V3Task* out){
    int64_t b=v3_ld(&d->bottom)-1; v3_st(&d->bottom,b); v3_fence();
    int64_t t=v3_ld(&d->top);
    if (t<=b){
        *out=d->buf[b & d->mask];
        if (t==b){ int ok=v3_cas(&d->top,t,t+1); v3_st(&d->bottom,b+1); return ok!=0; }
        return true;
    } else { v3_st(&d->bottom,b+1); return false; }
}
V3_INLINE int v3_deque_steal(V3Deque* d, V3Task* out){
    int64_t t=v3_ld(&d->top); v3_fence(); int64_t b=v3_ld(&d->bottom);
    if (t<b){ V3Task x=d->buf[t & d->mask]; if(!v3_cas(&d->top,t,t+1)) return 0; *out=x; return 1; }
    return 0;
}

typedef struct XPL_ALIGN(XPL_CACHELINE) {
    RingQueue ring;
    void*     buf;       /* V3Task[V3_SHARD_CAP] */
    char      _pad[XPL_CACHELINE];
} V3Shard;

typedef struct XPL_ALIGN(XPL_CACHELINE) {
    WSPoolV3*   pool;
    int         index;
    int         is_core;
    v3_thread_t handle;
    xwait_t     wait;
    V3Deque     deque;
    uint32_t    shard_cursor;
    uint32_t    steal_cursor;
    char        _pad[XPL_CACHELINE];
} V3Worker;

struct WSPoolV3 {
    int            n_workers;
    int            n_core;
    int            n_shards;
    V3Shard*       shards;
    V3Worker*      workers;
    xatomic_int*   ctrs;          /* [0..n_shards-1] externos; [n_shards..+n_workers-1] locais */
    int            n_ctrs;
    xatomic_uint32 submit_rr;
    xatomic_uint32 wake_rr;
    xatomic_int    n_parked_core; /* nº de workers CORE parqueados (gate do wake) */
    xatomic_int    stop;
};

#ifdef XPLATBASE_WIN
static __declspec(thread) V3Worker* g_v3_self;
#else
static __thread V3Worker* g_v3_self;
#endif

V3_INLINE void v3_run(WSPoolV3* pool, V3Task* t){ t->fn(t->arg); atomic_sub_inline(&pool->ctrs[t->ctr],1); }

V3_INLINE bool v3_try_get(WSPoolV3* pool, V3Worker* self, V3Task* out){
    if (v3_deque_take(&self->deque, out)) return true;
    /* puxa das shards compartilhadas */
    int G=pool->n_shards; uint32_t sc=self->shard_cursor;
    for (int g=0;g<G;g++){
        int s=(int)((sc+(uint32_t)g)%(uint32_t)G);
        if (xring_pop_mc(&pool->shards[s].ring, pool->shards[s].buf, out)){ self->shard_cursor=(uint32_t)s; return true; }
    }
    /* steal entre deques */
    int nw=pool->n_workers; uint32_t s2=self->steal_cursor;
    for (int k=1;k<=nw;k++){
        int v=(int)((s2+(uint32_t)k)%(uint32_t)nw);
        if (v==self->index) continue;
        if (v3_deque_steal(&pool->workers[v].deque, out)){ self->steal_cursor=(uint32_t)v; return true; }
    }
    return false;
}

static bool v3_spin(WSPoolV3* pool, V3Worker* self, V3Task* out){
    for (int i=0;i<V3_SPIN_PAUSE;i++){ if(v3_try_get(pool,self,out))return true; if(atomic_get_inline(&pool->stop))return false; xcpu_pause(); }
    for (int i=0;i<V3_SPIN_YIELD;i++){ if(v3_try_get(pool,self,out))return true; if(atomic_get_inline(&pool->stop))return false; v3_yield(); }
    for (int i=0;i<V3_SPIN_SLEEP0;i++){ if(v3_try_get(pool,self,out))return true; if(atomic_get_inline(&pool->stop))return false; v3_sleep0(); }
    return false;
}

static V3_FN v3_worker_fn(void* raw){
    V3Worker* self=(V3Worker*)raw; WSPoolV3* pool=self->pool; V3Task t;
    g_v3_self=self;
    int is_core=self->is_core;
    while (!atomic_get_inline(&pool->stop)){
        if (v3_try_get(pool,self,&t)){ v3_run(pool,&t); continue; }
        thread_wait_prepare_inline(&self->wait);
        if (v3_try_get(pool,self,&t)){ v3_run(pool,&t); continue; }

        if (is_core){
            if (v3_spin(pool,self,&t)){ v3_run(pool,&t); continue; }
        }
        if (atomic_get_inline(&pool->stop)) break;
        if (is_core) atomic_add_inline(&pool->n_parked_core,1);   /* gate do wake do submit */
        thread_wait_sleep_for_inline(&self->wait, V3_PARK_TIMEOUT_US);
        if (is_core) atomic_sub_inline(&pool->n_parked_core,1);
    }
    V3_RET;
}

WSPoolV3* v3_pool_create(int cores_override){
    int cores=cores_override>0?cores_override:xcpu_count(); if(cores<1)cores=1;
    int nw=cores; if(nw<1)nw=1;
#if V3_CORE_RESERVE
    int nc=cores*V3_CORE_NUM/V3_CORE_DEN; if(nc<1)nc=1; if(nc>nw)nc=nw;
#else
    int nc=nw;
#endif
    int G=cores/V3_SHARD_DIV; if(G<1)G=1;

    WSPoolV3* pool=(WSPoolV3*)calloc(1,sizeof(WSPoolV3)); if(!pool) return NULL;
    pool->n_workers=nw; pool->n_core=nc; pool->n_shards=G;
    pool->n_ctrs=G+nw;
    pool->ctrs=(xatomic_int*)calloc((size_t)pool->n_ctrs,sizeof(xatomic_int));
    pool->shards=(V3Shard*)calloc((size_t)G,sizeof(V3Shard));
    pool->workers=(V3Worker*)calloc((size_t)nw,sizeof(V3Worker));
    if(!pool->ctrs||!pool->shards||!pool->workers){ v3_pool_destroy(pool); return NULL; }
    thread_wait_init(false);
    for (int s=0;s<G;s++){
        ring_queue_init(&pool->shards[s].ring, V3_SHARD_CAP);
        pool->shards[s].buf=malloc((size_t)V3_SHARD_CAP*sizeof(V3Task));
        if (pool->shards[s].ring.capacity==0 || !pool->shards[s].buf){ v3_pool_destroy(pool); return NULL; }
    }
    for (int i=0;i<nw;i++){
        V3Worker* w=&pool->workers[i];
        w->pool=pool; w->index=i; w->is_core=(i<nc)?1:0;
        w->shard_cursor=(uint32_t)(i%G); w->steal_cursor=(uint32_t)i;
        thread_wait_prepare_inline(&w->wait);
        if (!v3_deque_init(&w->deque, V3_DEQUE_CAP)){ v3_pool_destroy(pool); return NULL; }
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

    /* reentrante (spawn): deque LOCAL; contador local = n_shards + index. */
    V3Worker* me=g_v3_self;
    if (me && me->pool==pool){
        V3Task t = { fn, arg, pool->n_shards + me->index };
        atomic_add_inline(&pool->ctrs[t.ctr],1);
        if (v3_deque_push(&me->deque,&t)) return true;
        v3_run(pool,&t);                       /* deque cheio: inline (sem self-deadlock) */
        return true;
    }

    /* externo: round-robin nas shards COMPARTILHADAS; contador = shard. */
    int G=pool->n_shards; int spins=0;
    for (;;){
        int s=(int)(atomic_u32_add_inline(&pool->submit_rr,1u)%(uint32_t)G);
        V3Task t = { fn, arg, s };
        atomic_add_inline(&pool->ctrs[s],1);
        if (xring_push_mp(&pool->shards[s].ring, pool->shards[s].buf, &t)){
            /* so acorda se houver CORE parqueado (em regime quente: nao acorda). */
            if (atomic_get_inline(&pool->n_parked_core) > 0){
                uint32_t k=atomic_u32_add_inline(&pool->wake_rr,1u)%(uint32_t)pool->n_core;
                thread_wait_wake_inline(&pool->workers[k].wait);
            }
            return true;
        }
        atomic_sub_inline(&pool->ctrs[s],1);          /* shard cheia: desfaz e re-tenta */
        if (atomic_get_inline(&pool->stop)) return false;
        if      (spins<64)  xcpu_pause();
        else if (spins<256) v3_yield();
        else                v3_sleep0();
        spins++;
    }
}

static long v3_total_pending(WSPoolV3* pool){
    long s=0; for (int i=0;i<pool->n_ctrs;i++) s+=atomic_get_inline(&pool->ctrs[i]); return s;
}
void v3_pool_wait_idle(WSPoolV3* pool){ if(!pool)return; while (v3_total_pending(pool)>0) v3_sleep0(); }
void v3_pool_dims(WSPoolV3* pool, int* w, int* c){ if(!pool)return; if(w)*w=pool->n_workers; if(c)*c=pool->n_core; }

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
        for (int i=0;i<pool->n_workers;i++) v3_deque_free(&pool->workers[i].deque);
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
