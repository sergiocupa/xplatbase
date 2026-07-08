/*
 * thread_pool.c — pool OFICIAL (ver thread_pool.h). Design consolidado (V2.05):
 *   arena (shards MPMC compartilhadas) + deque Chase-Lev local (spawn) +
 *   core/reserva + worker elastico (monitor detecta presos -> acorda extras).
 *   pending por contador-indexado (baixa contencao, correto com steal).
 *
 *   V2.05 consolida as otimizacoes validadas no bench de grupos (g1..g4/all/
 *   final, ver Tester/thread_pool). Ganho medido vs V2.04: +76-81% no submit
 *   externo (flat/rapida/satur, 2,3-2,6x TBB), spawn empatado com TBB com
 *   cauda melhor, lento-misto p50 142ms -> 0,6ms; cauda (max10) igual ou
 *   melhor na maioria dos cenarios. Historico completo nos TSV do Tester.
 *
 *   Mudancas vs V2.04:
 *     - Perf F (ring): fila de shard agora e um ring Vyukov TIPADO com ops
 *       inline neste TU: sem peek_guard (o seqno ja da exclusividade do slot),
 *       publicacao com store RELEASE (nao lock xchg), celula de 32B com seqno
 *       e task na mesma linha. Antes: ring_queue.c out-of-line, ~87ns por
 *       push+pop; agora ~24ns (e 2x melhor sob contencao de consumidores).
 *     - Perf G (lote): consumidor reserva ate POOL_BATCH tasks do shard com um
 *       unico CAS no head (estilo crossbeam steal_batch), excedente em buffer
 *       privado do worker. POOL_BATCH=2: batch maior (8) inflava o p99 quando
 *       uma task longa entrava no lote (validado no mini-bench).
 *     - Perf H (task 16B): removido o campo 'ctr' de PoolTask (era escrito e
 *       nunca lido — o decremento usa o indice do executor). 4 tasks por
 *       linha de cache em vez de 2,67.
 *     - Perf I (wake): o scan de wakeup LE 'parked' antes do CAS (o lock
 *       cmpxchg em worker nao-parqueado roubava a linha do dono em modo
 *       exclusivo); 'parked'+'wait' em linha de cache propria, separados dos
 *       campos quentes do dono.
 *     - Perf J (LIFO slot): spawn reentrante poe a task nova num slot LIFO
 *       nao-roubavel do worker (estilo Tokio) — o filho roda em seguida com
 *       cache quente; anti-inanicao via POOL_LIFO_CAP usos consecutivos.
 *     - Perf K (deque): take com fast-path de vazio (bottom<=top) sem pagar o
 *       mfence do protocolo Chase-Lev; push com fence RELEASE (x86: barreira
 *       de compilador) — so o take precisa de mfence (Le et al. 2013).
 *     - Perf L (contador): decremento de pending do worker com store RELEASE
 *       single-writer (o contador so e escrito pelo proprio worker) em vez de
 *       lock xadd.
 *
 *   NAO incorporado (regrediu no bench; ver versions/thread_pool_g2/g3/g4):
 *     - shard sticky por produtor (g2): FIFO atras das longas com produtor
 *       unico (p50 500ms no lento-misto); o round-robin espalha e compensa.
 *     - throttle de spinners (g3): -57% no flat sem wake-chaining.
 *     - parking por eventos (g4): -11% no flat (produtor paga o syscall de
 *       wake); reavaliar junto com wake-chaining se energia virar requisito.
 *
 *   Historico V2.04:
 *     - Bug 1: in_task tratado como PROFUNDIDADE (submit reentrante com deque
 *       cheio roda a task aninhada via pool_run sem zerar o flag da externa).
 *     - Bug 2/3: thread_wait_init/shutdown pareados via wait_inited.
 *     - Perf A: contadores 'ctrs' com padding de linha de cache.
 *     - Perf B: atomicos quentes do struct em linhas separadas.
 *     - Perf C: wakeup direcionado (acorda um core de fato parqueado).
 *     - Perf D: contador de conclusao por-worker (single-writer).
 *     - Perf E: monitor acorda um LOTE de elasticos por tick.
 */

#include "thread_pool.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "atomics.h"
#include "ring_queue.h"     /* XPL_ALIGN / XPL_CACHELINE */
#include "thread_wait.h"
#include "thread_handler.h"

#ifdef XPLATBASE_WIN
#include <malloc.h>
#endif
#ifdef _MSC_VER
#include <intrin.h>
#endif

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
#ifndef POOL_BATCH
#define POOL_BATCH         2     /* Perf G: tasks reservadas por CAS no shard */
#endif
#ifndef POOL_LIFO_CAP
#define POOL_LIFO_CAP      8     /* Perf J: usos seguidos do LIFO slot */
#endif

#ifdef _MSC_VER
#define POOL_INLINE static __forceinline
#else
#define POOL_INLINE static inline __attribute__((always_inline))
#endif

 // internal
static ThreadPool* GlobalPool = NULL;   /* opcional, para casos simples; nao precisa ser thread-safe */


typedef Thread* pool_thread_t;
typedef xthread_atomic64 pool_a64;

#define pool_thread_start(o,fn,a) (((*(o)) = thread_create((fn), (a), NULL)) != NULL)
#define pool_thread_join(h)      thread_join(&(h))
#define pool_yield()             thread_yield_inline()
#define pool_sleep0()            thread_sleep0_inline()
#define POOL_FN                  xthread_result_t
#define POOL_RET                 return (xthread_result_t)0
#define pool_ld(p)               thread_atomic64_load_inline((p))
#define pool_st(p,v)             thread_atomic64_store_inline((p), (long long)(v))
#define pool_fence()             thread_fence_inline()
#define pool_cas64(p,e,d)        thread_atomic64_cas_inline((p), (long long)(e), (long long)(d))

/* Perf K: fence de release para o push do deque.
 * x86: stores nao reordenam entre si -> basta impedir o compilador.
 * MSVC nao-x86 cai no MemoryBarrier; POSIX usa fence release C11. */
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#define pool_fence_release() _WriteBarrier()
#elif defined(_MSC_VER)
#define pool_fence_release() MemoryBarrier()
#else
#define pool_fence_release() atomic_thread_fence(memory_order_release)
#endif

/* Perf A+D: contador de pending de 64 bits, cada um na sua propria linha de
 * cache (o decremento por-worker pode acumular drift grande). */
typedef struct XPL_ALIGN(XPL_CACHELINE) {
    xatomic_int64 v;
    char _pad[XPL_CACHELINE - sizeof(xatomic_int64)];
} PoolCtr;

/* Perf H: 16 bytes (sem campo ctr morto) */
typedef struct { pool_task_fn fn; void* arg; } PoolTask;

/* ==================== Perf F: ring Vyukov tipado inline ==================== */
/* Atomics u32 inline (sem call): volatile em Win (/volatile:ms da acquire no
 * load e release no store), C11 no resto. */
#ifdef XPLATBASE_WIN
typedef volatile LONG pool_a32;
POOL_INLINE uint32_t pool_ld32(pool_a32* p){ return (uint32_t)*p; }
POOL_INLINE void     pool_st32_rel(pool_a32* p, uint32_t v){ *p = (LONG)v; }
POOL_INLINE int      pool_cas32(pool_a32* p, uint32_t e, uint32_t d){ return InterlockedCompareExchange(p,(LONG)d,(LONG)e)==(LONG)e; }
#else
typedef _Atomic uint32_t pool_a32;
POOL_INLINE uint32_t pool_ld32(pool_a32* p){ return atomic_load_explicit(p, memory_order_acquire); }
POOL_INLINE void     pool_st32_rel(pool_a32* p, uint32_t v){ atomic_store_explicit(p, v, memory_order_release); }
POOL_INLINE int      pool_cas32(pool_a32* p, uint32_t e, uint32_t d){ uint32_t x=e; return atomic_compare_exchange_strong_explicit(p,&x,d,memory_order_acq_rel,memory_order_acquire); }
#endif

/* celula 32B: seqno + task na MESMA linha (produtor toca 1 linha por push) */
typedef struct XPL_ALIGN(32) {
    pool_a32 seq;
    PoolTask t;
} PoolCell;

typedef struct XPL_ALIGN(XPL_CACHELINE) {
    pool_a32  tail;  char _pt[XPL_CACHELINE - sizeof(pool_a32)];   /* hot: produtores   */
    pool_a32  head;  char _ph[XPL_CACHELINE - sizeof(pool_a32)];   /* hot: consumidores */
    PoolCell* cells;
    uint32_t  mask;
} PoolRing;

static void* pool_aligned_alloc(size_t sz){
#ifdef XPLATBASE_WIN
    return _aligned_malloc(sz, 64);
#else
    void* p=NULL; if (posix_memalign(&p,64,sz)!=0) return NULL; return p;
#endif
}
static void pool_aligned_free(void* p){
#ifdef XPLATBASE_WIN
    _aligned_free(p);
#else
    free(p);
#endif
}

static bool pool_ring_init(PoolRing* r, int cap){
    r->cells=(PoolCell*)pool_aligned_alloc((size_t)cap*sizeof(PoolCell));
    if (!r->cells) return false;
    r->mask=(uint32_t)cap-1u;
    for (int i=0;i<cap;i++) pool_st32_rel(&r->cells[i].seq,(uint32_t)i);
    pool_st32_rel(&r->tail,0); pool_st32_rel(&r->head,0);
    return true;
}
static void pool_ring_free(PoolRing* r){ pool_aligned_free(r->cells); r->cells=NULL; }

POOL_INLINE bool pool_ring_push(PoolRing* r, const PoolTask* t){
    uint32_t pos = pool_ld32(&r->tail);
    for (;;){
        PoolCell* c = &r->cells[pos & r->mask];
        int32_t dif = (int32_t)(pool_ld32(&c->seq) - pos);
        if (dif == 0){
            if (pool_cas32(&r->tail, pos, pos+1u)){
                c->t = *t;                        /* copia inline (tamanho fixo) */
                pool_st32_rel(&c->seq, pos+1u);   /* publica com release        */
                return true;
            }
            pos = pool_ld32(&r->tail);
        }
        else if (dif < 0) return false;           /* cheio */
        else pos = pool_ld32(&r->tail);
    }
}

/* Perf G: reserva ate 'max' tasks com um unico CAS no head. */
POOL_INLINE int pool_ring_pop_batch(PoolRing* r, PoolTask* out, int max){
    for (;;){
        uint32_t pos = pool_ld32(&r->head);
        int n = 0;
        while (n < max){
            PoolCell* c = &r->cells[(pos+(uint32_t)n) & r->mask];
            if ((int32_t)(pool_ld32(&c->seq) - (pos+(uint32_t)n+1u)) != 0) break;
            n++;
        }
        if (!n) return 0;
        if (pool_cas32(&r->head, pos, pos+(uint32_t)n)){
            for (int i=0;i<n;i++){
                PoolCell* c = &r->cells[(pos+(uint32_t)i) & r->mask];
                out[i] = c->t;
                pool_st32_rel(&c->seq, pos+(uint32_t)i+r->mask+1u);  /* recicla slot */
            }
            return n;
        }
    }
}

POOL_INLINE int pool_ring_count(PoolRing* r){
    uint32_t t=pool_ld32(&r->tail), h=pool_ld32(&r->head);
    int32_t d=(int32_t)(t-h); return d>0?d:0;
}

/* ==================== deque Chase-Lev (Perf K) ==================== */
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
    d->buf[b & d->mask] = *t; pool_fence_release(); pool_st(&d->bottom, b+1); return true;
}
POOL_INLINE bool pool_deque_take(PoolDeque* d, PoolTask* out){
    /* Perf K: fast-path de vazio sem mfence. 'bottom' e exato (so o dono
     * escreve); 'top' defasado so faz o teste falhar para o lado seguro
     * (cai no protocolo completo, o CAS resolve) — nunca perde task. */
    int64_t b=pool_ld(&d->bottom);
    if (b - pool_ld(&d->top) <= 0) return false;
    b -= 1; pool_st(&d->bottom,b); pool_fence();
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
    PoolRing ring; char _pad[XPL_CACHELINE];
} PoolShard;

/* Perf I: campos do DONO agrupados; 'parked'+'wait' (escritos por wakers)
 * em linha de cache propria, sem false sharing com o hot-path do dono. */
typedef struct XPL_ALIGN(XPL_CACHELINE) {
    ThreadPool*   pool;
    int           index;
    int           is_core;
    int           is_elastic;
    pool_thread_t handle;
    uint32_t      shard_cursor;
    uint32_t      steal_cursor;
    volatile long done_count;     /* progresso (single-writer = este worker) */
    volatile int  in_task;        /* profundidade (Bug 1) */
    long          mon_last;       /* uso exclusivo do monitor */
    /* Perf J: LIFO slot (so o dono acessa — campos simples) */
    PoolTask      lifo_task;
    int           lifo_full;
    int           lifo_streak;
    /* Perf G: buffer do pop em lote (so o dono) */
    PoolTask      pending[POOL_BATCH];
    int           pend_i, pend_n;
    char          _pad0[XPL_CACHELINE];
    PoolDeque     deque;
    XPL_ALIGN(XPL_CACHELINE) xatomic_int parked;   /* linha dos wakers (Perf C/I) */
    xwait_t       wait;
    char          _pad1[XPL_CACHELINE];
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

/* Perf L: decremento single-writer com store release (sem lock xadd).
 * O contador do worker so e escrito pelo proprio worker (incremento no
 * submit reentrante, decremento aqui); leitores (wait_idle) usam acquire. */
POOL_INLINE void pool_ctr_dec_self(ThreadPool* pool, PoolWorker* self){
    xatomic_int64* p = &pool->ctrs[pool->n_shards + self->index].v;
#ifdef XPLATBASE_WIN
    *p = *p - 1;                       /* volatile ld+st; /volatile:ms = release */
#else
    atomic_store_explicit(p, atomic_load_explicit(p, memory_order_relaxed)-1, memory_order_release);
#endif
}

POOL_INLINE void pool_run(ThreadPool* pool, PoolWorker* self, PoolTask* t){
    self->in_task++;          /* Bug 1: profundidade (suporta pool_run aninhado) */
    t->fn(t->arg);
    self->in_task--;
    self->done_count++;
    pool_ctr_dec_self(pool, self);
}

POOL_INLINE bool pool_try_get(ThreadPool* pool, PoolWorker* self, PoolTask* out){
    /* Perf J: LIFO slot primeiro (cache quente), com anti-inanicao */
    if (self->lifo_full && self->lifo_streak < POOL_LIFO_CAP){
        self->lifo_streak++;
        *out = self->lifo_task; self->lifo_full = 0;
        return true;
    }
    if (pool_deque_take(&self->deque, out)){ self->lifo_streak=0; return true; }
    if (self->pend_i < self->pend_n){ self->lifo_streak=0; *out = self->pending[self->pend_i++]; return true; }
    int G=pool->n_shards; uint32_t sc=self->shard_cursor;
    for (int g=0;g<G;g++){
        int s=(int)((sc+(uint32_t)g)%(uint32_t)G);
        int n = pool_ring_pop_batch(&pool->shards[s].ring, self->pending, POOL_BATCH);
        if (n){ self->shard_cursor=(uint32_t)s; self->lifo_streak=0; *out=self->pending[0]; self->pend_i=1; self->pend_n=n; return true; }
    }
    int nt=pool->n_total; uint32_t s2=self->steal_cursor;
    for (int k=1;k<=nt;k++){
        int v=(int)((s2+(uint32_t)k)%(uint32_t)nt);
        if (v==self->index) continue;
        if (pool_deque_steal(&pool->workers[v].deque, out)){ self->steal_cursor=(uint32_t)v; self->lifo_streak=0; return true; }
    }
    /* nada em lugar nenhum: se o LIFO ficou retido pelo cap, drena agora */
    if (self->lifo_full){
        self->lifo_streak = 0;
        *out = self->lifo_task; self->lifo_full = 0;
        return true;
    }
    return false;
}

static bool pool_spin(ThreadPool* pool, PoolWorker* self, PoolTask* out){
    for (int i=0;i<POOL_SPIN_PAUSE;i++){ if(pool_try_get(pool,self,out))return true; if(atomic_get_inline(&pool->stop))return false; xcpu_pause(); }
    for (int i=0;i<POOL_SPIN_YIELD;i++){ if(pool_try_get(pool,self,out))return true; if(atomic_get_inline(&pool->stop))return false; pool_yield(); }
    for (int i=0;i<POOL_SPIN_SLEEP0;i++){ if(pool_try_get(pool,self,out))return true; if(atomic_get_inline(&pool->stop))return false; pool_sleep0(); }
    return false;
}

static POOL_FN pool_worker_fn(void* raw){
    PoolWorker* self=(PoolWorker*)raw; ThreadPool* pool=self->pool; PoolTask t;
    g_pool_self=self;

    if (self->is_elastic){
        while (!atomic_get_inline(&pool->stop)){
            atomic_set_inline(&self->parked,1);
            thread_wait_prepare_inline(&self->wait);
            if (atomic_get_inline(&self->parked)) thread_wait_sleep_for_inline(&self->wait, POOL_ELASTIC_PARK_US);
            if (atomic_get_inline(&pool->stop)) break;
            if (atomic_get_inline(&self->parked)) continue;     /* timeout, nao ativado */
            int idle=0;
            while (!atomic_get_inline(&pool->stop)){
                if (pool_try_get(pool,self,&t)){ pool_run(pool,self,&t); idle=0; continue; }
                if (++idle > POOL_ELASTIC_RETIRE_SPINS) break;
                xcpu_pause();
            }
        }
        POOL_RET;
    }

    int is_core=self->is_core;
    while (!atomic_get_inline(&pool->stop)){
        if (pool_try_get(pool,self,&t)){ pool_run(pool,self,&t); continue; }
        thread_wait_prepare_inline(&self->wait);
        if (pool_try_get(pool,self,&t)){ pool_run(pool,self,&t); continue; }
        if (is_core){ if (pool_spin(pool,self,&t)){ pool_run(pool,self,&t); continue; } }
        if (atomic_get_inline(&pool->stop)) break;
        /* Perf C: marca este core como parqueado para wakeup direcionado. */
        if (is_core){ atomic_set_inline(&self->parked,1); atomic_add_inline(&pool->n_parked_core,1); }
        thread_wait_sleep_for_inline(&self->wait, POOL_PARK_TIMEOUT_US);
        if (is_core){ atomic_sub_inline(&pool->n_parked_core,1); atomic_set_inline(&self->parked,0); }
    }
    POOL_RET;
}

static POOL_FN pool_monitor_fn(void* raw){
    ThreadPool* pool=(ThreadPool*)raw;
    thread_wait_prepare_inline(&pool->mon_wait);
    while (!atomic_get_inline(&pool->stop)){
        thread_wait_sleep_for_inline(&pool->mon_wait, POOL_MON_MS*1000);
        if (atomic_get_inline(&pool->stop)) break;
        int stuck=0;
        for (int i=0;i<pool->n_workers;i++){
            PoolWorker* w=&pool->workers[i];
            long dc=w->done_count;
            if (w->in_task && dc==w->mon_last) stuck++;
            w->mon_last=dc;
        }
        if (stuck < POOL_STUCK_MIN) continue;
        int backlog=0;
        for (int s=0;s<pool->n_shards;s++) if(pool_ring_count(&pool->shards[s].ring)>0){ backlog=1; break; }
        if (!backlog) continue;
        /* Perf E: acorda ate 'stuck' elasticos por tick (em vez de 1 por 5ms). */
        int to_wake=stuck;
        for (int e=pool->n_workers;e<pool->n_total && to_wake>0;e++){
            PoolWorker* w=&pool->workers[e];
            if (!atomic_get_inline(&w->parked)) continue;          /* Perf I: load antes do CAS */
            int exp=1;
            if (atomic_cas_inline(&w->parked,&exp,0)){ thread_wait_wake_inline(&w->wait); to_wake--; }
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
        if (!pool_ring_init(&pool->shards[s].ring, POOL_SHARD_CAP)){ pool_destroy_relative(pool); return NULL; }
    }
    for (int i=0;i<nt;i++)
    {
        PoolWorker* w=&pool->workers[i];
        w->pool=pool; w->index=i; w->is_core=(i<nc)?1:0; w->is_elastic=(i>=nw)?1:0;
        w->shard_cursor=(uint32_t)(i%G); w->steal_cursor=(uint32_t)i;
        w->lifo_full=0; w->lifo_streak=0; w->pend_i=0; w->pend_n=0;
        thread_wait_prepare_inline(&w->wait);
        atomic_set_inline(&w->parked, w->is_elastic?1:0);
        if (!pool_deque_init(&w->deque, POOL_DEQUE_CAP)){ pool_destroy_relative(pool); return NULL; }
    }
    for (int i=0;i<nt;i++)
    {
        if (!pool_thread_start(&pool->workers[i].handle, pool_worker_fn, &pool->workers[i])){
            atomic_set_inline(&pool->stop,1);
            for (int j=0;j<i;j++){ thread_wait_wake_inline(&pool->workers[j].wait); pool_thread_join(pool->workers[j].handle); }
            pool_destroy_relative(pool); return NULL;
        }
    }
    thread_wait_prepare_inline(&pool->mon_wait);
    if (!pool_thread_start(&pool->mon_handle, pool_monitor_fn, pool)){
        atomic_set_inline(&pool->stop,1);
        for (int i=0;i<nt;i++){ thread_wait_wake_inline(&pool->workers[i].wait); pool_thread_join(pool->workers[i].handle); }
        pool_destroy_relative(pool); return NULL;
    }
    return pool;
}


boolean pool_submit_relative(ThreadPool* pool, pool_task_fn fn, void* arg)
{
    if (!pool || !fn) return false;
    if (atomic_get_inline(&pool->stop)) return false;

    PoolWorker* me=g_pool_self;
    if (me && me->pool==pool){       /* reentrante: LIFO slot + deque local */
        PoolTask t = { fn, arg };
        atomic_add64_inline(&pool->ctrs[pool->n_shards + me->index].v,1);
        /* Perf J: task nova vai para o LIFO slot; a anterior desce ao deque */
        if (!me->lifo_full){ me->lifo_task=t; me->lifo_full=1; return true; }
        PoolTask old=me->lifo_task; me->lifo_task=t;
        if (!pool_deque_push(&me->deque,&old))
            pool_run(pool,me,&old);            /* deque cheio: roda a antiga ja */
        return true;
    }
    int G=pool->n_shards; int spins=0;
    for (;;){
        int s=(int)(atomic_u32_add_inline(&pool->submit_rr,1u)%(uint32_t)G);
        PoolTask t = { fn, arg };
        atomic_add64_inline(&pool->ctrs[s].v,1);
        if (pool_ring_push(&pool->shards[s].ring, &t)){
            if (atomic_get_inline(&pool->n_parked_core) > 0){
                /* Perf C: acorda exatamente UM core que esteja de fato parqueado. */
                int nc=pool->n_core;
                uint32_t start=atomic_u32_add_inline(&pool->wake_rr,1u);
                for (int j=0;j<nc;j++){
                    int idx=(int)((start+(uint32_t)j)%(uint32_t)nc);
                    if (!atomic_get_inline(&pool->workers[idx].parked)) continue;   /* Perf I */
                    int exp=1;
                    if (atomic_cas_inline(&pool->workers[idx].parked,&exp,0)){
                        thread_wait_wake_inline(&pool->workers[idx].wait); break;
                    }
                }
            }
            return true;
        }
        atomic_sub64_inline(&pool->ctrs[s].v,1);
        if (atomic_get_inline(&pool->stop)) return false;
        if      (spins<64)  xcpu_pause();
        else if (spins<256) pool_yield();
        else                pool_sleep0();
        spins++;
    }
    return true;
}

static long long pool_total_pending(ThreadPool* pool)
{
    long long s=0; for (int i=0;i<pool->n_ctrs;i++) s+=atomic_get64_inline(&pool->ctrs[i].v); return s;
}

void pool_wait_idle_relative(ThreadPool* pool){ if(!pool)return; while (pool_total_pending(pool)>0) pool_sleep0(); }
void pool_dims_relative(ThreadPool* pool, int* w, int* c){ if(!pool)return; if(w)*w=pool->n_workers; if(c)*c=pool->n_core; }

void pool_destroy_relative(ThreadPool* pool)
{
    if (!pool) return;
    if (!atomic_get_inline(&pool->stop)){
        pool_wait_idle_relative(pool);
        atomic_set_inline(&pool->stop,1);
        thread_wait_wake_inline(&pool->mon_wait);
        if (pool->mon_handle) pool_thread_join(pool->mon_handle);
        if (pool->workers)
            for (int i=0;i<pool->n_total;i++){
                thread_wait_wake_inline(&pool->workers[i].wait);
                if (pool->workers[i].handle) pool_thread_join(pool->workers[i].handle);
            }
    }
    if (pool->workers){
        for (int i=0;i<pool->n_total;i++) pool_deque_free(&pool->workers[i].deque);
        free(pool->workers);
    }
    if (pool->shards){
        for (int s=0;s<pool->n_shards;s++) pool_ring_free(&pool->shards[s].ring);
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
