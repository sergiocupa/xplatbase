/*
 * thread_pool.c — pool OFICIAL (ver thread_pool.h). Design consolidado (V2.04):
 *   arena (shards MPMC compartilhadas) + deque Chase-Lev local (spawn) +
 *   core/reserva + worker elastico (monitor detecta presos -> acorda extras).
 *   pending por contador-indexado-na-task (baixa contencao, correto com steal).
 *
 *   V2.04 incorpora as correcoes/otimizacoes validadas no bench de variantes:
 *     - Bug 1: in_task tratado como PROFUNDIDADE (submit reentrante com deque
 *       cheio roda a task aninhada via pool_run sem zerar o flag da task externa).
 *     - Bug 2/3: thread_wait_init/shutdown pareados via wait_inited; o retorno
 *       do init passa a ser usado.
 *     - Perf A: contadores 'ctrs' com padding de linha de cache (anti false-sharing).
 *     - Perf B: atomicos quentes do struct (submit_rr/wake_rr/n_parked_core/stop)
 *       em linhas de cache separadas.
 *     - Perf C: wakeup direcionado — o submit acorda exatamente um core de fato
 *       parqueado (em vez de round-robin cego).
 *     - Perf D: contador de conclusao por-worker (64 bits) — decremento single-writer
 *       no proprio executor; soma global conservada.
 *     - Perf E: o monitor acorda um LOTE de elasticos por tick (rampa mais rapida).
 */

#include "thread_pool.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "atomics.h"
#include "ring_queue.h"
#include "thread_wait.h"

#ifndef POOL_CORE_NUM
#define POOL_CORE_NUM      7
#endif
#ifndef POOL_CORE_DEN
#define POOL_CORE_DEN      10
#endif
#ifndef POOL_ELASTIC_NUM
#define POOL_ELASTIC_NUM   1
#endif
#ifndef POOL_ELASTIC_DEN
#define POOL_ELASTIC_DEN   1
#endif
#ifndef POOL_SHARD_DIV
#define POOL_SHARD_DIV     4
#endif
#ifndef POOL_SHARD_CAP
#define POOL_SHARD_CAP     4096
#endif
#ifndef POOL_DEQUE_CAP
#define POOL_DEQUE_CAP     4096
#endif
#ifndef POOL_SPIN_PAUSE
#define POOL_SPIN_PAUSE    512
#endif
#ifndef POOL_SPIN_YIELD
#define POOL_SPIN_YIELD    64
#endif
#ifndef POOL_SPIN_SLEEP0
#define POOL_SPIN_SLEEP0   8
#endif
#ifndef POOL_PARK_TIMEOUT_US
#define POOL_PARK_TIMEOUT_US 1000
#endif
#ifndef POOL_MON_MS
#define POOL_MON_MS        5
#endif
#ifndef POOL_STUCK_MIN
#define POOL_STUCK_MIN     2
#endif
#ifndef POOL_ELASTIC_PARK_US
#define POOL_ELASTIC_PARK_US 100000
#endif
#ifndef POOL_ELASTIC_RETIRE_SPINS
#define POOL_ELASTIC_RETIRE_SPINS 200000
#endif

#ifdef _MSC_VER
#define POOL_INLINE static __forceinline
#else
#define POOL_INLINE static inline __attribute__((always_inline))
#endif

 // internal
static ThreadPool* GlobalPool = NULL;   /* opcional, para casos simples; nao precisa ser thread-safe */


#ifdef XPLATBASE_WIN
    #include <intrin.h>
    typedef HANDLE       pool_thread_t;
    typedef DWORD WINAPI pool_fn_sig(void*);
    static bool pool_thread_start(pool_thread_t* o, pool_fn_sig* fn, void* a){ *o=CreateThread(NULL,0,fn,a,0,NULL); return *o!=NULL; }
    static void pool_thread_join(pool_thread_t h){ WaitForSingleObject(h,INFINITE); CloseHandle(h); }
    static void pool_yield(void){ SwitchToThread(); }
    static void pool_sleep0(void){ Sleep(0); }
    #define POOL_FN   DWORD WINAPI
    #define POOL_RET  return 0
    typedef volatile LONG64 pool_a64;
    #define pool_ld(p)        (*(p))
    #define pool_st(p,v)      (*(p) = (LONG64)(v))
    #define pool_fence()      MemoryBarrier()
    static int pool_cas64(pool_a64* p, LONG64 e, LONG64 d){ return InterlockedCompareExchange64(p,d,e)==e; }
#else
    #include <pthread.h>
    #include <sched.h>
    #include <time.h>
    #include <stdatomic.h>
    typedef pthread_t pool_thread_t;
    typedef void*     pool_fn_sig(void*);
    static bool pool_thread_start(pool_thread_t* o, pool_fn_sig* fn, void* a){ memset(o,0,sizeof(*o)); return pthread_create(o,NULL,fn,a)==0; }
    static void pool_thread_join(pool_thread_t h){ pthread_join(h,NULL); }
    static void pool_yield(void){ sched_yield(); }
    static void pool_sleep0(void){ struct timespec z={0,0}; nanosleep(&z,NULL); }
    #define POOL_FN   void*
    #define POOL_RET  return NULL
    typedef _Atomic long long pool_a64;
    #define pool_ld(p)        atomic_load_explicit((p), memory_order_relaxed)
    #define pool_st(p,v)      atomic_store_explicit((p),(long long)(v), memory_order_relaxed)
    #define pool_fence()      atomic_thread_fence(memory_order_seq_cst)
    static int pool_cas64(pool_a64* p, long long e, long long d){ return atomic_compare_exchange_strong_explicit(p,&e,d,memory_order_seq_cst,memory_order_relaxed); }
#endif

/* Perf A+D: contador de pending de 64 bits, cada um na sua propria linha de
 * cache (o decremento por-worker do Perf D pode acumular drift grande). */
typedef struct XPL_ALIGN(XPL_CACHELINE) {
    xatomic_int64 v;
    char _pad[XPL_CACHELINE - sizeof(xatomic_int64)];
} PoolCtr;

typedef struct { pool_task_fn fn; void* arg; int ctr; } PoolTask;

typedef struct XPL_ALIGN(XPL_CACHELINE) {
    pool_a64 top;
    char     _p0[XPL_CACHELINE - sizeof(pool_a64)];
    pool_a64 bottom;
    char     _p1[XPL_CACHELINE - sizeof(pool_a64)];
    PoolTask* buf; int cap; int mask;
} PoolDeque;

static bool pool_deque_init(PoolDeque* d, int cap){
    d->buf=(PoolTask*)malloc((size_t)cap*sizeof(PoolTask)); if(!d->buf) return false;
    d->cap=cap; d->mask=cap-1; pool_st(&d->top,0); pool_st(&d->bottom,0); return true;
}
static void pool_deque_free(PoolDeque* d){ free(d->buf); d->buf=NULL; }
POOL_INLINE bool pool_deque_push(PoolDeque* d, const PoolTask* t){
    int64_t b=pool_ld(&d->bottom), tp=pool_ld(&d->top);
    if (b - tp >= d->cap) return false;
    d->buf[b & d->mask] = *t; pool_fence(); pool_st(&d->bottom, b+1); return true;
}
POOL_INLINE bool pool_deque_take(PoolDeque* d, PoolTask* out){
    int64_t b=pool_ld(&d->bottom)-1; pool_st(&d->bottom,b); pool_fence();
    int64_t t=pool_ld(&d->top);
    if (t<=b){ *out=d->buf[b & d->mask];
        if (t==b){ int ok=pool_cas64(&d->top,t,t+1); pool_st(&d->bottom,b+1); return ok!=0; } return true;
    } else { pool_st(&d->bottom,b+1); return false; }
}
POOL_INLINE int pool_deque_steal(PoolDeque* d, PoolTask* out){
    int64_t t=pool_ld(&d->top); pool_fence(); int64_t b=pool_ld(&d->bottom);
    if (t<b){ PoolTask x=d->buf[t & d->mask]; if(!pool_cas64(&d->top,t,t+1)) return 0; *out=x; return 1; }
    return 0;
}

typedef struct XPL_ALIGN(XPL_CACHELINE) {
    RingQueue ring; void* buf; char _pad[XPL_CACHELINE];
} PoolShard;

typedef struct XPL_ALIGN(XPL_CACHELINE) {
    ThreadPool*   pool;
    int           index;
    int           is_core;
    int           is_elastic;
    pool_thread_t handle;
    xwait_t       wait;
    PoolDeque     deque;
    uint32_t      shard_cursor;
    uint32_t      steal_cursor;
    volatile long done_count;     /* progresso (single-writer = este worker) */
    volatile int  in_task;        /* profundidade (Bug 1) */
    long          mon_last;       /* uso exclusivo do monitor */
    xatomic_int   parked;         /* elastico e core (Perf C): 1 = parqueado */
    char          _pad[XPL_CACHELINE];
} PoolWorker;

struct XPL_ALIGN(XPL_CACHELINE) ThreadPool {
    int            n_core;
    int            n_workers;      /* core + reserva (sem elasticos) */
    int            n_total;        /* n_workers + n_elastic */
    int            n_elastic;
    int            n_shards;
    PoolShard*     shards;
    PoolWorker*    workers;        /* [0,n_core)=core [n_core,n_workers)=reserva [n_workers,n_total)=elastico */
    PoolCtr*       ctrs;
    int            n_ctrs;         /* n_shards + n_total */
    int            wait_inited;    /* Bug 2/3: pareia thread_wait_init/shutdown */
    /* Perf B: atomicos quentes em linhas de cache separadas */
    char           _padb0[XPL_CACHELINE];
    xatomic_uint32 submit_rr;
    char           _padb1[XPL_CACHELINE - sizeof(xatomic_uint32)];
    xatomic_uint32 wake_rr;
    char           _padb2[XPL_CACHELINE - sizeof(xatomic_uint32)];
    xatomic_int    n_parked_core;
    char           _padb3[XPL_CACHELINE - sizeof(xatomic_int)];
    xatomic_int    stop;
    char           _padb4[XPL_CACHELINE - sizeof(xatomic_int)];
    pool_thread_t  mon_handle;
    xwait_t        mon_wait;
};

#ifdef XPLATBASE_WIN
static __declspec(thread) PoolWorker* g_pool_self;
#else
static __thread PoolWorker* g_pool_self;
#endif

POOL_INLINE void pool_run(ThreadPool* pool, PoolWorker* self, PoolTask* t){
    self->in_task++;          /* Bug 1: profundidade (suporta pool_run aninhado) */
    t->fn(t->arg);
    self->in_task--;
    self->done_count++;
    /* Perf D: decrementa o contador DESTE worker (single-writer no decremento);
     * a soma global continua conservada (todo +1 tem um -1 em algum indice). */
    atomic_sub64(&pool->ctrs[pool->n_shards + self->index].v, 1);
}

POOL_INLINE bool pool_try_get(ThreadPool* pool, PoolWorker* self, PoolTask* out){
    if (pool_deque_take(&self->deque, out)) return true;
    int G=pool->n_shards; uint32_t sc=self->shard_cursor;
    for (int g=0;g<G;g++){
        int s=(int)((sc+(uint32_t)g)%(uint32_t)G);
        if (xring_pop_mc(&pool->shards[s].ring, pool->shards[s].buf, out)){ self->shard_cursor=(uint32_t)s; return true; }
    }
    int nt=pool->n_total; uint32_t s2=self->steal_cursor;
    for (int k=1;k<=nt;k++){
        int v=(int)((s2+(uint32_t)k)%(uint32_t)nt);
        if (v==self->index) continue;
        if (pool_deque_steal(&pool->workers[v].deque, out)){ self->steal_cursor=(uint32_t)v; return true; }
    }
    return false;
}

static bool pool_spin(ThreadPool* pool, PoolWorker* self, PoolTask* out){
    for (int i=0;i<POOL_SPIN_PAUSE;i++){ if(pool_try_get(pool,self,out))return true; if(atomic_get(&pool->stop))return false; xcpu_pause(); }
    for (int i=0;i<POOL_SPIN_YIELD;i++){ if(pool_try_get(pool,self,out))return true; if(atomic_get(&pool->stop))return false; pool_yield(); }
    for (int i=0;i<POOL_SPIN_SLEEP0;i++){ if(pool_try_get(pool,self,out))return true; if(atomic_get(&pool->stop))return false; pool_sleep0(); }
    return false;
}

static POOL_FN pool_worker_fn(void* raw){
    PoolWorker* self=(PoolWorker*)raw; ThreadPool* pool=self->pool; PoolTask t;
    g_pool_self=self;

    if (self->is_elastic){
        while (!atomic_get(&pool->stop)){
            atomic_set(&self->parked,1);
            thread_wait_prepare(&self->wait);
            if (atomic_get(&self->parked)) thread_wait_sleep_for(&self->wait, POOL_ELASTIC_PARK_US);
            if (atomic_get(&pool->stop)) break;
            if (atomic_get(&self->parked)) continue;     /* timeout, nao ativado */
            int idle=0;
            while (!atomic_get(&pool->stop)){
                if (pool_try_get(pool,self,&t)){ pool_run(pool,self,&t); idle=0; continue; }
                if (++idle > POOL_ELASTIC_RETIRE_SPINS) break;
                xcpu_pause();
            }
        }
        POOL_RET;
    }

    int is_core=self->is_core;
    while (!atomic_get(&pool->stop)){
        if (pool_try_get(pool,self,&t)){ pool_run(pool,self,&t); continue; }
        thread_wait_prepare(&self->wait);
        if (pool_try_get(pool,self,&t)){ pool_run(pool,self,&t); continue; }
        if (is_core){ if (pool_spin(pool,self,&t)){ pool_run(pool,self,&t); continue; } }
        if (atomic_get(&pool->stop)) break;
        /* Perf C: marca este core como parqueado para wakeup direcionado. */
        if (is_core){ atomic_set(&self->parked,1); atomic_add(&pool->n_parked_core,1); }
        thread_wait_sleep_for(&self->wait, POOL_PARK_TIMEOUT_US);
        if (is_core){ atomic_sub(&pool->n_parked_core,1); atomic_set(&self->parked,0); }
    }
    POOL_RET;
}

static POOL_FN pool_monitor_fn(void* raw){
    ThreadPool* pool=(ThreadPool*)raw;
    thread_wait_prepare(&pool->mon_wait);
    while (!atomic_get(&pool->stop)){
        thread_wait_sleep_for(&pool->mon_wait, POOL_MON_MS*1000);
        if (atomic_get(&pool->stop)) break;
        int stuck=0;
        for (int i=0;i<pool->n_workers;i++){
            PoolWorker* w=&pool->workers[i];
            long dc=w->done_count;
            if (w->in_task && dc==w->mon_last) stuck++;
            w->mon_last=dc;
        }
        if (stuck < POOL_STUCK_MIN) continue;
        int backlog=0;
        for (int s=0;s<pool->n_shards;s++) if(!ring_queue_empty(&pool->shards[s].ring)){ backlog=1; break; }
        if (!backlog) continue;
        /* Perf E: acorda ate 'stuck' elasticos por tick (em vez de 1 por 5ms). */
        int to_wake=stuck;
        for (int e=pool->n_workers;e<pool->n_total && to_wake>0;e++){
            PoolWorker* w=&pool->workers[e];
            int exp=1;
            if (atomic_cas(&w->parked,&exp,0)){ thread_wait_wake(&w->wait); to_wake--; }
        }
    }
    POOL_RET;
}

ThreadPool* pool_create_relative(int cores_override)
{
    int cores=cores_override>0?cores_override:xcpu_count(); if(cores<1)cores=1;
    int nw=cores; if(nw<1)nw=1;
    int nc=cores*POOL_CORE_NUM/POOL_CORE_DEN; if(nc<1)nc=1; if(nc>nw)nc=nw;
    int ne=cores*POOL_ELASTIC_NUM/POOL_ELASTIC_DEN; if(ne<0)ne=0;
    int nt=nw+ne;
    int G=cores/POOL_SHARD_DIV; if(G<1)G=1;

    ThreadPool* pool=(ThreadPool*)calloc(1,sizeof(ThreadPool)); if(!pool) return NULL;
    pool->n_core=nc; pool->n_workers=nw; pool->n_elastic=ne; pool->n_total=nt; pool->n_shards=G;
    pool->n_ctrs=G+nt;
    pool->ctrs=(PoolCtr*)calloc((size_t)pool->n_ctrs,sizeof(PoolCtr));
    pool->shards=(PoolShard*)calloc((size_t)G,sizeof(PoolShard));
    pool->workers=(PoolWorker*)calloc((size_t)nt,sizeof(PoolWorker));
    if(!pool->ctrs||!pool->shards||!pool->workers){ pool_destroy_relative(pool); return NULL; }
    /* Bug 2/3: so registra shutdown se o init de fato elevou o timer (return). */
    pool->wait_inited = thread_wait_init(false) ? 1 : 0;
    for (int s=0;s<G;s++)
    {
        ring_queue_init(&pool->shards[s].ring, POOL_SHARD_CAP);
        pool->shards[s].buf=malloc((size_t)POOL_SHARD_CAP*sizeof(PoolTask));
        if (pool->shards[s].ring.capacity==0 || !pool->shards[s].buf){ pool_destroy_relative(pool); return NULL; }
    }
    for (int i=0;i<nt;i++)
    {
        PoolWorker* w=&pool->workers[i];
        w->pool=pool; w->index=i; w->is_core=(i<nc)?1:0; w->is_elastic=(i>=nw)?1:0;
        w->shard_cursor=(uint32_t)(i%G); w->steal_cursor=(uint32_t)i;
        thread_wait_prepare(&w->wait);
        atomic_set(&w->parked, w->is_elastic?1:0);
        if (!pool_deque_init(&w->deque, POOL_DEQUE_CAP)){ pool_destroy_relative(pool); return NULL; }
    }
    for (int i=0;i<nt;i++)
    {
        if (!pool_thread_start(&pool->workers[i].handle, pool_worker_fn, &pool->workers[i])){
            atomic_set(&pool->stop,1);
            for (int j=0;j<i;j++){ thread_wait_wake(&pool->workers[j].wait); pool_thread_join(pool->workers[j].handle); }
            pool_destroy_relative(pool); return NULL;
        }
    }
    thread_wait_prepare(&pool->mon_wait);
    if (!pool_thread_start(&pool->mon_handle, pool_monitor_fn, pool)){
        atomic_set(&pool->stop,1);
        for (int i=0;i<nt;i++){ thread_wait_wake(&pool->workers[i].wait); pool_thread_join(pool->workers[i].handle); }
        pool_destroy_relative(pool); return NULL;
    }
    return pool;
}


boolean pool_submit_relative(ThreadPool* pool, pool_task_fn fn, void* arg)
{
    if (!pool || !fn) return false;
    if (atomic_get(&pool->stop)) return false;

    PoolWorker* me=g_pool_self;
    if (me && me->pool==pool){       /* reentrante: deque local */
        PoolTask t = { fn, arg, pool->n_shards + me->index };
        atomic_add64(&pool->ctrs[t.ctr].v,1);
        if (pool_deque_push(&me->deque,&t)) return true;
        pool_run(pool,me,&t);
        return true;
    }
    int G=pool->n_shards; int spins=0;
    for (;;){
        int s=(int)(atomic_u32_add(&pool->submit_rr,1u)%(uint32_t)G);
        PoolTask t = { fn, arg, s };
        atomic_add64(&pool->ctrs[s].v,1);
        if (xring_push_mp(&pool->shards[s].ring, pool->shards[s].buf, &t)){
            if (atomic_get(&pool->n_parked_core) > 0){
                /* Perf C: acorda exatamente UM core que esteja de fato parqueado. */
                int nc=pool->n_core;
                uint32_t start=atomic_u32_add(&pool->wake_rr,1u);
                for (int j=0;j<nc;j++){
                    int idx=(int)((start+(uint32_t)j)%(uint32_t)nc);
                    int exp=1;
                    if (atomic_cas(&pool->workers[idx].parked,&exp,0)){
                        thread_wait_wake(&pool->workers[idx].wait); break;
                    }
                }
            }
            return true;
        }
        atomic_sub64(&pool->ctrs[s].v,1);
        if (atomic_get(&pool->stop)) return false;
        if      (spins<64)  xcpu_pause();
        else if (spins<256) pool_yield();
        else                pool_sleep0();
        spins++;
    }
    return true;
}

static long long pool_total_pending(ThreadPool* pool)
{
    long long s=0; for (int i=0;i<pool->n_ctrs;i++) s+=atomic_get64(&pool->ctrs[i].v); return s;
}

void pool_wait_idle_relative(ThreadPool* pool){ if(!pool)return; while (pool_total_pending(pool)>0) pool_sleep0(); }
void pool_dims_relative(ThreadPool* pool, int* w, int* c){ if(!pool)return; if(w)*w=pool->n_workers; if(c)*c=pool->n_core; }

void pool_destroy_relative(ThreadPool* pool)
{
    if (!pool) return;
    if (!atomic_get(&pool->stop)){
        pool_wait_idle_relative(pool);
        atomic_set(&pool->stop,1);
        thread_wait_wake(&pool->mon_wait);
        if (pool->mon_handle) pool_thread_join(pool->mon_handle);
        if (pool->workers)
            for (int i=0;i<pool->n_total;i++){
                thread_wait_wake(&pool->workers[i].wait);
                if (pool->workers[i].handle) pool_thread_join(pool->workers[i].handle);
            }
    }
    if (pool->workers){
        for (int i=0;i<pool->n_total;i++) pool_deque_free(&pool->workers[i].deque);
        free(pool->workers);
    }
    if (pool->shards){
        for (int s=0;s<pool->n_shards;s++){ free(pool->shards[s].buf); ring_queue_destroy(&pool->shards[s].ring); }
        free(pool->shards);
    }
    free(pool->ctrs);
    /* Bug 2/3: so faz shutdown se o init foi pareado. */
    if (pool->wait_inited) thread_wait_shutdown();
    free(pool);
}


// internal
void pool_create()
{
    if (!GlobalPool) { GlobalPool = pool_create_relative(0); }
}

void pool_destroy()
{
    if (GlobalPool) { pool_destroy_relative(GlobalPool); GlobalPool = 0; }
}

boolean pool_submit(pool_task_fn fn, void* arg)
{
    if (GlobalPool) { return pool_submit_relative(GlobalPool, fn, arg); } return false;
}

void pool_wait_idle()
{
    pool_wait_idle_relative(GlobalPool);
}

void pool_dims(int* w, int* c)
{
    pool_dims_relative(GlobalPool, w, c);
}
