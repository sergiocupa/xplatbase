/*
 * memory_pool - pool de alocacao por size-class, estilo mimalloc/rpmalloc.
 *
 * Tecnicas adotadas (melhor de cada lib, mantendo simples):
 *   - Span alinhado de 64KB; metadado achado por MASCARA do ponteiro
 *     (mimalloc/rpmalloc): ZERO header por objeto.
 *   - Size-classes finas (~25% de passo) + mapeamento size->classe O(1) (tabela):
 *     baixa fragmentacao interna e lookup constante no hot path.
 *   - Heap por-thread (TLS); listas de spans por classe.
 *   - Caminho local lock-free E sem atomico: free-list por span tocada so pelo
 *     dono; contadores (used/stats) tambem so do dono.
 *   - Free remoto: push atomico (Treiber) na remote_free do span; o dono drena
 *     e ajusta 'used' (subtrai a contagem drenada) -> 'used' continua nao-atomico.
 *   - Cache de chunks de 64KB em 2 niveis (por-thread sem lock + global): reuso
 *     entre classes/threads e devolucao ao SO ao exceder o cap.
 *   - Refill SINCRONO por span (sem thread de background).
 */

#include "memory_pool.h"
#include "atomics.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef XPLATBASE_WIN
#include <malloc.h>
#define MEMOP_TLS __declspec(thread)
#else
#define MEMOP_TLS __thread
#endif

#ifndef MEMOP_CLASS_COUNT
#define MEMOP_CLASS_COUNT 36
#endif

#define MEMOP_MAX_SMALL   16384u   /* acima disso: caminho large */

#define MEMOP_SPAN_SIZE   (64u * 1024u)
#define MEMOP_SPAN_MASK   (~(uintptr_t)(MEMOP_SPAN_SIZE - 1u))
#define MEMOP_SPAN_HEADER 64u   /* offset alinhado onde comecam os blocos */

#define MEMOP_CLASS_LARGE 0xFFFFFFFFu  /* sentinela: bloco grande (passthrough OS) */

#define MEMOP_UNINIT 0
#define MEMOP_BUSY   1
#define MEMOP_READY  2

/* Stats sao observabilidade: ON por padrao (o teste depende delas). Defina
 * MEMOP_NO_STATS para zerar o custo no hot path (build de maxima performance). */
#ifdef MEMOP_NO_STATS
#define MEMOP_STAT(x) ((void)0)
#else
#define MEMOP_STAT(x) do { x; } while (0)
#endif

typedef struct MemFreeBlock
{
    struct MemFreeBlock* next;
} MemFreeBlock;

typedef struct MemHeap MemHeap;

/* Cabecalho no inicio de cada span 64KB-alinhado. */
typedef struct MemSpan
{
    MemHeap*       owner;
    struct MemSpan* next;        /* lista (dupla) da classe no heap dono */
    struct MemSpan* prev;
    MemFreeBlock*  local_free;   /* so o dono toca (sem atomico)  */
    xatomic_ptr    remote_free;  /* frees cross-thread (Treiber)  */
    uint32 class_id;
    uint32 stride;               /* tamanho usavel por bloco       */
    uint32 block_count;          /* nro de blocos no span (large: unidades 64KB) */
    uint32 used;                 /* so o dono ajusta (nao-atomico) */
    uint32 bump_idx;             /* carve PREGUICOSO: proximo bloco nunca entregue */
} MemSpan;
/* sizeof(MemSpan) deve caber em MEMOP_SPAN_HEADER (64B): 5 ptr + 5 uint32 = 60 -> 64 */

struct MemHeap
{
    MemSpan* spans[MEMOP_CLASS_COUNT];
    uint32   id;
    int      active;
    MemHeap* hnext;
    /* cache de chunks de 64KB da PROPRIA thread: reuso SEM lock (hot path). */
    void*    local_cache;
    int      local_count;
    /* stats da PROPRIA thread (nao-atomicas, agregadas em get_stats) */
    uint64 s_alloc;
    uint64 s_free;
    uint64 s_span_allocs;
    uint64 s_remote_frees;
};

#ifndef MEMOP_HEAP_CACHE_MAX
#define MEMOP_HEAP_CACHE_MAX 64    /* 64 * 64KB = 4 MB por thread, sem lock */
#endif

#ifndef MEMOP_SEG_SPANS
#define MEMOP_SEG_SPANS 64         /* 64 * 64KB = 4 MB por segmento do SO */
#endif
#define MEMOP_SEG_SIZE ((size_t)MEMOP_SEG_SPANS * MEMOP_SPAN_SIZE)

/* Um segmento = uma alocacao grande e alinhada do SO, fatiada em MEMOP_SEG_SPANS
 * chunks de 64KB. A memoria de um segmento NUNCA volta ao SO em runtime: fica
 * cacheada para reuso e so e liberada no shutdown (poucos mmap/commit, sem churn). */
typedef struct MemSegment
{
    struct MemSegment* next;
    void*  base;               /* ponteiro original (span_os_alloc) p/ liberar */
    uint32 next_chunk;         /* bump: proximo chunk de 64KB ainda nao entregue */
} MemSegment;

typedef struct MemPool
{
    xmutex_t lock;        /* somente lifecycle: lista de heaps, ids, stats */
    MemHeap* heaps;
    uint32   next_id;
    uint64   heaps_created;
    uint64   heaps_destroyed;

    /* Cache global de chunks de 64KB vazios, reutilizaveis por qualquer
     * classe/thread (e pelo large de 1 unidade). Chunks pertencem a segmentos. */
    xmutex_t    cache_lock;
    void*       span_cache;   /* lista simples: link no offset 0 do chunk */
    int         cache_count;
    MemSegment* segments;     /* todos os segmentos, liberados no shutdown */
} MemPool;

/* Classes finas (~25% de passo): baixa fragmentacao interna sem sacrificar
 * velocidade. O mapeamento size->classe e O(1) via g_size2class. */
static const uint32 MemClassSizes[MEMOP_CLASS_COUNT] = {
    16,32,48,64, 80,96,112,128, 160,192,224,256, 320,384,448,512,
    640,768,896,1024, 1280,1536,1792,2048, 2560,3072,3584,4096,
    5120,6144,7168,8192, 10240,12288,14336,16384 };

static MemPool        G;
static xatomic_int    g_init_state;
static MEMOP_TLS MemHeap* g_heap;

/* Tabela de lookup O(1): indice = (size-1)>>3 (granularidade de 8 bytes). */
static uint8_t g_size2class[MEMOP_MAX_SMALL / 8];

/* Stride (tamanho real do bloco) por classe, pre-computado: evita recalcular
 * e evita ler span->stride no hot path da alloc (uma dependencia de memoria a menos). */
static uint32 g_class_stride[MEMOP_CLASS_COUNT];




/* ----------------------------------------------------------------------- */
/*  SO: alocacao de span alinhado a MEMOP_SPAN_SIZE                        */
/* ----------------------------------------------------------------------- */

static void* span_os_alloc(size_t size)
{
#ifdef XPLATBASE_WIN
    return _aligned_malloc(size, MEMOP_SPAN_SIZE);
#else
    return aligned_alloc(MEMOP_SPAN_SIZE, size);
#endif
}

static void span_os_free(void* p)
{
#ifdef XPLATBASE_WIN
    _aligned_free(p);
#else
    free(p);
#endif
}

/* Cache de chunks de 64KB em dois niveis:
 *   - por-thread (heap->local_cache): SEM lock, e o caminho normal (hot).
 *   - global (G.span_cache): so quando o local esgota/transborda; balanceia
 *     entre threads.
 * Quando ambos esgotam, dola-se um chunk por BUMP de um segmento grande do SO
 * (amortiza mmap/commit em MEMOP_SEG_SPANS chunks, sem tocar paginas a frente).
 * Nada volta ao SO ate o shutdown. */
static void* chunk_acquire(MemHeap* heap)
{
    void* mem;
    void* base;
    MemSegment* seg;

    if (heap && heap->local_cache)
    {
        mem = heap->local_cache;
        heap->local_cache = *(void**)mem;
        heap->local_count--;
        return mem;
    }

    thread_mutex_lock(&G.cache_lock);
    /* 1) chunk liberado (reuso de algo ja tocado) */
    if (G.span_cache)
    {
        mem = G.span_cache;
        G.span_cache = *(void**)mem;
        G.cache_count--;
        thread_mutex_unlock(&G.cache_lock);
        return mem;
    }
    /* 2) bump do segmento corrente: dola o proximo chunk SEM toca-lo (a pagina
     *    so falha quando o chunk for realmente usado) -> sem storm de page fault. */
    if (G.segments && G.segments->next_chunk < MEMOP_SEG_SPANS)
    {
        seg = G.segments;
        mem = (char*)seg->base + (size_t)seg->next_chunk * MEMOP_SPAN_SIZE;
        seg->next_chunk++;
        thread_mutex_unlock(&G.cache_lock);
        return mem;
    }
    thread_mutex_unlock(&G.cache_lock);

    /* 3) segmento esgotado/inexistente: aloca um novo (1 chamada do SO p/ ate
     *    MEMOP_SEG_SPANS chunks). chunk 0 vai pro chamador; o resto via bump. */
    base = span_os_alloc(MEMOP_SEG_SIZE);
    if (!base) return NULL;
    seg = (MemSegment*)malloc(sizeof(MemSegment));
    if (!seg) { span_os_free(base); return NULL; }
    seg->base = base;
    seg->next_chunk = 1;

    thread_mutex_lock(&G.cache_lock);
    seg->next = G.segments;
    G.segments = seg;
    thread_mutex_unlock(&G.cache_lock);

    return base;   /* chunk 0 */
}

static void chunk_release(MemHeap* heap, void* mem)
{
    if (heap && heap->local_count < MEMOP_HEAP_CACHE_MAX)
    {
        *(void**)mem = heap->local_cache;
        heap->local_cache = mem;
        heap->local_count++;
        return;
    }
    /* Excedente -> cache global. SEM devolucao ao SO: o chunk pertence a um
     * segmento (liberado so no shutdown). Reuso vale mais que churn de mmap. */
    thread_mutex_lock(&G.cache_lock);
    *(void**)mem = G.span_cache;
    G.span_cache = mem;
    G.cache_count++;
    thread_mutex_unlock(&G.cache_lock);
}

/* Move o cache local de um heap (thread que terminou) para o cache global. */
static void chunk_local_to_global(MemHeap* heap)
{
    void* mem = heap->local_cache;
    heap->local_cache = NULL;
    heap->local_count = 0;
    while (mem)
    {
        void* nx = *(void**)mem;
        chunk_release(NULL, mem);   /* heap=NULL -> vai pro global/SO */
        mem = nx;
    }
}

/* Libera TODA a memoria de chunks de uma vez (no shutdown): os ponteiros nos
 * caches sao apenas fatias dos segmentos, entao basta liberar os segmentos. */
static void segments_free_all(void)
{
    MemSegment* s;
    thread_mutex_lock(&G.cache_lock);
    s = G.segments;
    G.segments = NULL;
    G.span_cache = NULL;   /* ponteiros p/ dentro dos segmentos: descartados */
    G.cache_count = 0;
    thread_mutex_unlock(&G.cache_lock);
    while (s) { MemSegment* nx = s->next; span_os_free(s->base); free(s); s = nx; }
}

static uint32 memop_align_up(uint32 v, uint32 a)
{
    return (uint32)((v + a - 1u) & ~(a - 1u));
}

/* O(1): uma indexacao de tabela (vs varredura linear). */
static int memop_class_index(uint64 size)
{
    if (size == 0 || size > MEMOP_MAX_SMALL) return -1;
    return (int)g_size2class[(size - 1u) >> 3];
}

/* Monta g_size2class uma vez no init: cada bucket de 8B aponta para a menor
 * classe que o atende. Como toda classe e multipla de 8, o mapeamento e exato. */
static void memop_build_size2class(void)
{
    uint32 idx, ci;
    for (ci = 0; ci < MEMOP_CLASS_COUNT; ci++)
        g_class_stride[ci] = memop_align_up(MemClassSizes[ci], 16u);
    for (idx = 0; idx < (MEMOP_MAX_SMALL / 8u); idx++)
    {
        uint32 sz = (idx + 1u) * 8u;
        ci = 0;
        while (ci < MEMOP_CLASS_COUNT && MemClassSizes[ci] < sz) ci++;
        g_size2class[idx] = (uint8_t)ci;
    }
}




/* ----------------------------------------------------------------------- */
/*  Lifecycle (lock global SO aqui)                                        */
/* ----------------------------------------------------------------------- */

static void memop_lock(void)   { thread_mutex_lock(&G.lock); }
static void memop_unlock(void) { thread_mutex_unlock(&G.lock); }

void memop_init(void)
{
    for (;;)
    {
        int st = atomic_get(&g_init_state);
        if (st == MEMOP_READY) return;
        if (st == MEMOP_BUSY) { thread_yield(); continue; }
        {
            int expected = MEMOP_UNINIT;
            if (atomic_cas(&g_init_state, &expected, MEMOP_BUSY))
            {
                thread_mutex_init(&G.lock);
                thread_mutex_init(&G.cache_lock);
                memop_build_size2class();
                atomic_set(&g_init_state, MEMOP_READY);
                return;
            }
        }
    }
}




/* ----------------------------------------------------------------------- */
/*  Spans                                                                  */
/* ----------------------------------------------------------------------- */

/* ptr -> span pelo alinhamento (sem header por objeto). */
static MemSpan* span_of(void* ptr)
{
    return (MemSpan*)((uintptr_t)ptr & MEMOP_SPAN_MASK);
}

static MemSpan* span_create(MemHeap* heap, uint32 class_id)
{
    MemSpan* span;
    uint32   stride = g_class_stride[class_id];
    void*    mem = chunk_acquire(heap);      /* cache local -> global -> SO */
    if (!mem) return NULL;

    span = (MemSpan*)mem;
    span->owner = heap;
    span->next = NULL;
    span->prev = NULL;
    span->local_free = NULL;                 /* nada pre-carved */
    atomic_set_ptr(&span->remote_free, NULL);
    span->class_id = class_id;
    span->stride = stride;
    span->used = 0;
    /* Carve PREGUICOSO: blocos sao entregues incrementando bump_idx (sem escrever
     * a free-list inteira na criacao). So blocos LIBERADOS vao para local_free. */
    span->bump_idx = 0;
    span->block_count = (MEMOP_SPAN_SIZE - MEMOP_SPAN_HEADER) / stride;
    return span;
}

/* Drena a lista remota para a local; devolve quantos blocos vieram. */
static uint32 span_drain_remote(MemSpan* span)
{
    void* head = atomic_get_ptr(&span->remote_free);
    MemFreeBlock* b;
    uint32 n = 0;
    if (!head) return 0;
    while (!atomic_cas_ptr(&span->remote_free, &head, NULL)) { /* retry */ }

    b = (MemFreeBlock*)head;
    while (b)
    {
        MemFreeBlock* nx = b->next;
        b->next = span->local_free;
        span->local_free = b;
        n++;
        b = nx;
    }
    return n;
}




/* ----------------------------------------------------------------------- */
/*  Heap por-thread                                                        */
/* ----------------------------------------------------------------------- */

static MemHeap* heap_create_locked(void)
{
    MemHeap* h = (MemHeap*)calloc(1, sizeof(MemHeap));
    if (!h) return NULL;
    h->active = 1;
    h->id = ++G.next_id;
    h->hnext = G.heaps;
    G.heaps = h;
    G.heaps_created++;
    return h;
}

static MemHeap* get_heap(void)
{
    if (g_heap) return g_heap;

    memop_init();
    memop_lock();
    g_heap = heap_create_locked();
    memop_unlock();
    return g_heap;
}

static void heap_destroy(MemHeap* h)
{
    if (!h) return;
    /* spans[] e local_cache apontam para fatias de segmentos (liberadas em
     * segments_free_all no shutdown); aqui so liberamos a struct do heap. */
    free(h);
}




/* ----------------------------------------------------------------------- */
/*  Caminho quente: alloc / free                                           */
/* ----------------------------------------------------------------------- */

static void heap_list_insert_head(MemHeap* heap, uint32 class_id, MemSpan* span)
{
    MemSpan* head = heap->spans[class_id];
    span->prev = NULL;
    span->next = head;
    if (head) head->prev = span;
    heap->spans[class_id] = span;
}

static void heap_list_unlink(MemHeap* heap, uint32 class_id, MemSpan* span)
{
    if (span->prev) span->prev->next = span->next;
    else            heap->spans[class_id] = span->next;
    if (span->next) span->next->prev = span->prev;
    span->prev = span->next = NULL;
}

/* Entrega um bloco da classe quando a cabeca nao tem local_free imediato.
 * Tenta, em ordem: bump da cabeca -> outro span com free/bump/remote -> span novo.
 * Promove o span util para a cabeca (fast path da proxima alloc) e ja faz used++. */
static MemFreeBlock* memop_refill(MemHeap* heap, uint32 class_id)
{
    uint32   stride = g_class_stride[class_id];
    MemSpan* span = heap->spans[class_id];
    MemFreeBlock* b;

    while (span)
    {
        b = span->local_free;
        if (b) { span->local_free = b->next; goto got; }
        if (span->bump_idx < span->block_count)
        {
            b = (MemFreeBlock*)((char*)span + MEMOP_SPAN_HEADER + (size_t)span->bump_idx * stride);
            span->bump_idx++;
            goto got;
        }
        if (span_drain_remote(span) > 0) { b = span->local_free; span->local_free = b->next; goto got; }
        span = span->next;
    }

    /* nenhum span util: cria um (carve preguicoso, O(1)) e entrega o 1o via bump */
    span = span_create(heap, class_id);
    if (!span) return NULL;
    heap_list_insert_head(heap, class_id, span);
    MEMOP_STAT(heap->s_span_allocs++);
    b = (MemFreeBlock*)((char*)span + MEMOP_SPAN_HEADER);
    span->bump_idx = 1;

got:
    if (heap->spans[class_id] != span)
    {
        heap_list_unlink(heap, class_id, span);
        heap_list_insert_head(heap, class_id, span);
    }
    span->used++;
    return b;
}

/* Caminho FRIO (>16KB): alocacao do SO 64KB-alinhada, marcada LARGE no header
 * do span-base; o free roteia pela mesma mascara de ponteiro. Fica fora da
 * funcao quente para nao inchar o hot path (I-cache / inlining do caminho small). */
static void* memop_alloc_large(MemHeap* heap, uint64 size)
{
    size_t total = (size_t)size + MEMOP_SPAN_HEADER;
    size_t units;
    MemSpan* lspan;
    void* mem;
    total = (total + (MEMOP_SPAN_SIZE - 1u)) & ~((size_t)MEMOP_SPAN_SIZE - 1u);
    units = total / MEMOP_SPAN_SIZE;
    /* 1 unidade (<= ~64KB): reusa o cache de chunks; multi-unidade: SO direto. */
    mem = (units == 1) ? chunk_acquire(heap) : span_os_alloc(total);
    if (!mem) return NULL;
    lspan = (MemSpan*)mem;
    lspan->owner = NULL;
    lspan->class_id = MEMOP_CLASS_LARGE;
    lspan->stride = (uint32)(total - MEMOP_SPAN_HEADER);
    lspan->block_count = (uint32)units;
    lspan->used = 1;
    lspan->bump_idx = 0;   /* large nao usa bump; higiene (chunk reciclado) */
    MEMOP_STAT(heap->s_alloc++);
    MEMOP_STAT(heap->s_span_allocs++);
    return (char*)mem + MEMOP_SPAN_HEADER;
}

/* Fast path de ponteiro CRU: devolve void* em registrador (sem sret do struct
 * MemBuffer). E a entrada quente real; memop_alloc envolve isto. */
void* memop_alloc_raw(uint64 size)
{
    MemHeap*  heap;
    MemSpan*  span;
    MemFreeBlock* block;
    int class_id;

    if (size == 0) return NULL;

    /* Fast path do heap por-thread: UMA leitura de TLS (igual mimalloc/rpmalloc).
     * So cai no get_heap (lento, com lock) na 1a alloc da thread. */
    heap = g_heap;
    if (!heap)
    {
        heap = get_heap();
        if (!heap) return NULL;
    }

    class_id = memop_class_index(size);
    if (class_id < 0) return memop_alloc_large(heap, size);   /* >16KB: caminho frio */

    span = heap->spans[class_id];
    if (span && (block = span->local_free) != NULL)
    {
        /* caminho mais quente: reuso de bloco liberado (LIFO) */
        span->local_free = block->next;
        span->used++;
    }
    else if (span && span->bump_idx < span->block_count)
    {
        /* crescimento: entrega o proximo bloco do span por bump (sem carve) */
        block = (MemFreeBlock*)((char*)span + MEMOP_SPAN_HEADER
                                + (size_t)span->bump_idx * g_class_stride[class_id]);
        span->bump_idx++;
        span->used++;
    }
    else
    {
        /* span cheio/inexistente: drena remoto, outro span, ou cria novo */
        block = memop_refill(heap, (uint32)class_id);
        if (!block) return NULL;
    }

    MEMOP_STAT(heap->s_alloc++);
    return block;
}

MemBuffer memop_alloc(uint64 size)
{
    MemBuffer out = { 0, 0 };
    void* p = memop_alloc_raw(size);
    if (p)
    {
        MemSpan* s = span_of(p);
        out.Ptr  = p;
        out.Size = (s->class_id == MEMOP_CLASS_LARGE) ? s->stride
                                                      : g_class_stride[s->class_id];
    }
    return out;
}

/* Fast path de free por ponteiro CRU (sem MemBuffer). memop_free envolve isto. */
void memop_free_raw(void* ptr)
{
    MemSpan* span;
    MemFreeBlock* block;
    MemHeap* owner;

    if (!ptr) return;

    span = span_of(ptr);

    if (span->class_id == MEMOP_CLASS_LARGE)
    {
        MEMOP_STAT(if (g_heap) g_heap->s_free++);
        if (span->block_count == 1) chunk_release(g_heap, span); /* 64KB -> cache */
        else                        span_os_free(span);          /* multi-unidade: SO */
        return;
    }

    owner = span->owner;
    block = (MemFreeBlock*)ptr;

    if (owner == g_heap)
    {
        /* free local: zero atomico */
        block->next = span->local_free;
        span->local_free = block;
        span->used--;
        MEMOP_STAT(g_heap->s_free++);

        /* span vazio: devolve ao cache global (reclamacao), mantendo ao menos
         * um span residente por classe para nao ficar realocando na fronteira. */
        if (span->used == 0 && (span->prev || span->next))
        {
            heap_list_unlink(g_heap, span->class_id, span);
            chunk_release(g_heap, span);
        }
    }
    else
    {
        /* free remoto: push atomico na lista do span dono (Treiber).
         * NAO toca 'used' (o dono ajusta ao drenar) -> dono fica nao-atomico. */
        void* head = atomic_get_ptr(&span->remote_free);
        do {
            block->next = (MemFreeBlock*)head;
        } while (!atomic_cas_ptr(&span->remote_free, &head, block));

        MEMOP_STAT(if (g_heap) { g_heap->s_free++; g_heap->s_remote_frees++; });
    }
}

void memop_free(MemBuffer* buffer)
{
    if (!buffer || !buffer->Ptr) return;
    memop_free_raw(buffer->Ptr);
    buffer->Ptr = NULL;
    buffer->Size = 0;
}




/* ----------------------------------------------------------------------- */
/*  Consultas / ciclo de vida                                              */
/* ----------------------------------------------------------------------- */

void memop_shutdown(void)
{
    MemHeap* h;
    int expected = MEMOP_READY;

    if (atomic_get(&g_init_state) != MEMOP_READY) return;
    if (!atomic_cas(&g_init_state, &expected, MEMOP_BUSY)) return;

    memop_lock();
    h = G.heaps;
    G.heaps = NULL;
    memop_unlock();

    while (h) { MemHeap* nx = h->hnext; heap_destroy(h); h = nx; }

    segments_free_all();
    thread_mutex_destroy(&G.cache_lock);
    thread_mutex_destroy(&G.lock);

    memset(&G, 0, sizeof(G));
    /* Zera a TLS da thread chamadora: a proxima alloc dela pega um heap novo.
     * (Sem epoch global: shutdown deve ser chamado quando nenhuma OUTRA thread
     *  esta alocando -- caso contrario seria uso incorreto.) */
    g_heap = NULL;

    atomic_set(&g_init_state, MEMOP_UNINIT);
}

void memop_get_stats(MemPoolStats* out_stats)
{
    MemHeap* h;
    if (!out_stats) return;
    memset(out_stats, 0, sizeof(*out_stats));

    memop_init();
    memop_lock();
    out_stats->lanes_created   = G.heaps_created;
    out_stats->lanes_destroyed = G.heaps_destroyed;
    for (h = G.heaps; h; h = h->hnext)
    {
        out_stats->alloc_count   += h->s_alloc;
        out_stats->free_count    += h->s_free;
        out_stats->sync_refills  += h->s_span_allocs;
        out_stats->remote_frees  += h->s_remote_frees;
    }
    /* async_* nao existem mais no v2 (mantidos 0 por compatibilidade de ABI) */
    memop_unlock();
}

void memop_test_reset(void)
{
    memop_shutdown();
    memop_init();
}

void memop_on_created_thread(const Thread* thr)
{
    (void)thr;
    g_heap = get_heap();
}

void memop_on_ended_thread(const Thread* thr)
{
    MemHeap* h = g_heap;
    uint32 c;
    (void)thr;
    if (!h) return;

    /* Recicla spans vazios da thread que termina para o cache global (reuso por
     * outras threads, sem ir ao SO). Spans com blocos ainda vivos (em uso por
     * outras threads) permanecem no heap e sao liberados no shutdown.
     * used==0 garante que nenhum bloco do span esta vivo -> seguro reciclar. */
    for (c = 0; c < MEMOP_CLASS_COUNT; c++)
    {
        MemSpan* span = h->spans[c];
        MemSpan* kept = NULL;
        h->spans[c] = NULL;
        while (span)
        {
            MemSpan* next = span->next;
            span_drain_remote(span);
            if (span->used == 0)
            {
                chunk_release(NULL, span);   /* -> global, p/ outras threads */
            }
            else
            {
                span->prev = NULL;
                span->next = kept;
                if (kept) kept->prev = span;
                kept = span;
            }
            span = next;
        }
        h->spans[c] = kept;
    }
    chunk_local_to_global(h);   /* cache local da thread morta -> global */

    h->active = 0;
    memop_lock();
    G.heaps_destroyed++;
    memop_unlock();

    g_heap = NULL;
}
