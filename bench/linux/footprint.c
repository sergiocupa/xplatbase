/*
 * footprint.c - mede recuperacao de memoria (Linux).
 *   1) baseline RSS
 *   2) aloca ~512MB vivos (blocos aleatorios), toca a memoria -> pico (VmHWM)
 *   3) libera tudo
 *   4) RSS apos liberar  -> quanto voltou ao SO
 *
 * mode: sys (malloc, alocador via LD_PRELOAD) | memop (embutido)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../../Xplatbase/Xplatbase/src/memory_pool.h"

void platform_init(void) {}

static int g_memop = 0;
static void* A(size_t n){ if(!g_memop) return malloc(n); { MemBuffer b=memop_alloc(n); return b.Ptr; } }
static void  F(void* p){ if(!p) return; if(!g_memop){ free(p); return; } { MemBuffer b; b.Ptr=p; b.Size=0; memop_free(&b); } }

static uint64_t rng_next(uint64_t* s){ uint64_t x=*s; x^=x<<13; x^=x>>7; x^=x<<17; *s=x; return x; }
static size_t rand_size(uint64_t* s){
    uint32_t r=(uint32_t)(rng_next(s)%100u);
    if(r<75) return 8   + (size_t)(rng_next(s)%121u);
    if(r<95) return 129 + (size_t)(rng_next(s)%1920u);
    return        2049 + (size_t)(rng_next(s)%63488u);
}

static long rss_kb(const char* key){
    FILE* f=fopen("/proc/self/status","r"); char line[256]; long v=-1;
    if(!f) return -1;
    while(fgets(line,sizeof line,f)){ if(strncmp(line,key,strlen(key))==0){ sscanf(line+strlen(key)," %ld",&v); break; } }
    fclose(f); return v;
}

int main(int argc, char** argv){
    const char* mode = argc>1?argv[1]:"sys";
    const char* label = getenv("BENCH_LABEL"); if(!label||!*label) label=mode;
    long target_mb = argc>2?atol(argv[2]):512;
    uint64_t rng = 0x1234567ull;
    size_t want = (size_t)target_mb*1024*1024;
    size_t got = 0;
    long cap = 4000000, n = 0, i;
    void** ptr;
    long base, peak, after;

    if(strcmp(mode,"memop")==0){ g_memop=1; thread_init(NULL,NULL); memop_init(); }
    ptr = (void**)malloc((size_t)cap*sizeof(void*));

    base = rss_kb("VmRSS:");

    while(got < want && n < cap){
        size_t s = rand_size(&rng);
        void* p = A(s);
        if(!p) break;
        ((char*)p)[0]=1; ((char*)p)[s-1]=1;   /* toca: commit real */
        ptr[n++]=p; got += s;
    }
    peak = rss_kb("VmHWM:");

    for(i=0;i<n;i++) F(ptr[i]);
    /* mede com o pool AINDA vivo: reflete o que o alocador devolveu ao SO
     * vs reteve em cache (shutdown devolveria tudo e mascararia isso). */
    after = rss_kb("VmRSS:");

    printf("  %-10s  live=%4ldMB  base=%5ldMB  pico=%5ldMB  apos_free=%5ldMB  devolvido=%5ldMB\n",
           label, (long)(got/1024/1024), base/1024, peak/1024, after/1024, (peak-after)/1024);
    free(ptr);
    return 0;
}
