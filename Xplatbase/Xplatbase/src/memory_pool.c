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
#define MEMOP_TLS __declspec(thread)
#else
#include <time.h>
#include <sys/mman.h>
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

/* Estado do span (so o dono le/escreve; nada de atomico):
 *   AVAILABLE -> na lista da classe (heap->spans[c]): tem/pode ter blocos.
 *   FULL      -> na lista full do heap: esgotado; NUNCA varrido pelo alloc.
 * A transicao de volta e O(1): free local religa na classe (modelo rp/mimalloc). */
#define MEMOP_SPAN_AVAILABLE 0u
#define MEMOP_SPAN_FULL      1u

/* Cabecalho no inicio de cada span 64KB-alinhado. */
typedef struct MemSpan
{
    MemHeap*       owner;
    struct MemSpan* next;        /* lista dupla: classe (AVAILABLE) ou full (FULL) */
    struct MemSpan* prev;
    MemFreeBlock*  local_free;   /* so o dono toca (sem atomico)  */
    xatomic_ptr    remote_free;  /* frees cross-thread (Treiber)  */
    uint32 class_id;
    uint32 stride;               /* tamanho usavel por bloco       */
    uint32 block_count;          /* nro de blocos no span (large: unidades 64KB) */
    uint32 used;                 /* so o dono ajusta (nao-atomico) */
    uint32 bump_idx;             /* carve PREGUICOSO: proximo bloco nunca entregue */
    uint32 state;                /* MEMOP_SPAN_AVAILABLE | MEMOP_SPAN_FULL */
} MemSpan;
/* sizeof(MemSpan) deve caber em MEMOP_SPAN_HEADER (64B): 5 ptr + 6 uint32 = 64 */
typedef char memop_span_header_fits[(sizeof(MemSpan) <= 64) ? 1 : -1];

struct MemHeap
{
    MemSpan* spans[MEMOP_CLASS_COUNT];  /* AVAILABLE por classe (so spans uteis) */
    /* Spans FULL (esgotados), fora do caminho do alloc. O refill checa ate K
     * deles (round-robin) antes de criar span novo, p/ reaproveitar frees
     * remotos que chegaram a spans cheios. */
    MemSpan* full_head;
    MemSpan* full_tail;
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
#define MEMOP_SEG_MASK (~((uintptr_t)MEMOP_SEG_SIZE - 1u))
/* chunk -> segmento por mascara (segmento e 4MB-alinhado, header no offset 0). */
#define MEMOP_SEG_OF(p) ((MemSegment*)((uintptr_t)(p) & MEMOP_SEG_MASK))

/* ---- Purga com atraso (devolve segmentos ociosos ao SO) ----------------
 * Liga em 3 mecanismos para nao cair em "vale falso" (liberar memoria que
 * sera reusada logo):
 *   1) ATRASO: um segmento so e elegivel apos ficar ocioso > DELAY_MS.
 *   2) AMORTIZACAO: o tamanho do cache entra numa media exponencial (EWMA)
 *      antes de decidir -> picos transitorios nao disparam purga.
 *   3) HISTERESE: comeca a purgar acima de HIGH e so para abaixo de LOW
 *      -> evita thrash na fronteira de um unico limiar. */
#ifndef MEMOP_PURGE_ENABLE
#define MEMOP_PURGE_ENABLE 1
#endif
#ifndef MEMOP_PURGE_DELAY_MS
#define MEMOP_PURGE_DELAY_MS 500u   /* ocioso > isto para virar elegivel */
#endif
#ifndef MEMOP_PURGE_PERIOD_MS
#define MEMOP_PURGE_PERIOD_MS 100u  /* intervalo minimo entre avaliacoes */
#endif
#ifndef MEMOP_PURGE_EWMA_SHIFT
#define MEMOP_PURGE_EWMA_SHIFT 2     /* alpha = 1/4 (suavizacao) */
#endif
#define MEMOP_MB_CHUNKS (uint32)((1024u*1024u)/MEMOP_SPAN_SIZE)  /* 16 chunks/MB */
#ifndef MEMOP_PURGE_HIGH_MB
#define MEMOP_PURGE_HIGH_MB 64u      /* cache suavizado acima disto -> purga */
#endif
#ifndef MEMOP_PURGE_LOW_MB
#define MEMOP_PURGE_LOW_MB 32u       /* histerese: purga ate cair a isto */
#endif
#define MEMOP_PURGE_HIGH ((int)(MEMOP_PURGE_HIGH_MB * MEMOP_MB_CHUNKS))
#define MEMOP_PURGE_LOW  ((int)(MEMOP_PURGE_LOW_MB  * MEMOP_MB_CHUNKS))

/* Um segmento = uma alocacao de 4MB do SO, alinhada a 4MB, fatiada em
 * MEMOP_SEG_SPANS chunks de 64KB. O HEADER mora no chunk 0 (offset 0), entao
 * 'chunk -> segmento' e uma mascara O(1). Chunks 1..63 sao usaveis (63).
 * Em runtime a memoria fica cacheada para reuso; segmentos OCIOSOS ha muito
 * tempo sao devolvidos ao SO pela purga (alem do shutdown). */
typedef struct MemSegment
{
    struct MemSegment* next;
    uint32 next_chunk;         /* bump: proximo chunk (1..63) nunca entregue */
    uint32 live;               /* chunks doled atualmente FORA do cache global */
    uint64 idle_since;         /* ms quando 'live' zerou (0 = em uso) */
    uint32 doomed;             /* marca temporaria durante a purga */
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
    MemSegment* segments;     /* todos os segmentos (libera no shutdown/purga) */

    /* estado da purga (sob cache_lock) */
    int    cache_avg;         /* EWMA de cache_count (amortizacao) */
    int    purging;           /* modo histerese: 1 = purgando ate LOW */
    uint64 last_purge_ms;     /* ultima avaliacao (throttle por PERIOD) */
    uint64 purge_count;       /* segmentos devolvidos ao SO (stat) */
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

/* Paginas DIRETO do SO (VirtualAlloc/mmap), sem o CRT heap no caminho:
 *   - menos overhead por chamada (sem bookkeeping/headers do heap do CRT);
 *   - _aligned_malloc(4MB,4MB) over-aloca p/ alinhar e nao apara as pontas
 *     (~ate 4MB de commit charge desperdicado POR SEGMENTO); aqui nao.
 * O commit e charge apenas: as paginas so viram RAM no primeiro toque. */

static void* span_os_alloc(size_t size)
{
#ifdef XPLATBASE_WIN
    /* VirtualAlloc ja retorna alinhado a 64KB (granularidade do Windows) */
    return VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    /* mmap alinha so a 4KB: over-mapeia e APARA as pontas (munmap parcial ok) */
    size_t over = size + MEMOP_SPAN_SIZE;
    char*  raw  = (char*)mmap(NULL, over, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    char*  base;
    size_t pre, post;
    if (raw == MAP_FAILED) return NULL;
    base = (char*)(((uintptr_t)raw + (MEMOP_SPAN_SIZE - 1u)) & ~(uintptr_t)(MEMOP_SPAN_SIZE - 1u));
    pre  = (size_t)(base - raw);
    post = over - pre - size;
    if (pre)  munmap(raw, pre);
    if (post) munmap(base + size, post);
    return base;
#endif
}

static void span_os_free(void* p, size_t size)
{
#ifdef XPLATBASE_WIN
    (void)size;
    VirtualFree(p, 0, MEM_RELEASE);
#else
    munmap(p, size);
#endif
}

/* Segmento de 4MB ALINHADO a 4MB (para 'chunk -> segmento' por mascara). */
static void* seg_os_alloc(void)
{
#ifdef XPLATBASE_WIN
    int tries;
    /* tentativa direta: frequentemente o SO ja devolve alinhado */
    void* p = VirtualAlloc(NULL, MEMOP_SEG_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (p && (((uintptr_t)p & (MEMOP_SEG_SIZE - 1u)) == 0)) return p;
    if (p) VirtualFree(p, 0, MEM_RELEASE);
    /* over-RESERVA (sem commit, sem custo) p/ descobrir endereco alinhado e
     * realoca fixo nele; corrida com outra thread -> tenta de novo */
    for (tries = 0; tries < 8; tries++)
    {
        void* r = VirtualAlloc(NULL, MEMOP_SEG_SIZE * 2, MEM_RESERVE, PAGE_NOACCESS);
        uintptr_t aligned;
        if (!r) return NULL;
        aligned = ((uintptr_t)r + (MEMOP_SEG_SIZE - 1u)) & ~(uintptr_t)(MEMOP_SEG_SIZE - 1u);
        VirtualFree(r, 0, MEM_RELEASE);
        p = VirtualAlloc((void*)aligned, MEMOP_SEG_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (p) return p;
    }
    return NULL;
#else
    size_t over = MEMOP_SEG_SIZE * 2;
    char*  raw  = (char*)mmap(NULL, over, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    char*  base;
    size_t pre, post;
    if (raw == MAP_FAILED) return NULL;
    base = (char*)(((uintptr_t)raw + (MEMOP_SEG_SIZE - 1u)) & ~(uintptr_t)(MEMOP_SEG_SIZE - 1u));
    pre  = (size_t)(base - raw);
    post = over - pre - MEMOP_SEG_SIZE;
    if (pre)  munmap(raw, pre);
    if (post) munmap(base + MEMOP_SEG_SIZE, post);
    return base;
#endif
}

/* Relogio monotonico em milissegundos (resolucao grossa basta p/ a purga). */
static uint64 memop_now_ms(void)
{
#ifdef XPLATBASE_WIN
    return (uint64)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64)ts.tv_sec * 1000u + (uint64)(ts.tv_nsec / 1000000);
#endif
}

/* Cache de chunks de 64KB em dois niveis:
 *   - por-thread (heap->local_cache): SEM lock, e o caminho normal (hot).
 *   - global (G.span_cache): so quando o local esgota/transborda; balanceia
 *     entre threads.
 * Quando ambos esgotam, dola-se um chunk por BUMP de um segmento (amortiza
 * mmap/commit em ~63 chunks, sem tocar paginas a frente). 'seg->live' conta os
 * chunks doled que NAO estao no cache global (em uso ou em cache local); quando
 * zera, o segmento fica candidato a purga (devolucao ao SO). */
static void* chunk_acquire(MemHeap* heap)
{
    void* mem;
    MemSegment* seg;

    if (heap && heap->local_cache)   /* hot: cache local, sem lock e sem mudar 'live' */
    {
        mem = heap->local_cache;
        heap->local_cache = *(void**)mem;
        heap->local_count--;
        return mem;
    }

    thread_mutex_lock(&G.cache_lock);
    /* 1) chunk liberado (reuso) -> sai do cache global, entra em uso */
    if (G.span_cache)
    {
        mem = G.span_cache;
        G.span_cache = *(void**)mem;
        G.cache_count--;
        MEMOP_SEG_OF(mem)->live++;
        thread_mutex_unlock(&G.cache_lock);
        return mem;
    }
    /* 2) bump do segmento corrente: dola o proximo chunk SEM toca-lo */
    if (G.segments && G.segments->next_chunk < MEMOP_SEG_SPANS)
    {
        seg = G.segments;
        mem = (char*)seg + (size_t)seg->next_chunk * MEMOP_SPAN_SIZE;
        seg->next_chunk++;
        seg->live++;
        thread_mutex_unlock(&G.cache_lock);
        return mem;
    }
    thread_mutex_unlock(&G.cache_lock);

    /* 3) segmento novo (4MB do SO, alinhado). HEADER no chunk 0; entrega o chunk 1
     *    ao chamador; chunks 2..63 ficam para bump. */
    seg = (MemSegment*)seg_os_alloc();
    if (!seg) return NULL;
    seg->next_chunk = 2;
    seg->live = 1;
    seg->idle_since = 0;
    seg->doomed = 0;
    mem = (char*)seg + MEMOP_SPAN_SIZE;   /* chunk 1 */

    thread_mutex_lock(&G.cache_lock);
    seg->next = G.segments;
    G.segments = seg;
    thread_mutex_unlock(&G.cache_lock);
    return mem;
}

/* Condena segmentos ociosos (live==0) e devolve os 4MB ao SO. Sob cache_lock.
 *   force=1  -> trim explicito: ignora atraso e alvo (libera tudo ocioso agora).
 *   force=0  -> automatico: respeita o ATRASO por segmento e para ao atingir 'target'. */
static void purge_apply_locked(uint64 now, int force, int target)
{
    MemSegment** pp;
    MemSegment*  doomed = NULL;
    void**       link;

    /* 1a passada: condena (desliga da lista de segmentos) */
    pp = &G.segments;
    while (*pp && (force || target > 0))
    {
        MemSegment* s = *pp;
        if (s->live == 0 && (force || (now - s->idle_since) >= MEMOP_PURGE_DELAY_MS))
        {
            if (!force) target -= (int)(s->next_chunk - 1);  /* chunks doled (no cache) */
            s->doomed = 1;
            *pp = s->next;
            s->next = doomed;
            doomed = s;
        }
        else pp = &s->next;
    }
    if (!doomed) return;

    /* 2a passada: extrai do cache global os chunks dos segmentos condenados */
    link = &G.span_cache;
    while (*link)
    {
        void* c = *link;
        if (MEMOP_SEG_OF(c)->doomed) { *link = *(void**)c; G.cache_count--; }
        else                          link = (void**)c;
    }

    /* devolve os 4MB ao SO (seg == base do segmento) */
    while (doomed)
    {
        MemSegment* nx = doomed->next;
        span_os_free(doomed, MEMOP_SEG_SIZE);
        G.purge_count++;
        doomed = nx;
    }
}

/* Avaliacao AUTOMATICA (piggyback no chunk_release). Throttle por PERIOD;
 * amortizacao por EWMA; histerese HIGH/LOW; atraso por segmento. Sob cache_lock. */
static void memop_purge_locked(uint64 now)
{
#if MEMOP_PURGE_ENABLE
    int target;
    if (now - G.last_purge_ms < MEMOP_PURGE_PERIOD_MS) return;
    G.last_purge_ms = now;

    /* amortizacao: media exponencial do tamanho do cache (nao reage a picos) */
    G.cache_avg += (G.cache_count - G.cache_avg) / (1 << MEMOP_PURGE_EWMA_SHIFT);

    /* histerese: liga acima de HIGH, desliga abaixo de LOW */
    if (!G.purging) { if (G.cache_avg > MEMOP_PURGE_HIGH) G.purging = 1; else return; }
    else            { if (G.cache_avg < MEMOP_PURGE_LOW)  { G.purging = 0; return; } }

    target = G.cache_count - MEMOP_PURGE_LOW;   /* devolve ate cair a LOW */
    if (target > 0) purge_apply_locked(now, 0, target);
#else
    (void)now;
#endif
}

static void chunk_release(MemHeap* heap, void* mem)
{
    MemSegment* seg;

    if (heap && heap->local_count < MEMOP_HEAP_CACHE_MAX)   /* hot: local, sem lock */
    {
        *(void**)mem = heap->local_cache;
        heap->local_cache = mem;
        heap->local_count++;
        return;
    }
    /* Excedente -> cache global; o chunk volta a estar disponivel. */
    thread_mutex_lock(&G.cache_lock);
    *(void**)mem = G.span_cache;
    G.span_cache = mem;
    G.cache_count++;
    seg = MEMOP_SEG_OF(mem);
    if (seg->live > 0) seg->live--;
    if (seg->live == 0)
    {
        uint64 now = memop_now_ms();
        seg->idle_since = now;        /* inicia o relogio de ociosidade */
        memop_purge_locked(now);
    }
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
    while (s) { MemSegment* nx = s->next; span_os_free(s, MEMOP_SEG_SIZE); s = nx; }  /* seg == base */
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
    span->state = MEMOP_SPAN_AVAILABLE;
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
    span->used -= n;   /* contrato: o dono ajusta 'used' ao drenar (nao-atomico) */
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

/* Lista FULL do heap (dupla, com cauda p/ round-robin). So o dono toca. */
static void heap_full_push_tail(MemHeap* heap, MemSpan* span)
{
    span->next = NULL;
    span->prev = heap->full_tail;
    if (heap->full_tail) heap->full_tail->next = span;
    else                 heap->full_head = span;
    heap->full_tail = span;
    span->state = MEMOP_SPAN_FULL;
}

static void heap_full_unlink(MemHeap* heap, MemSpan* span)
{
    if (span->prev) span->prev->next = span->next;
    else            heap->full_head = span->next;
    if (span->next) span->next->prev = span->prev;
    else            heap->full_tail = span->prev;
    span->prev = span->next = NULL;
    span->state = MEMOP_SPAN_AVAILABLE;
}

/* Quantos spans FULL checar (drain remoto) antes de criar span novo. */
#ifndef MEMOP_FULL_SCAN_K
#define MEMOP_FULL_SCAN_K 2
#endif

/* Entrega um bloco da classe quando a cabeca nao tem local_free/bump imediato.
 * A lista da classe contem SO spans com blocos potenciais: spans esgotados sao
 * movidos para a lista full do heap (fora do caminho do alloc) e voltam O(1)
 * quando um free os reabastece. Antes de criar span novo, checa ate K spans
 * full (round-robin) p/ reaproveitar frees remotos pendentes. */
static MemFreeBlock* memop_refill(MemHeap* heap, uint32 class_id)
{
    uint32   stride = g_class_stride[class_id];
    MemSpan* span = heap->spans[class_id];
    MemFreeBlock* b;
    MemSpan* start;
    int k;

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
        /* esgotado: sai da classe -> lista full (nao sera mais varrido) */
        {
            MemSpan* nx = span->next;
            heap_list_unlink(heap, class_id, span);
            heap_full_push_tail(heap, span);
            span = nx;
        }
    }

    /* classe sem span util: tenta reaproveitar ate K spans full (frees remotos
     * pendentes) antes de pedir memoria nova; rotaciona p/ cobrir todos ao longo
     * do tempo (round-robin amortizado O(1)). */
    start = NULL;
    for (k = 0; k < MEMOP_FULL_SCAN_K; k++)
    {
        MemSpan* fs = heap->full_head;
        if (!fs || fs == start) break;
        if (!start) start = fs;
        if (span_drain_remote(fs) > 0)
        {
            heap_full_unlink(heap, fs);
            heap_list_insert_head(heap, fs->class_id, fs);
            if (fs->class_id == class_id)   /* serve a esta alloc? */
            {
                span = fs;
                b = span->local_free;
                span->local_free = b->next;
                goto got;
            }
            continue;   /* reativou outra classe (conta no limite K) */
        }
        if (heap->full_tail != fs)          /* rotaciona: proximo refill checa outro */
        {
            heap_full_unlink(heap, fs);
            heap_full_push_tail(heap, fs);
        }
    }

    /* cria span novo (carve preguicoso, O(1)) e entrega o 1o bloco via bump */
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
    lspan->state = MEMOP_SPAN_AVAILABLE;   /* nunca lido p/ large; higiene */
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
        else span_os_free(span, (size_t)span->block_count * MEMOP_SPAN_SIZE); /* multi-unidade: SO */
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

        /* span estava FULL: religa na cabeca da classe, O(1) (rp/mimalloc).
         * E isto que mantem a lista da classe so com spans uteis. */
        if (span->state == MEMOP_SPAN_FULL)
        {
            heap_full_unlink(g_heap, span);                       /* -> AVAILABLE */
            heap_list_insert_head(g_heap, span->class_id, span);
        }
        /* span vazio: devolve ao cache global (reclamacao), mantendo ao menos
         * um span residente por classe para nao ficar realocando na fronteira. */
        else if (span->used == 0 && (span->prev || span->next))
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

/* Trim explicito (app-driven): devolve AGORA ao SO todos os segmentos ociosos,
 * sem esperar o atraso/histerese. Util em pontos de ociosidade conhecidos. */
void memop_purge(void)
{
    if (atomic_get(&g_init_state) != MEMOP_READY) return;
    thread_mutex_lock(&G.cache_lock);
    purge_apply_locked(memop_now_ms(), 1, 0);
    thread_mutex_unlock(&G.cache_lock);
}

void memop_get_stats(MemPoolStats* out_stats)
{
    MemHeap* h;
    MemSegment* s;
    uint64 segs = 0;
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

    thread_mutex_lock(&G.cache_lock);
    for (s = G.segments; s; s = s->next) segs++;
    out_stats->os_reserved_bytes = segs * (uint64)MEMOP_SEG_SIZE;
    out_stats->cached_chunks     = (uint64)G.cache_count;
    out_stats->purge_count       = G.purge_count;
    thread_mutex_unlock(&G.cache_lock);
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
    /* lista FULL: mesmo tratamento (drenar; vazios -> cache global; vivos ficam
     * no heap morto ate o shutdown, recebendo frees remotos normalmente). */
    {
        MemSpan* span = h->full_head;
        MemSpan* kept = NULL;
        MemSpan* kept_tail = NULL;
        h->full_head = h->full_tail = NULL;
        while (span)
        {
            MemSpan* next = span->next;
            span_drain_remote(span);
            if (span->used == 0)
            {
                chunk_release(NULL, span);
            }
            else
            {
                span->next = NULL;
                span->prev = kept_tail;
                if (kept_tail) kept_tail->next = span;
                else           kept = span;
                kept_tail = span;
            }
            span = next;
        }
        h->full_head = kept;
        h->full_tail = kept_tail;
    }
    chunk_local_to_global(h);   /* cache local da thread morta -> global */

    h->active = 0;
    memop_lock();
    G.heaps_destroyed++;
    memop_unlock();

    g_heap = NULL;
}
