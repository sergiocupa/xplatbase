/*
 * bench_linux.c - comparativo de alocadores (Linux / gcc)
 *
 * Um binario, dois modos:
 *   mode "sys"   -> chama malloc/free (alocador real definido via LD_PRELOAD:
 *                   glibc / jemalloc / tcmalloc / mimalloc / snmalloc)
 *   mode "memop" -> nossa pool embutida (>16KB usa fallback malloc)
 *
 * Mesmo workload do bench Windows: Larson churn (random small/med/large,
 * multi-thread) + small fixed 64B single-thread.
 *
 * uso: ./bench_linux <sys|memop> [threads] [ops_por_thread]
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>

#include "../../Xplatbase/Xplatbase/src/memory_pool.h"

/* No Linux o auto-init (__attribute__((constructor))) chama platform_init e NAO
 * e protegido por XPLATBASE_NO_AUTO_INIT (guard so existe no lado Windows).
 * Como inicializamos memop explicitamente, basta um stub para satisfazer o link. */
void platform_init(void) {}

/* ---- RNG xorshift64 ---- */
static uint64_t rng_next(uint64_t* s){ uint64_t x=*s; x^=x<<13; x^=x>>7; x^=x<<17; *s=x; return x; }
static size_t rand_size(uint64_t* s){
    uint32_t r=(uint32_t)(rng_next(s)%100u);
    if(r<75) return 8   + (size_t)(rng_next(s)%121u);
    if(r<95) return 129 + (size_t)(rng_next(s)%1920u);
    return        2049 + (size_t)(rng_next(s)%63488u);
}
static double now_s(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (double)t.tv_sec + (double)t.tv_nsec/1e9; }

/* ---- alocadores: sys (malloc, via LD_PRELOAD) ou memop (embutido) ---- */
static int g_use_memop = 0;

static inline void* A(size_t n){
    if(!g_use_memop) return malloc(n);
    { MemBuffer b = memop_alloc(n); return b.Ptr; }
}
static inline void F(void* p, size_t n){
    (void)n;
    if(!p) return;
    if(!g_use_memop){ free(p); return; }
    { MemBuffer b; b.Ptr=p; b.Size=0; memop_free(&b); }
}

/* ---- cenario A: Larson churn ---- */
typedef struct { int slots; long ops; uint64_t seed; double elapsed; } Work;

static void* larson_worker(void* arg){
    Work* w=(Work*)arg;
    uint64_t rng=w->seed;
    int i; long k; double t0,t1;
    void**  ptr=(void**)malloc((size_t)w->slots*sizeof(void*));
    size_t* sz =(size_t*)malloc((size_t)w->slots*sizeof(size_t));

    for(i=0;i<w->slots;i++){ size_t s=rand_size(&rng); ptr[i]=A(s); sz[i]=s; if(ptr[i]){((char*)ptr[i])[0]=1; ((char*)ptr[i])[s-1]=1;} }

    t0=now_s();
    for(k=0;k<w->ops;k++){
        int idx=(int)(rng_next(&rng)%(uint64_t)w->slots);
        F(ptr[idx], sz[idx]);
        { size_t s=rand_size(&rng); void* p=A(s); ptr[idx]=p; sz[idx]=s; if(p){((char*)p)[0]=(char)k; ((char*)p)[s-1]=(char)k;} }
    }
    t1=now_s();

    for(i=0;i<w->slots;i++) F(ptr[i], sz[i]);
    free(ptr); free(sz);
    w->elapsed=t1-t0;
    return NULL;
}

static void run_larson(const char* name, int threads, int slots, long ops){
    pthread_t* h=(pthread_t*)malloc((size_t)threads*sizeof(pthread_t));
    Work* w=(Work*)malloc((size_t)threads*sizeof(Work));
    int i; double maxel=0.0; double total=(double)ops*threads;
    for(i=0;i<threads;i++){
        w[i].slots=slots; w[i].ops=ops;
        w[i].seed=0x9E3779B97F4A7C15ull ^ ((uint64_t)(i+1)*0xD1B54A32D192ED03ull);
        w[i].elapsed=0.0;
        pthread_create(&h[i],NULL,larson_worker,&w[i]);
    }
    for(i=0;i<threads;i++){ pthread_join(h[i],NULL); if(w[i].elapsed>maxel) maxel=w[i].elapsed; }
    printf("  %-11s  %7.3f s   %8.2f Mops/s\n", name, maxel, (total/maxel)/1e6);
    free(h); free(w);
}

/* ---- cenario B: small fixed 64B ---- */
static void run_smallfixed(const char* name, long ops){
    enum { SL=256 }; void* ptr[SL]; int i; long k; double t0,t1;
    for(i=0;i<SL;i++){ ptr[i]=A(64); if(ptr[i]) ((char*)ptr[i])[0]=1; }
    t0=now_s();
    for(k=0;k<ops;k++){ int idx=(int)(k&(SL-1)); F(ptr[idx],64); ptr[idx]=A(64); if(ptr[idx]) ((char*)ptr[idx])[0]=(char)k; }
    t1=now_s();
    for(i=0;i<SL;i++) F(ptr[i],64);
    printf("  %-11s  %7.3f s   %8.2f Mops/s\n", name, (t1-t0), ((double)ops/(t1-t0))/1e6);
}

int main(int argc, char** argv){
    const char* mode = argc>1 ? argv[1] : "sys";
    int threads = argc>2 ? atoi(argv[2]) : 8;
    long ops    = argc>3 ? atol(argv[3]) : 4000000;
    long smallops = 30000000;
    const char* label = mode;

    if(strcmp(mode,"memop")==0){
        g_use_memop=1;
        thread_init(NULL,NULL); memop_init();
    }

    /* nome de exibicao: se LD_PRELOAD setado, mostra-o */
    { const char* pre=getenv("BENCH_LABEL"); if(pre&&*pre) label=pre; }

    printf("== A: Larson churn  (threads=%d ops/th=%ld) ==\n", threads, ops);
    run_larson(label, threads, 2000, ops);
    printf("== B: small fixed 64B (1 thread ops=%ld) ==\n", smallops);
    run_smallfixed(label, smallops);

    if(g_use_memop) memop_shutdown();
    return 0;
}
