/*
thread_pool.c

Implementação do pool de threads fragmentado descrito em thread_pool.h.

Pontos-chave do projeto:

- O worker é desacoplado do Shard. Um Shard possui um worker_id atual; o worker o lê após cada tarefa para saber se foi desanexado.
- Caminho crítico do worker: esvaziar o anel -> loop de espera -> estacionar (xwait).
  Isso é feito em worker_idle_phase(), que substitui o par original
  pool_wake_one()/thread_wait_sleep().
- Submissão: rodízio entre os shards, solicitação de expansão sob pressão.
- Monitoramento: thread única executando quatro tarefas:
    1) detectar workers longos e bloqueados -> transferência
    2) reabastecer o pool de reserva quando estiver baixo
    3) juntar-se a workers desanexados que terminaram
    4) atender à solicitação de expansão (stub)
------------------------------------------------------------------------- */

#include "thread_pool.h"
#include "thread_handler.h"
#include <stdlib.h>
#include <string.h>

 /* -------------------------------------------------------------------------
  * Internal types.
  * ------------------------------------------------------------------------- */

  /* Worker lifecycle states. */
enum {
    WSTATE_RESERVE = 0,   /* in reserve pool, thread parked, no shard */
    WSTATE_ACTIVE = 1,   /* assigned to a shard, running its loop */
    WSTATE_DETACHED = 2,   /* was active, now stuck on a long task; will exit */
    WSTATE_EXITING = 3    /* finished, waiting to be joined and freed */
};

struct Worker {
    /* Identity & thread. */
    int               id;
    void*             thread;
    xthread_handle_t  thread_handle;   /* for activity sampling from monitor */

    /* Shard assignment (NULL when in reserve or detached after task ends). */
    Shard* home_shard;

    /* Park primitive. */
    xwait_t           wait;
    xatomic_int       sleeping;        /* 0/1 */

    /* Task tracking for activity-based handoff. */
    xatomic_int       task_in_progress; /* 0/1 */
    xthread_sample_t  task_start_sample;

    /* Lifecycle. */
    xatomic_int       state;            /* one of WSTATE_* */

    /* Back-reference for the worker_fn. */
    ShardedPool* pool;

    /* Linked list pointer (used for reserve & detached lists). */
    struct Worker* next;
};

struct Shard {
    int          id;
    RingQueue    ring;
    Task*        buffer;
    int          capacity;

    /* Currently active worker for this shard. NULL means orphaned
     * (waiting for monitor to pull from reserve). */
    xatomic_ptr  active_worker;        /* Worker* */
};

/* Simple intrusive linked list with a spinlock for reserve & detached. */
typedef struct 
{
    Worker*      head;
    int          count;
    xatomic_int  lock;                 /* spinlock; low contention */
} WorkerList;

struct ShardedPool {
    /* Shards. */
    Shard**      shards;
    xatomic_int  shard_count;
    int          max_shards;

    /* Submit round-robin. */
    xatomic_int  submit_idx;

    /* Stats / signals. */
    xatomic_int  pending;
    xatomic_int  expand_requested;
    xatomic_int  total_submitted;
    xatomic_int  submit_failures;
    xatomic_int  total_handoffs;

    /* Reserve & detached lists. */
    WorkerList   reserve;
    WorkerList   detached;
    int          reserve_target;

    /* Monitor. */
    void*        monitor_thread;
    xatomic_int  shutdown;
    int          monitor_interval_ms;

    /* Worker config (passed to new workers). */
    int          spin_iterations;
    int          ring_capacity;
    xtask_thresholds_t task_thresholds;

    /* Worker id allocator. */
    xatomic_int   next_worker_id;
};

/* -------------------------------------------------------------------------
 * Forward declarations of internal functions.
 * ------------------------------------------------------------------------- */
static void*   worker_fn(void* arg);
static void    worker_idle_phase(Worker* w);
static Worker* worker_create(ShardedPool* pool);
static void    worker_destroy(Worker* w);
static void    worker_assign_to_shard(Worker* w, Shard* s);

static Shard*  shard_create(int id, int capacity);
static void    shard_destroy(Shard* s);
static int     shard_usage_pct(const Shard* s);

static void    list_init(WorkerList* l);
static void    list_push(WorkerList* l, Worker* w);
static Worker* list_pop(WorkerList* l);
static Worker* list_pop_finished(WorkerList* l);  /* pops a worker in WSTATE_EXITING */

static void*   monitor_fn(void* arg);
static void    monitor_check_handoff(ShardedPool* pool);
static void    monitor_replenish_reserve(ShardedPool* pool);
static void    monitor_join_finished(ShardedPool* pool);
static void    monitor_handle_expand(ShardedPool* pool);



/* -------------------------------------------------------------------------
* Acorda o trabalhador ativo de um shard se ele estiver em repouso.
* Chamado pelo comando submit após um push bem-sucedido.
* ------------------------------------------------------------------------- */
static inline void shard_wake(Shard* s)
{
    Worker* w = (Worker*)atomic_get_ptr(&s->active_worker);
    if (!w) return;
    if (atomic_get(&w->sleeping))
    {
        thread_wait_wake(&w->wait);
    }
}


PoolConfig pool_default_config(void) 
{
    PoolConfig c;
    memset(&c, 0, sizeof(c));

    c.shard_count         = 0;     /* auto-detect */
    c.max_shards          = POOL_DEFAULT_MAX_SHARDS;
    c.ring_capacity       = POOL_DEFAULT_RING_CAPACITY;
    c.spin_iterations     = POOL_DEFAULT_SPIN_ITERATIONS;
    c.reserve_size        = POOL_DEFAULT_RESERVE_SIZE;
    c.monitor_interval_ms = POOL_MONITOR_INTERVAL_MS;
    c.task_thresholds_set = false;
    return c;
}


static void list_init(WorkerList* l) 
{
    l->head = NULL;
    l->count = 0;
    atomic_set(&l->lock, 0);
}


static inline void list_lock(WorkerList* l)
{
    int expected = 0;
    while (!atomic_cas(&l->lock, &expected, 1))
    {
        /* Spin com loads simples até parecer livre, só então tenta CAS de novo. */
        while (atomic_get(&l->lock) != 0)
        {
            xcpu_pause();
        }
        expected = 0;
    }
}


static inline void list_unlock(WorkerList* l) 
{
    atomic_set(&l->lock, 0);
}

static void list_push(WorkerList* l, Worker* w) 
{
    list_lock(l);
    w->next = l->head;
    l->head = w;
    l->count++;
    list_unlock(l);
}

static Worker* list_pop(WorkerList* l) 
{
    list_lock(l);
    Worker* w = l->head;
    if (w) {
        l->head = w->next;
        w->next = NULL;
        l->count--;
    }
    list_unlock(l);
    return w;
}


static Worker* list_pop_finished(WorkerList* l) 
{
    list_lock(l);
    Worker** pp = &l->head;
    while (*pp) 
    {
        if (atomic_get(&(*pp)->state) == WSTATE_EXITING)
        {
            Worker* w = *pp;
            *pp = w->next;
            w->next = NULL;
            l->count--;
            list_unlock(l);
            return w;
        }
        pp = &(*pp)->next;
    }
    list_unlock(l);
    return NULL;
}


static Shard* shard_create(int id, int capacity)
{
    Shard* s = (Shard*)calloc(1, sizeof(Shard));
    if (!s) return NULL;
    s->id = id;
    s->capacity = capacity;
    s->buffer = (Task*)calloc(capacity, sizeof(Task));
    if (!s->buffer) { free(s); return NULL; }

    ring_queue_init(&s->ring, capacity);

    atomic_set_ptr(&s->active_worker, NULL);
    return s;
}

static void shard_destroy(Shard* s) 
{
    if (!s) return;
    ring_queue_destroy(&s->ring);
    free(s->buffer);
    free(s);
}

static int shard_usage_pct(const Shard* s)
{
    int used = ring_queue_count((RingQueue*)&s->ring);
    return (used * 100) / s->capacity;
}


static Worker* worker_create(ShardedPool* pool) 
{
    Worker* w = (Worker*)calloc(1, sizeof(Worker));
    if (!w) return NULL;

    w->id = atomic_add(&pool->next_worker_id, 1);
    w->pool = pool;
    w->home_shard = NULL;
    atomic_set(&w->state, WSTATE_RESERVE);
    atomic_set(&w->sleeping, 0);
    atomic_set(&w->task_in_progress, 0);

    if (thread_create(&w->thread, worker_fn, w) == 0)
    {
        thread_wait_destroy(&w->wait);
        free(w);
        return NULL;
    }
    w->thread_handle = (xthread_handle_t)w->thread;

    return w;
}

static void worker_destroy(Worker* w) 
{
    if (!w) return;
    thread_wait_destroy(&w->wait);
    free(w);
}

/* -------------------------------------------------------------------------
 Atribua um worker de reserva a um shard. O chamador possui o worker (por exemplo,
 acabou de sair da reserva) e deseja que ele comece a atender o shard.

 O worker está atualmente parado em worker_fn aguardando atribuição. Nós
 definimos home_shard, fazemos a transição para ACTIVE, publicamos no slot active_worker do shard
 e então o ativamos.
 ------------------------------------------------------------------------- */
static void worker_assign_to_shard(Worker* w, Shard* s) 
{
    w->home_shard = s;
    atomic_set(&w->state, WSTATE_ACTIVE);
    atomic_set_ptr(&s->active_worker, w);

    /* Wake the worker out of its initial reserve park. */
    atomic_set(&w->sleeping, 0);
    thread_wait_wake(&w->wait);
}


/* -------------------------------------------------------------------------
 Fase ociosa do trabalhador.

 Substitui o par original pool_wake_one()/thread_wait_sleep() por
 a sequência integrada drain+spin+park especificada.

 Chamado por worker_fn após a conclusão de cada tarefa. Retorna quando o
 trabalhador deve remover outra tarefa (o anel está em execução) OU quando shutdown/
 detach foi sinalizado (o chamador verifica o estado).
 ------------------------------------------------------------------------- */
static void worker_idle_phase(Worker* w) 
{
    Shard* s = w->home_shard;
    Task   t;

    while (xring_pop(&s->ring, s->buffer, &t))
    {
        atomic_sub(&w->pool->pending, 1);

        w->task_start_sample = xthread_sample_self();
        atomic_set(&w->task_in_progress, 1);

        t.fn(t.arg);

        atomic_set(&w->task_in_progress, 0);

        if (atomic_get(&w->state) != WSTATE_ACTIVE) return;
        if (atomic_get(&w->pool->shutdown))         return;
    }

    int spin_iters = w->pool->spin_iterations;
    for (int i = 0; i < spin_iters; i++) 
    {
        if (ring_queue_count(&s->ring) > 0) return; /* work ready */
        if (atomic_get(&w->state) != WSTATE_ACTIVE)  return;
        if (atomic_get(&w->pool->shutdown))          return;
        xcpu_pause();
    }

    atomic_set(&w->sleeping, 1);

    if (ring_queue_count(&s->ring) > 0 || atomic_get(&w->state) != WSTATE_ACTIVE || atomic_get(&w->pool->shutdown))
    {
        atomic_set(&w->sleeping, 0);
        return;
    }

    thread_wait_sleep(&w->wait);
    atomic_set(&w->sleeping, 0);
}



/* -------------------------------------------------------------------------

 Função da thread de trabalho.

 Ciclo de vida:
 - Nascida em WSTATE_RESERVE: estaciona imediatamente, aguarda até ser atribuída.
 - WSTATE_ACTIVE: executa o loop de drenagem/giro/estacionamento vinculado ao home_shard.
 - WSTATE_DETACHED (definido pelo monitor): o chamador está no meio da tarefa; ao retornar
   da função, sai do loop, transita para EXITING e permite que o monitor
   entre.
 - WSTATE_EXITING: a função da thread retorna; o monitor entrará.

* ------------------------------------------------------------------------- */
static void* worker_fn(void* arg)
{
    Worker* w = (Worker*)arg;
    thread_wait_prepare(&w->wait);

    // aguarda fim de todas execucoes
    while (atomic_get(&w->state) == WSTATE_RESERVE) 
    {
        if (atomic_get(&w->pool->shutdown)) 
        {
            atomic_set(&w->state, WSTATE_EXITING);
            return NULL;
        }
        atomic_set(&w->sleeping, 1);

        if (atomic_get(&w->state) != WSTATE_RESERVE)
        {
            atomic_set(&w->sleeping, 0);
            break;
        }
        thread_wait_sleep(&w->wait);
        atomic_set(&w->sleeping, 0);
    }

    while (atomic_get(&w->state) == WSTATE_ACTIVE && !atomic_get(&w->pool->shutdown))
    {
        worker_idle_phase(w);
    }

    atomic_set(&w->state, WSTATE_EXITING);
    return NULL;
}


bool pool_submit(ShardedPool* pool, task_fn fn, void* arg) 
{
    int idx = atomic_add(&pool->submit_idx, 1);
    int count = atomic_get(&pool->shard_count);
    Task task = { fn, arg };

    for (int i = 0; i < count; i++) 
    {
        Shard* s = pool->shards[(idx + i) % count];

        if (s && xring_push_mp(&s->ring, s->buffer, &task)) 
        {
            atomic_add(&pool->pending, 1);
            atomic_add(&pool->total_submitted, 1);
            shard_wake(s);

            if (shard_usage_pct(s) >= POOL_PRESSURE_THRESHOLD_PCT)
            {
                atomic_set(&pool->expand_requested, 1);
            }
            return true;
        }
    }

    /* Defense in depth: all shards full. */
    atomic_add(&pool->submit_failures, 1);
    atomic_set(&pool->expand_requested, 1);
    return false;
}

/* -------------------------------------------------------------------------
* Monitor: detecção de handover.
*
* Para cada shard, amostra a atividade da CPU do worker ativo. Se a tarefa
* for longa E estiver bloqueada (taxa baixa), execute o handover:
* 1) Desanexe o worker (estado := DESANEXADO, remova do shard).
* 2) Retire um novo worker da reserva.
* 3) Atribua-o ao shard.
* 4) Adicione o worker antigo à lista de desanexados.
*
* Se a reserva estiver vazia, ignore — é melhor suportar a tarefa longa do que
* travar o monitor criando um worker de forma síncrona. O reabastecimento é executado
* após isso e reabastecerá para o próximo ciclo.
 * ------------------------------------------------------------------------- */
static void monitor_check_handoff(ShardedPool* pool)
{
    int count = atomic_get(&pool->shard_count);
    for (int i = 0; i < count; i++) {
        Shard* s = pool->shards[i];
        if (!s) continue;

        Worker* w = (Worker*)atomic_get_ptr(&s->active_worker);
        if (!w) continue;
        if (!atomic_get(&w->task_in_progress)) continue;

        xthread_sample_t now = xthread_sample_of(w->thread_handle);
        if (now.wall_ns == 0) continue;  /* sample failed */

        xtask_eval_t eval = xthread_evaluate_task(&w->task_start_sample, &now, &pool->task_thresholds);

        if (!xthread_should_handoff(&eval)) continue;

        /* Try to pull a replacement before committing the handoff.
         * If reserve is empty, abort — wait for next cycle. */
        Worker* replacement = list_pop(&pool->reserve);
        if (!replacement) continue;

        /* Commit: detach old, install new. The old worker, when it
         * returns from fn, will see state != ACTIVE and exit. */
        atomic_set(&w->state, WSTATE_DETACHED);
        atomic_set_ptr(&s->active_worker, NULL);

        worker_assign_to_shard(replacement, s);

        list_push(&pool->detached, w);
        atomic_add(&pool->total_handoffs, 1);
    }
}


static void monitor_replenish_reserve(ShardedPool* pool)
{
    while (pool->reserve.count < pool->reserve_target) 
    {
        Worker* w = worker_create(pool);
        if (!w) break;  /* OOM or thread create failed; try again later */
        list_push(&pool->reserve, w);
    }
}


static void monitor_join_finished(ShardedPool* pool) 
{
    Worker* w;
    while ((w = list_pop_finished(&pool->detached)) != NULL)
    {
        thread_join(&w->thread);
        worker_destroy(w);
    }
}


static void monitor_handle_expand(ShardedPool* pool) 
{
    int expected = 1;
    if (!atomic_cas(&pool->expand_requested, &expected, 0)) return;
    (void)pool;
}


static void* monitor_fn(void* arg) 
{
    ShardedPool* pool = (ShardedPool*)arg;
    while (!atomic_get(&pool->shutdown))
    {
        xsleep_ms(pool->monitor_interval_ms);
        monitor_check_handoff(pool);
        monitor_handle_expand(pool);
        monitor_replenish_reserve(pool);
        monitor_join_finished(pool);
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * Pool create.
 * ------------------------------------------------------------------------- */
ShardedPool* pool_create(const PoolConfig* user_cfg) 
{
    PoolConfig cfg = pool_default_config();
    if (user_cfg) cfg = *user_cfg;
    if (cfg.shard_count <= 0) cfg.shard_count = xcpu_count();
    if (cfg.shard_count <= 0) cfg.shard_count = 4;

    xthread_activity_init();
    thread_wait_init(false);

    ShardedPool* pool = (ShardedPool*)calloc(1, sizeof(ShardedPool));
    if (!pool) return NULL;

    pool->max_shards = cfg.max_shards;
    pool->ring_capacity = cfg.ring_capacity;
    pool->spin_iterations = cfg.spin_iterations;
    pool->reserve_target = cfg.reserve_size;
    pool->monitor_interval_ms = cfg.monitor_interval_ms;

    if (cfg.task_thresholds_set) 
    {
        pool->task_thresholds = cfg.task_thresholds;
    }
    else 
    {
        pool->task_thresholds.long_threshold_ns = XTASK_DEFAULT_LONG_NS;
        pool->task_thresholds.blocked_ratio_max = XTASK_DEFAULT_BLOCKED_RATIO;
        pool->task_thresholds.cpu_ratio_min = XTASK_DEFAULT_CPU_RATIO;
    }

    list_init(&pool->reserve);
    list_init(&pool->detached);

    /* Allocate shards array sized to max_shards (grow-only). */
    pool->shards = (Shard**)calloc(pool->max_shards, sizeof(Shard*));
    if (!pool->shards) { free(pool); return NULL; }

    /* Create initial shards and one worker per shard. */
    for (int i = 0; i < cfg.shard_count; i++) {
        Shard* s = shard_create(i, pool->ring_capacity);
        if (!s) goto fail;
        pool->shards[i] = s;

        Worker* w = worker_create(pool);
        if (!w) goto fail;
        worker_assign_to_shard(w, s);
    }

    atomic_set(&pool->shard_count, cfg.shard_count);

    /* Pre-fill reserve. */
    for (int i = 0; i < pool->reserve_target; i++) 
    {
        Worker* w = worker_create(pool);
        if (!w) break;  /* tolerate; monitor will retry */
        list_push(&pool->reserve, w);
    }

    /* Spawn monitor. */
    if (thread_create(&pool->monitor_thread, monitor_fn, pool) == 0)
    {
        goto fail;
    }

    return pool;

fail:
    pool_shutdown(pool);
    return NULL;
}

/* -------------------------------------------------------------------------
 * Pool shutdown.
 *
 * Order matters:
 *   1) Set shutdown flag.
 *   2) Wake every worker (active, reserve, and any detached not yet
 *      stuck in a task) so they exit their loops.
 *   3) Join monitor (it will stop scheduling new work).
 *   4) Join all workers — including detached, which may take a while
 *      if their long task hasn't finished. This is unavoidable: we
 *      can't kill a thread mid-task.
 *   5) Free shards.
 * ------------------------------------------------------------------------- */
void pool_shutdown(ShardedPool* pool) 
{
    if (!pool) return;
    atomic_set(&pool->shutdown, 1);

    /* Wake everyone. */
    int count = atomic_get(&pool->shard_count);
    for (int i = 0; i < count; i++) {
        Shard* s = pool->shards[i];
        if (!s) continue;
        Worker* w = (Worker*)atomic_get_ptr(&s->active_worker);
        if (w) thread_wait_wake(&w->wait);
    }
    /* Drain reserve and wake. */
    Worker* w;
    while ((w = list_pop(&pool->reserve)) != NULL) 
    {
        atomic_set(&w->state, WSTATE_EXITING); /* nudge it out of reserve loop */
        thread_wait_wake(&w->wait);
        thread_join(&w->thread);
        worker_destroy(w);
    }

    /* Stop monitor. */
    if (pool->monitor_thread) thread_join(&pool->monitor_thread);

    /* Join active workers (still attached to shards). */
    for (int i = 0; i < count; i++) 
    {
        Shard* s = pool->shards[i];
        if (!s) continue;
        Worker* aw = (Worker*)atomic_get_ptr(&s->active_worker);

        if (aw)
        {
            thread_join(&aw->thread);
            worker_destroy(aw);
        }
    }

    /* Join detached workers (may block until their long tasks finish). */
    while ((w = list_pop(&pool->detached)) != NULL)
    {
        thread_join(&w->thread);
        worker_destroy(w);
    }

    /* Free shards. */
    for (int i = 0; i < count; i++) 
    {
        shard_destroy(pool->shards[i]);
    }
    free(pool->shards);
    free(pool);
}

/* -------------------------------------------------------------------------
 * Stats.
 * ------------------------------------------------------------------------- */
void pool_stats(ShardedPool* pool, PoolStats* out) 
{
    if (!pool || !out) return;
    int count = atomic_get(&pool->shard_count);

    int active = 0;
    for (int i = 0; i < count; i++)
    {
        Shard* s = pool->shards[i];
        if (s && atomic_get_ptr(&s->active_worker)) active++;
    }

    out->shard_count         = count;
    out->active_worker_count = active;
    out->reserve_count       = pool->reserve.count;
    out->detached_count      = pool->detached.count;
    out->pending_tasks       = atomic_get(&pool->pending);
    out->total_submitted     = (uint64_t)atomic_get(&pool->total_submitted);
    out->submit_failures     = (uint64_t)atomic_get(&pool->submit_failures);
    out->total_handoffs      = (uint64_t)atomic_get(&pool->total_handoffs);
}