/*
 * Benchmark v2: Medi��o mais precisa com batches
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

#define ITERATIONS      10000000
#define BLOCK_SIZE      256
#define BATCH_SIZE      1000

static uint64_t get_time_ns(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq = { 0 };
    LARGE_INTEGER counter;
    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }
    QueryPerformanceCounter(&counter);
    return (uint64_t)(counter.QuadPart * 1000000000ULL / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
#endif
}

/* Freelist */
typedef struct freelist_node { struct freelist_node* next; } freelist_node_t;
typedef struct {
    void* memory;
    freelist_node_t* free_list;
    size_t block_size;
} freelist_pool_t;

static void freelist_init(freelist_pool_t* pool, size_t block_size, size_t num_blocks) {
    pool->memory = malloc(block_size * num_blocks);
    pool->block_size = block_size;
    pool->free_list = NULL;
    uint8_t* ptr = (uint8_t*)pool->memory;
    for (size_t i = 0; i < num_blocks; i++) {
        freelist_node_t* node = (freelist_node_t*)(ptr + i * block_size);
        node->next = pool->free_list;
        pool->free_list = node;
    }
}

static void* freelist_alloc(freelist_pool_t* pool) {
    freelist_node_t* node = pool->free_list;
    pool->free_list = node->next;
    return node;
}

static void freelist_free(freelist_pool_t* pool, void* ptr) {
    freelist_node_t* node = (freelist_node_t*)ptr;
    node->next = pool->free_list;
    pool->free_list = node;
}

/* Arena */
typedef struct {
    void* memory;
    size_t capacity;
    size_t offset;
} arena_t;

static void arena_init(arena_t* arena, size_t capacity) {
    arena->memory = malloc(capacity);
    arena->capacity = capacity;
    arena->offset = 0;
}

static void* arena_alloc(arena_t* arena, size_t size) {
    size_t aligned = (size + 7) & ~7;
    void* ptr = (uint8_t*)arena->memory + arena->offset;
    arena->offset += aligned;
    return ptr;
}

static void arena_reset(arena_t* arena) { arena->offset = 0; }

/* Ring Buffer */
typedef struct {
    void* memory;
    size_t block_size;
    size_t num_blocks;
    size_t head;
    size_t tail;
} ring_pool_t;

static void ring_init(ring_pool_t* ring, size_t block_size, size_t num_blocks) {
    ring->memory = malloc(block_size * num_blocks);
    ring->block_size = block_size;
    ring->num_blocks = num_blocks;
    ring->head = ring->tail = 0;
}

static void* ring_alloc(ring_pool_t* ring) {
    void* ptr = (uint8_t*)ring->memory + (ring->head * ring->block_size);
    ring->head = (ring->head + 1) % ring->num_blocks;
    return ptr;
}

static void ring_free(ring_pool_t* ring) {
    ring->tail = (ring->tail + 1) % ring->num_blocks;
}

/* Slab (simplificado: 4 tamanhos) */
typedef struct {
    freelist_pool_t slabs[4];
    size_t sizes[4];
} slab_t;

static void slab_init(slab_t* s, size_t num_per_slab) {
    s->sizes[0] = 64; s->sizes[1] = 256; s->sizes[2] = 1024; s->sizes[3] = 4096;
    for (int i = 0; i < 4; i++) freelist_init(&s->slabs[i], s->sizes[i], num_per_slab);
}

static void* slab_alloc(slab_t* s, size_t size) {
    for (int i = 0; i < 4; i++) if (size <= s->sizes[i]) return freelist_alloc(&s->slabs[i]);
    return NULL;
}

static void slab_free(slab_t* s, void* ptr, size_t size) {
    for (int i = 0; i < 4; i++) if (size <= s->sizes[i]) { freelist_free(&s->slabs[i], ptr); return; }
}


static void malloc_test()
{
    void* ptrs[BATCH_SIZE];
    uint64_t start, end;
    double malloc_ns, freelist_ns, arena_ns, ring_ns, slab_ns;
    int batches = ITERATIONS / BATCH_SIZE;

    printf("\n");
    printf("================================================================================\n");
    printf("  BENCHMARK: Pool de Mem�ria vs malloc\n");
    printf("  %d itera��es, bloco de %d bytes\n", ITERATIONS, BLOCK_SIZE);
    printf("================================================================================\n\n");

    /* 1. malloc/free */
    start = get_time_ns();
    for (int b = 0; b < batches; b++) {
        for (int i = 0; i < BATCH_SIZE; i++) ptrs[i] = malloc(BLOCK_SIZE);
        for (int i = 0; i < BATCH_SIZE; i++) free(ptrs[i]);
    }
    end = get_time_ns();
    malloc_ns = (double)(end - start) / ITERATIONS;

    /* 2. Freelist */
    freelist_pool_t fl;
    freelist_init(&fl, BLOCK_SIZE, BATCH_SIZE + 100);
    start = get_time_ns();
    for (int b = 0; b < batches; b++) {
        for (int i = 0; i < BATCH_SIZE; i++) ptrs[i] = freelist_alloc(&fl);
        for (int i = 0; i < BATCH_SIZE; i++) freelist_free(&fl, ptrs[i]);
    }
    end = get_time_ns();
    freelist_ns = (double)(end - start) / ITERATIONS;

    /* 3. Arena */
    arena_t ar;
    arena_init(&ar, (size_t)BATCH_SIZE * 264 + 1024);
    start = get_time_ns();
    for (int b = 0; b < batches; b++) {
        for (int i = 0; i < BATCH_SIZE; i++) ptrs[i] = arena_alloc(&ar, BLOCK_SIZE);
        arena_reset(&ar);
    }
    end = get_time_ns();
    arena_ns = (double)(end - start) / ITERATIONS;

    /* 4. Ring Buffer */
    ring_pool_t rg;
    ring_init(&rg, BLOCK_SIZE, BATCH_SIZE + 100);
    start = get_time_ns();
    for (int b = 0; b < batches; b++) {
        for (int i = 0; i < BATCH_SIZE; i++) ptrs[i] = ring_alloc(&rg);
        for (int i = 0; i < BATCH_SIZE; i++) ring_free(&rg);
    }
    end = get_time_ns();
    ring_ns = (double)(end - start) / ITERATIONS;

    /* 5. Slab (tamanho fixo 256 pra comparar igual) */
    slab_t sl;
    slab_init(&sl, BATCH_SIZE + 100);
    start = get_time_ns();
    for (int b = 0; b < batches; b++) {
        for (int i = 0; i < BATCH_SIZE; i++) ptrs[i] = slab_alloc(&sl, BLOCK_SIZE);
        for (int i = 0; i < BATCH_SIZE; i++) slab_free(&sl, ptrs[i], BLOCK_SIZE);
    }
    end = get_time_ns();
    slab_ns = (double)(end - start) / ITERATIONS;

    /* Resultados */
    printf("  %-20s %12s %15s %12s\n", "Tipo", "ns/op", "Ops/segundo", "Speedup");
    printf("  %-20s %12s %15s %12s\n", "--------------------", "------------", "---------------", "------------");
    printf("  %-20s %12.2f %15.0f %12s\n", "malloc/free", malloc_ns, 1e9 / malloc_ns, "1.00x");
    printf("  %-20s %12.2f %15.0f %11.1fx\n", "Freelist", freelist_ns, 1e9 / freelist_ns, malloc_ns / freelist_ns);
    printf("  %-20s %12.2f %15.0f %11.1fx\n", "Arena", arena_ns, 1e9 / arena_ns, malloc_ns / arena_ns);
    printf("  %-20s %12.2f %15.0f %11.1fx\n", "Ring Buffer", ring_ns, 1e9 / ring_ns, malloc_ns / ring_ns);
    printf("  %-20s %12.2f %15.0f %11.1fx\n", "Slab", slab_ns, 1e9 / slab_ns, malloc_ns / slab_ns);

    /* Benchmark com tamanhos variados */
    printf("\n");
    printf("================================================================================\n");
    printf("  BENCHMARK: Tamanhos Variados (64, 128, 256, 512, 1024 bytes)\n");
    printf("================================================================================\n\n");

    size_t sizes[] = { 64, 128, 256, 512, 1024 };
    int varied_batch = 200; /* 5 tamanhos x 200 = 1000 ops por batch */
    int varied_batches = ITERATIONS / (varied_batch * 5);

    /* malloc variado */
    start = get_time_ns();
    for (int b = 0; b < varied_batches; b++) {
        for (int s = 0; s < 5; s++) {
            for (int i = 0; i < varied_batch; i++) ptrs[i] = malloc(sizes[s]);
            for (int i = 0; i < varied_batch; i++) free(ptrs[i]);
        }
    }
    end = get_time_ns();
    double malloc_varied = (double)(end - start) / (varied_batches * varied_batch * 5);

    /* Slab variado */
    slab_t sl2;
    slab_init(&sl2, varied_batch + 100);
    start = get_time_ns();
    for (int b = 0; b < varied_batches; b++) {
        for (int s = 0; s < 5; s++) {
            for (int i = 0; i < varied_batch; i++) ptrs[i] = slab_alloc(&sl2, sizes[s]);
            for (int i = 0; i < varied_batch; i++) slab_free(&sl2, ptrs[i], sizes[s]);
        }
    }
    end = get_time_ns();
    double slab_varied = (double)(end - start) / (varied_batches * varied_batch * 5);

    printf("  %-20s %12s %15s %12s\n", "Tipo", "ns/op", "Ops/segundo", "Speedup");
    printf("  %-20s %12s %15s %12s\n", "--------------------", "------------", "---------------", "------------");
    printf("  %-20s %12.2f %15.0f %12s\n", "malloc/free", malloc_varied, 1e9 / malloc_varied, "1.00x");
    printf("  %-20s %12.2f %15.0f %11.1fx\n", "Slab", slab_varied, 1e9 / slab_varied, malloc_varied / slab_varied);

    printf("\n");
    printf("================================================================================\n");
    printf("  RESUMO\n");
    printf("================================================================================\n\n");
    printf("  Pool           | Speedup | Uso ideal\n");
    printf("  ---------------|---------|------------------------------------------\n");
    printf("  Arena          | ~%.0fx   | Request HTTP (aloca muito, libera tudo)\n", malloc_ns / arena_ns);
    printf("  Ring Buffer    | ~%.0fx   | Frames de v�deo (FIFO)\n", malloc_ns / ring_ns);
    printf("  Freelist       | ~%.0fx    | Conex�es, objetos de tamanho fixo\n", malloc_ns / freelist_ns);
    printf("  Slab           | ~%.0fx   | Buffers I/O de tamanhos variados\n", malloc_ns / slab_ns);
    printf("\n");
}



extern void test_task_handler_run(void);

int main(void)
{
    //malloc_test();

    test_task_handler_run();






    return 0;
}