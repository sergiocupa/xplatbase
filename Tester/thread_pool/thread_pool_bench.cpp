/*
 * Tester/thread_pool_bench.cpp
 *
 * Bench oficial: thread_pool (oficial V2.05) vs TBB vs WinTP.
 * (As variantes por grupo g1..g4/all/final foram consolidadas no oficial;
 *  historico de resultados nos thread_pool_bench_results_run*.tsv.)
 *
 * Tabelas (uma por cenario): flat-externo, spawn-arvore, os classicos,
 * malloc-default (tasks que alocam/liberam com malloc padrao) e lento-misto.
 * Colunas: latencias (us no console / ms no TSV), maxms = MEDIA das rodadas,
 * max10 = media dos 10 maiores, Mtask/s, tasks/s, cores, cpu%,
 * xTBB/xWinTP = velocidade relativa (Mtask/s adapter / referencia; >1 = mais rapido).
 *
 * Build: build.bat   Run: thread_pool_bench.exe [reps] [N_flat]
 * Salva thread_pool_bench_results.tsv (ms, 6 casas, ponto decimal, tab).
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <algorithm>
#include <atomic>
#include <random>

extern "C" {
#include "thread_pool.h"                    /* pool oficial (V2.05) */
#include "memory_pool.h"                    /* cenarios mempool/* (interacao) */
#include "thread_handler.h"
}

#ifdef TBB_AVAILABLE
#include <tbb/task_arena.h>
#include <tbb/task_group.h>
#include <tbb/global_control.h>
#endif

/* Stub: isola o bench do platform_init real (memory_pool WIP + pool global de
 * 33 threads criado no CRT init). Mesmo padrao do bench/linux/bench_linux.c.
 * xplatbase.c e memory_pool.c ficam FORA do link (ver build.bat). */
extern "C" XPLATBASE_API void platform_init(void) {}

#ifdef TBB_AVAILABLE
/* global_control e process-wide no TBB; criar/destruir por rep gera crash
 * intermitente no teardown (ver crash.log: tbb::detail::r1::create).
 * Singleton criado no primeiro uso e nunca destruido. */
static tbb::global_control* g_tbb_gc = nullptr;
static void tbb_ensure_gc(int wk){ if (!g_tbb_gc) g_tbb_gc = new tbb::global_control(tbb::global_control::max_allowed_parallelism,(size_t)wk); }
#endif

static LARGE_INTEGER g_freq;
static int g_cpus = 1;
static double qpc_ms(LARGE_INTEGER a, LARGE_INTEGER b){ return (double)(b.QuadPart-a.QuadPart)*1000.0/(double)g_freq.QuadPart; }
static LARGE_INTEGER qpc_now(){ LARGE_INTEGER q; QueryPerformanceCounter(&q); return q; }
static int logical_cpus(){ SYSTEM_INFO si; GetSystemInfo(&si); return (int)si.dwNumberOfProcessors; }
static double med(std::vector<double> v){ std::sort(v.begin(),v.end()); size_t n=v.size(); return n? ((n&1)?v[n/2]:(v[n/2-1]+v[n/2])/2.0):0.0; }
static double mean(const std::vector<double>& v){ if(v.empty())return 0.0; double s=0; for(double x:v)s+=x; return s/(double)v.size(); }
static double pct(std::vector<double>& v, double q){ return v.empty()?0.0:v[(size_t)(v.size()*q)]; }
static double top10_mean(const std::vector<double>& asc){ int n=(int)asc.size(); if(!n)return 0; int k=n<10?n:10; double s=0; for(int i=n-k;i<n;i++)s+=asc[i]; return s/k; }
static uint32_t lcg(uint32_t* s){ *s=(*s*1664525u)+1013904223u; return *s; }
static void precise_sleep_us(int us){ if(us<=0)return; LARGE_INTEGER a=qpc_now(); double tgt=(double)us/1000.0; for(;;){ if(qpc_ms(a,qpc_now())>=tgt)return; YieldProcessor(); } }

struct CpuSnap { unsigned long long t100ns; };
static unsigned long long ft2u(const FILETIME& f){ ULARGE_INTEGER u; u.LowPart=f.dwLowDateTime; u.HighPart=f.dwHighDateTime; return u.QuadPart; }
static CpuSnap cpu_snap(){ FILETIME c,e,k,u; GetProcessTimes(GetCurrentProcess(),&c,&e,&k,&u); CpuSnap s; s.t100ns=ft2u(k)+ft2u(u); return s; }
static double cpu_ms(CpuSnap a, CpuSnap b){ return (double)(b.t100ns-a.t100ns)/10000.0; }

struct Res { double wall_ms,p50,p99,p999,maxv,max10,mtask_s,tasks_s,cores,cpu_pct; int wrk; };
struct Scen { const char* name; int tasks; int wmin, wmax, long_every, long_us; int allocs; int alloc_pool; };

/* ===================== API generica dos pools ===================== */
struct PoolAPI {
    void* (*create)(int);
    void  (*destroy)(void*);
    bool  (*submit)(void*, void(*)(void*), void*);
    void  (*wait_idle)(void*);
    void  (*dims)(void*, int*, int*);
};

#define MK_API(pfx, T, CRE, DES, SUB, WID, DIM)                                        \
    static void* pfx##_cr(int c){ return (void*)CRE(c); }                             \
    static void  pfx##_de(void* p){ DES((T*)p); }                                     \
    static bool  pfx##_su(void* p, void(*f)(void*), void* a){ return SUB((T*)p,f,a) != 0; } \
    static void  pfx##_wi(void* p){ WID((T*)p); }                                     \
    static void  pfx##_dm(void* p,int* w,int* c){ DIM((T*)p,w,c); }

MK_API(of, ThreadPool,   pool_create_relative, pool_destroy_relative, pool_submit_relative, pool_wait_idle_relative, pool_dims_relative)

#define NPOOL 1
static const PoolAPI POOLS[NPOOL] = {
    { of_cr, of_de, of_su, of_wi, of_dm },
};

static const char* NM[] = {"oficial","TBB","WinTP"};
#define NADAPT 3

/* ===================== flat ===================== */
struct BTask { LARGE_INTEGER enq, start; int work_us; int alloc_n; int alloc_pool; uint32_t alloc_seed; std::atomic<int>* done; };

/* cenario malloc-default: aloca/escreve/libera com o malloc padrao do CRT */
static void alloc_work(int n, uint32_t seed){
    void* ptrs[16]; int k = n>16?16:n;
    for (int i=0;i<k;i++){
        uint32_t sz = 64u + (lcg(&seed) % 1985u);          /* 64..2048B */
        ptrs[i]=malloc(sz);
        if (ptrs[i]){ ((volatile char*)ptrs[i])[0]=(char)i; ((volatile char*)ptrs[i])[sz-1]=(char)sz; }
    }
    for (int i=0;i<k;i++) free(ptrs[i]);
}

/* cenario mempool: mesma carga, mas via memory_pool (memop_alloc_raw/free_raw).
 * Objetivo: interacao thread_pool x memory_pool (lanes por worker, churn de
 * threads, free remoto eventual), nao performance. */
static void alloc_work_pool(int n, uint32_t seed){
    void* ptrs[16]; int k = n>16?16:n;
    for (int i=0;i<k;i++){
        uint32_t sz = 64u + (lcg(&seed) % 1985u);          /* 64..2048B */
        ptrs[i]=memop_alloc_raw(sz);
        if (ptrs[i]){ ((volatile char*)ptrs[i])[0]=(char)i; ((volatile char*)ptrs[i])[sz-1]=(char)sz; }
    }
    for (int i=0;i<k;i++) memop_free_raw(ptrs[i]);
}

static void task_body(BTask* t){
    t->start=qpc_now();
    if      (t->alloc_n && t->alloc_pool) alloc_work_pool(t->alloc_n, t->alloc_seed);
    else if (t->alloc_n)                  alloc_work(t->alloc_n, t->alloc_seed);
    else                                  precise_sleep_us(t->work_us);
    t->done->fetch_add(1,std::memory_order_relaxed);
}
static void tramp(void* a){ task_body((BTask*)a); }
static VOID CALLBACK wintp_simple(PTP_CALLBACK_INSTANCE,PVOID a){ task_body((BTask*)a); }
static void drain_counter(std::atomic<int>& d,int n){ while(d.load(std::memory_order_acquire)<n) Sleep(0); }

static void fill_work(std::vector<BTask>& T, const Scen& s){
    uint32_t rnd=0x12345678u;
    for (int i=0;i<(int)T.size();i++){
        int w=s.wmin;
        if (s.wmax>s.wmin){ int span=s.wmax-s.wmin+1; w=s.wmin+(int)(lcg(&rnd)%(uint32_t)span); }
        if (s.long_every>0 && (i%s.long_every)==0) w=s.long_us;
        T[i].work_us=w;
        T[i].alloc_n=s.allocs;
        T[i].alloc_pool=s.alloc_pool;
        T[i].alloc_seed=0x9E3779B9u*(uint32_t)(i+1);
    }
}
static void finish(Res& r, double wall, CpuSnap c0, CpuSnap c1, std::vector<double>& lat, int tasks){
    r.wall_ms=wall;
    std::sort(lat.begin(),lat.end());
    r.p50=pct(lat,0.5);r.p99=pct(lat,0.99);r.p999=pct(lat,0.999);r.maxv=lat.empty()?0:lat.back();r.max10=top10_mean(lat);
    r.tasks_s=wall>0?(double)tasks/(wall/1000.0):0; r.mtask_s=r.tasks_s/1e6;
    r.cores = wall>0 ? cpu_ms(c0,c1)/wall : 0.0;
    r.cpu_pct = r.cores*100.0/(double)g_cpus;
}
template<class CR,class SU,class DE>
static Res run_flat(const Scen& s, int wrk, CR cr, SU su, DE de){
    int tasks=s.tasks;
    std::vector<BTask> T((size_t)tasks); std::atomic<int> done{0};
    for(auto&x:T)x.done=&done;
    fill_work(T,s);
    cr();
    CpuSnap c0=cpu_snap(); LARGE_INTEGER t0=qpc_now();
    for(int i=0;i<tasks;i++){ T[i].enq=qpc_now(); su(&T[i]); }
    drain_counter(done,tasks);
    LARGE_INTEGER t1=qpc_now(); CpuSnap c1=cpu_snap();
    de();
    Res r; memset(&r,0,sizeof(r)); r.wrk=wrk;
    std::vector<double> lat((size_t)tasks);
    for(int i=0;i<tasks;i++) lat[i]=qpc_ms(T[i].enq,T[i].start);
    finish(r,qpc_ms(t0,t1),c0,c1,lat,tasks);
    return r;
}
static Res flat_pool(const Scen& s, const PoolAPI& api){
    void* p=nullptr; int w=0,c=0;
    Res r=run_flat(s,0,
        [&]{p=api.create(0);api.dims(p,&w,&c);},
        [&](BTask*t){while(!api.submit(p,tramp,t)){}},
        [&]{api.destroy(p);});
    r.wrk=w; return r;
}
static Res flat_wintp(const Scen& s,int wk){ PTP_POOL p=nullptr; TP_CALLBACK_ENVIRON e;
    return run_flat(s,wk,[&]{p=CreateThreadpool(NULL);SetThreadpoolThreadMaximum(p,(DWORD)wk);SetThreadpoolThreadMinimum(p,(DWORD)wk);InitializeThreadpoolEnvironment(&e);SetThreadpoolCallbackPool(&e,p);},
        [&](BTask*t){while(!TrySubmitThreadpoolCallback(wintp_simple,t,&e))Sleep(0);}, [&]{DestroyThreadpoolEnvironment(&e);CloseThreadpool(p);}); }
static Res flat_tbb(const Scen& s,int wk){
#ifdef TBB_AVAILABLE
    tbb::task_arena*ar=nullptr;
    return run_flat(s,wk,[&]{tbb_ensure_gc(wk);ar=new tbb::task_arena(wk);ar->initialize();},
        [&](BTask*t){ar->enqueue([t]{task_body(t);});}, [&]{delete ar;});
#else
    Res r; memset(&r,0,sizeof(r)); return r;
#endif
}

/* ===================== spawn-arvore ===================== */
struct Tree { int n; std::vector<int> cstart, ccount; };
static Tree build_tree(int target, unsigned seed){
    Tree t; std::mt19937 rng(seed);
    t.cstart.push_back(0); t.ccount.push_back(0); int total=1;
    for(int i=0;i<total;i++){
        if(total>=target) continue;
        int k=1+(int)(rng()%4);
        if(k>target-total) k=target-total;
        t.cstart[i]=total; t.ccount[i]=k;
        for(int j=0;j<k;j++){ t.cstart.push_back(0); t.ccount.push_back(0); total++; }
    }
    t.n=total; return t;
}
struct SpawnCtx;
struct NodeArg { SpawnCtx* ctx; int idx; LARGE_INTEGER enq, start; };
struct SpawnCtx {
    const Tree* tree; NodeArg* args; std::atomic<long long>* outstanding; int adapter;
    const PoolAPI* papi; void* pph; PTP_CALLBACK_ENVIRON env;
#ifdef TBB_AVAILABLE
    tbb::task_group* tg;
#endif
};
static void node_run(NodeArg* na);
static void node_tramp(void* a){ node_run((NodeArg*)a); }
static VOID CALLBACK node_wintp(PTP_CALLBACK_INSTANCE,PVOID a){ node_run((NodeArg*)a); }
static void submit_child(SpawnCtx* c, int idx){   /* adapter: 0..5 pools, 6 TBB, 7 WinTP */
    NodeArg* na=&c->args[idx]; na->ctx=c; na->idx=idx;
    c->outstanding->fetch_add(1,std::memory_order_relaxed);
    QueryPerformanceCounter(&na->enq);
    int a=c->adapter;
    if (a<NPOOL) { while(!c->papi->submit(c->pph,node_tramp,na)){} }
#ifdef TBB_AVAILABLE
    else if (a==NPOOL) { c->tg->run([na]{ node_run(na); }); }
#endif
    else { while(!TrySubmitThreadpoolCallback(node_wintp,na,c->env))Sleep(0); }
}
static void node_run(NodeArg* na){
    SpawnCtx* c=na->ctx; QueryPerformanceCounter(&na->start);
    int s=c->tree->cstart[na->idx], k=c->tree->ccount[na->idx];
    for(int j=0;j<k;j++) submit_child(c, s+j);
    c->outstanding->fetch_sub(1,std::memory_order_relaxed);
}
static Res spawn_once(int adapter, const Tree& tree, NodeArg* args, int cpus){
    std::atomic<long long> outstanding{0};
    SpawnCtx ctx; memset(&ctx,0,sizeof(ctx));
    ctx.tree=&tree; ctx.args=args; ctx.outstanding=&outstanding; ctx.adapter=adapter;
    int wrk=cpus;
    void* pp=nullptr; PTP_POOL wp=nullptr; TP_CALLBACK_ENVIRON env;
#ifdef TBB_AVAILABLE
    tbb::task_arena*ar=nullptr; tbb::task_group tg;
#endif
    if (adapter<NPOOL){ ctx.papi=&POOLS[adapter]; pp=ctx.papi->create(0); ctx.pph=pp; int l; ctx.papi->dims(pp,&wrk,&l); }
#ifdef TBB_AVAILABLE
    else if (adapter==NPOOL){ tbb_ensure_gc(cpus); ar=new tbb::task_arena(cpus); ar->initialize(); ctx.tg=&tg; wrk=cpus; }
#endif
    else { wp=CreateThreadpool(NULL); SetThreadpoolThreadMaximum(wp,(DWORD)cpus); SetThreadpoolThreadMinimum(wp,(DWORD)cpus); InitializeThreadpoolEnvironment(&env); SetThreadpoolCallbackPool(&env,wp); ctx.env=&env; wrk=cpus; }

    CpuSnap c0=cpu_snap(); LARGE_INTEGER t0=qpc_now();
#ifdef TBB_AVAILABLE
    if (adapter==NPOOL){ ar->execute([&]{ submit_child(&ctx,0); tg.wait(); }); }
    else
#endif
    { submit_child(&ctx,0); while(outstanding.load(std::memory_order_acquire)>0) Sleep(0); }
    LARGE_INTEGER t1=qpc_now(); CpuSnap c1=cpu_snap();

    if (adapter<NPOOL) ctx.papi->destroy(pp);
#ifdef TBB_AVAILABLE
    else if (adapter==NPOOL){ delete ar; }
#endif
    else { DestroyThreadpoolEnvironment(&env); CloseThreadpool(wp); }

    Res r; memset(&r,0,sizeof(r)); r.wrk=wrk;
    int n=tree.n; std::vector<double> lat; lat.reserve(n);
    for(int i=0;i<n;i++) if(args[i].start.QuadPart) lat.push_back(qpc_ms(args[i].enq,args[i].start));
    finish(r,qpc_ms(t0,t1),c0,c1,lat,n);
    return r;
}

/* ===================== driver ===================== */
struct Row { char name[64]; Res r[NADAPT]; };
static std::vector<Row> g_rows;
static void record_row(const char* name, Res* res){
    Row row; memset(&row,0,sizeof(row)); strncpy(row.name,name,sizeof(row.name)-1);
    for(int i=0;i<NADAPT;i++) row.r[i]=res[i];
    g_rows.push_back(row);
}
static const char* f6(char* buf, double v){ snprintf(buf,32,"%.6f",v); return buf; }
/* velocidade relativa (Mtask/s do adapter / Mtask/s da referencia); >1 = mais rapido */
static double rel_speed(const Res& r, const Res& ref){ return ref.mtask_s>0 ? r.mtask_s/ref.mtask_s : 0.0; }

static void write_tsv(const char* path){
    FILE* f=fopen(path,"w"); if(!f){ fprintf(stderr,"nao abriu %s\n",path); return; }
    fprintf(f,"scenario\tadapter\twrk\twall_ms\tp50_ms\tp99_ms\tp999_ms\tmax_ms\tmax10_ms\tMtask_s\ttasks_s\tcores\tcpu_pct\txTBB\txWinTP\n");
    char b[16][32];
    for(const Row& row: g_rows){
        const Res& tbb=row.r[NADAPT-2]; const Res& wtp=row.r[NADAPT-1];
        for(int i=0;i<NADAPT;i++){ const Res& r=row.r[i];
            fprintf(f,"%s\t%s\t%d\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n",
                row.name, NM[i], r.wrk, f6(b[0],r.wall_ms), f6(b[1],r.p50), f6(b[2],r.p99), f6(b[3],r.p999),
                f6(b[4],r.maxv), f6(b[5],r.max10), f6(b[6],r.mtask_s), f6(b[7],r.tasks_s), f6(b[8],r.cores), f6(b[9],r.cpu_pct),
                f6(b[10],rel_speed(r,tbb)), f6(b[11],rel_speed(r,wtp))); }
    }
    fclose(f);
    fprintf(stderr,"\n>> resultados salvos em: %s  (TSV, ms, 6 casas, ponto decimal)\n",path); fflush(stderr);
}
static void aggregate(Res* out, std::vector<Res>* per){
    for(int a=0;a<NADAPT;a++){
        std::vector<double> w,p50,p99,p999,mx,mx10,mt,ts,co,cp; int wrk=0;
        for(const Res& r: per[a]){ w.push_back(r.wall_ms);p50.push_back(r.p50);p99.push_back(r.p99);p999.push_back(r.p999);
            mx.push_back(r.maxv);mx10.push_back(r.max10);mt.push_back(r.mtask_s);ts.push_back(r.tasks_s);co.push_back(r.cores);cp.push_back(r.cpu_pct);wrk=r.wrk; }
        Res o; memset(&o,0,sizeof(o));
        o.wall_ms=med(w);o.p50=med(p50);o.p99=med(p99);o.p999=med(p999);o.maxv=mean(mx);o.max10=med(mx10);
        o.mtask_s=med(mt);o.tasks_s=med(ts);o.cores=med(co);o.cpu_pct=med(cp);o.wrk=wrk;
        out[a]=o;
    }
}
static void print_table(const char* title, Res* res){
    printf("\n=== %s ===\n",title);
    printf("%-8s %4s %9s %9s %9s %9s %9s %9s %10s %12s %7s %7s %7s %7s\n",
        "adapter","wrk","wall_ms","p50us","p99us","p999us","maxms","max10ms","Mtask/s","tasks/s","cores","cpu%","xTBB","xWinTP");
    printf("-------------------------------------------------------------------------------------------------------------------------------------\n");
    for(int i=0;i<NADAPT;i++)
        printf("%-8s %4d %9.2f %9.3f %9.3f %9.3f %9.3f %9.3f %10.4f %12.0f %7.2f %7.1f %7.2f %7.2f\n",
            NM[i],res[i].wrk,res[i].wall_ms,res[i].p50*1000,res[i].p99*1000,res[i].p999*1000,res[i].maxv,res[i].max10,
            res[i].mtask_s,res[i].tasks_s,res[i].cores,res[i].cpu_pct,
            rel_speed(res[i],res[NADAPT-2]),rel_speed(res[i],res[NADAPT-1]));
    record_row(title,res);
}
static void bench_flat(const Scen& s, int reps, int cpus){
    std::vector<Res> per[NADAPT];
    for(int rep=0;rep<reps;rep++) for(int a=0;a<NADAPT;a++){
        Res r = a<NPOOL ? flat_pool(s,POOLS[a]) : a==NPOOL ? flat_tbb(s,cpus) : flat_wintp(s,cpus);
        per[a].push_back(r);
        fprintf(stderr,"    [%s/%s] rep %d/%d  wall=%.1fms  %.3f Mtask/s  cpu=%.0f%%\n",s.name,NM[a],rep+1,reps,r.wall_ms,r.mtask_s,r.cpu_pct); fflush(stderr);
    }
    Res res[NADAPT]; aggregate(res,per); print_table(s.name,res);
}

int main(int argc,char**argv){
    QueryPerformanceFrequency(&g_freq);
    int reps=(argc>1)?atoi(argv[1]):10; if(reps<1)reps=1;
    int Nflat=(argc>2)?atoi(argv[2]):1000000; if(Nflat<1000)Nflat=1000;
    g_cpus=logical_cpus();
    printf("CPUs=%d  reps=%d  N_flat=%d\n",g_cpus,reps,Nflat); fflush(stdout);

    /* memory_pool: init + hooks de thread — todo worker de todo pool criado no
     * bench ganha/perde lane (interacao thread_pool x memory_pool). Threads do
     * TBB/WinTP nao passam pelos hooks e usam o fallback lazy do memop. */
    memop_init();
    thread_init(memop_on_created_thread, memop_on_ended_thread);

    fprintf(stderr,"\n## flat-externo\n"); fflush(stderr);
    { Scen s={"flat-externo",Nflat,0,0,0,0,0}; bench_flat(s,reps,g_cpus); }

    fprintf(stderr,"\n## spawn-arvore\n"); fflush(stderr);
    {
        Tree tree=build_tree(Nflat,12345u);
        NodeArg* args=(NodeArg*)malloc((size_t)tree.n*sizeof(NodeArg));
        std::vector<Res> per[NADAPT];
        for(int rep=0;rep<reps;rep++) for(int a=0;a<NADAPT;a++){
            Res r=spawn_once(a,tree,args,g_cpus); per[a].push_back(r);
            fprintf(stderr,"    [spawn/%s] rep %d/%d  wall=%.1fms  %.3f Mtask/s\n",NM[a],rep+1,reps,r.wall_ms,r.mtask_s); fflush(stderr);
        }
        free(args);
        Res res[NADAPT]; aggregate(res,per); print_table("spawn-arvore (task dentro de task)",res);
    }

    static Scen SCN[] = {
        {"rapida/baixa",         20000,  0,  0,   0,   0, 0},
        {"rapida/media",         60000,  0,  0,   0,   0, 0},
        {"rapida/alta",         120000,  0,  0,   0,   0, 0},
        {"rapida/ultra",        180000,  0,  0,   0,   0, 0},
        {"media/baixa",          20000, 15, 25,   0,   0, 0},
        {"media/media",          60000, 15, 35,   0,   0, 0},
        {"media/alta",          120000, 15, 45,   0,   0, 0},
        {"media/ultra",         180000, 15, 55,   0,   0, 0},
        {"mista/baixa",          20000,  0,  5,  16, 250, 0},
        {"mista/media",          60000,  0,  5,  16, 300, 0},
        {"mista/alta",          120000,  0,  5,  16, 350, 0},
        {"mista/ultra",         180000,  0,  5,  16, 400, 0},
        {"satur/alta",          160000,  0,  0,   0,   0, 0},
        {"satur/ultra",         240000,  0,  0,   0,   0, 0},
        {"rajada-curta/media",   60000,  0,  0,   0,   0, 0},
        {"rajada-mista/media",   60000,  0,  5,  12, 300, 0},
        {"mista-massiva/alta",  180000,  0,  8,  24, 500, 0},
        {"mista-massiva/ultra", 240000,  0,  8,  24, 600, 0},
        {"malloc-default/media", 60000,  0,  0,   0,   0, 8, 0},
        {"malloc-default/alta", 120000,  0,  0,   0,   0, 8, 0},
        {"mempool/media",        60000,  0,  0,   0,   0, 8, 1},
        {"mempool/alta",        120000,  0,  0,   0,   0, 8, 1},
        {"lento-misto",           4000,  0,  0,   2, 8000, 0, 0},
    };
    int nscn=(int)(sizeof(SCN)/sizeof(SCN[0]));
    for(int i=0;i<nscn;i++){ fprintf(stderr,"\n## %s\n",SCN[i].name); fflush(stderr); bench_flat(SCN[i],reps,g_cpus); }

    write_tsv("thread_pool_bench_results.tsv");

    /* evidencia da interacao: lanes/allocs/frees devem estar pareados */
    {
        MemPoolStats st; memop_get_stats(&st);
        printf("\n== memory_pool stats ==\n");
        printf("lanes criadas/destruidas : %llu / %llu\n",(unsigned long long)st.lanes_created,(unsigned long long)st.lanes_destroyed);
        printf("alloc/free               : %llu / %llu\n",(unsigned long long)st.alloc_count,(unsigned long long)st.free_count);
        printf("frees remotos            : %llu\n",(unsigned long long)st.remote_frees);
        printf("refills sync/async       : %llu / %llu (req async %llu)\n",(unsigned long long)st.sync_refills,(unsigned long long)st.async_refills,(unsigned long long)st.async_requests);
        printf("reservado do SO / cache  : %llu KB / %llu chunks (purgas %llu)\n",(unsigned long long)(st.os_reserved_bytes/1024),(unsigned long long)st.cached_chunks,(unsigned long long)st.purge_count);
        fflush(stdout);
    }
    return 0;
}
