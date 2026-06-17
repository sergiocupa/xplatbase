/*
 * Tester/V2/thread_pool_bench.cpp
 *
 * Bench oficial: thread_pool (oficial: arena + core/reserva + elastico) vs TBB vs WinTP.
 *
 * Tabelas (uma por cenario): flat-externo, spawn-arvore, os 18 classicos e
 * lento-misto. Colunas: latencias (us no console / ms no TSV), maxms = MEDIA das
 * rodadas, max10 = media dos 10 maiores, Mtask/s, tasks/s, cores, cpu%.
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

#include "thread_pool.h"   /* pool oficial */

#ifdef TBB_AVAILABLE
#include <tbb/task_arena.h>
#include <tbb/task_group.h>
#include <tbb/global_control.h>
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
struct Scen { const char* name; int tasks; int wmin, wmax, long_every, long_us; };

/* ===================== flat ===================== */
struct BTask { LARGE_INTEGER enq, start; int work_us; std::atomic<int>* done; };
static void task_body(BTask* t){ t->start=qpc_now(); precise_sleep_us(t->work_us); t->done->fetch_add(1,std::memory_order_relaxed); }
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
static Res flat_pool(const Scen& s){ ThreadPool*p=nullptr;int w=0,c=0; Res r=run_flat(s,0,[&]{p=pool_create(0);pool_dims(p,&w,&c);},[&](BTask*t){while(!pool_submit(p,tramp,t)){}},[&]{pool_destroy(p);}); r.wrk=w; return r; }
static Res flat_wintp(const Scen& s,int wk){ PTP_POOL p=nullptr; TP_CALLBACK_ENVIRON e;
    return run_flat(s,wk,[&]{p=CreateThreadpool(NULL);SetThreadpoolThreadMaximum(p,(DWORD)wk);SetThreadpoolThreadMinimum(p,(DWORD)wk);InitializeThreadpoolEnvironment(&e);SetThreadpoolCallbackPool(&e,p);},
        [&](BTask*t){while(!TrySubmitThreadpoolCallback(wintp_simple,t,&e))Sleep(0);}, [&]{DestroyThreadpoolEnvironment(&e);CloseThreadpool(p);}); }
static Res flat_tbb(const Scen& s,int wk){
#ifdef TBB_AVAILABLE
    tbb::global_control*gc=nullptr; tbb::task_arena*ar=nullptr;
    return run_flat(s,wk,[&]{gc=new tbb::global_control(tbb::global_control::max_allowed_parallelism,(size_t)wk);ar=new tbb::task_arena(wk);ar->initialize();},
        [&](BTask*t){ar->enqueue([t]{task_body(t);});}, [&]{delete ar;delete gc;});
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
    ThreadPool* pool; PTP_CALLBACK_ENVIRON env;
#ifdef TBB_AVAILABLE
    tbb::task_group* tg;
#endif
};
static void node_run(NodeArg* na);
static void node_tramp(void* a){ node_run((NodeArg*)a); }
static VOID CALLBACK node_wintp(PTP_CALLBACK_INSTANCE,PVOID a){ node_run((NodeArg*)a); }
static void submit_child(SpawnCtx* c, int idx){       /* adapter: 0 oficial, 1 TBB, 2 WinTP */
    NodeArg* na=&c->args[idx]; na->ctx=c; na->idx=idx;
    c->outstanding->fetch_add(1,std::memory_order_relaxed);
    QueryPerformanceCounter(&na->enq);
    int a=c->adapter;
    if (a==0) { while(!pool_submit(c->pool,node_tramp,na)){} }
#ifdef TBB_AVAILABLE
    else if (a==1) { c->tg->run([na]{ node_run(na); }); }
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
    ThreadPool* pp=nullptr; PTP_POOL wp=nullptr; TP_CALLBACK_ENVIRON env;
#ifdef TBB_AVAILABLE
    tbb::global_control*gc=nullptr; tbb::task_arena*ar=nullptr; tbb::task_group tg;
#endif
    if (adapter==0){ pp=pool_create(0); ctx.pool=pp; int l; pool_dims(pp,&wrk,&l); }
#ifdef TBB_AVAILABLE
    else if (adapter==1){ gc=new tbb::global_control(tbb::global_control::max_allowed_parallelism,(size_t)cpus); ar=new tbb::task_arena(cpus); ar->initialize(); ctx.tg=&tg; wrk=cpus; }
#endif
    else { wp=CreateThreadpool(NULL); SetThreadpoolThreadMaximum(wp,(DWORD)cpus); SetThreadpoolThreadMinimum(wp,(DWORD)cpus); InitializeThreadpoolEnvironment(&env); SetThreadpoolCallbackPool(&env,wp); ctx.env=&env; wrk=cpus; }

    CpuSnap c0=cpu_snap(); LARGE_INTEGER t0=qpc_now();
#ifdef TBB_AVAILABLE
    if (adapter==1){ ar->execute([&]{ submit_child(&ctx,0); tg.wait(); }); }
    else
#endif
    { submit_child(&ctx,0); while(outstanding.load(std::memory_order_acquire)>0) Sleep(0); }
    LARGE_INTEGER t1=qpc_now(); CpuSnap c1=cpu_snap();

    if (adapter==0) pool_destroy(pp);
#ifdef TBB_AVAILABLE
    else if (adapter==1){ delete ar; delete gc; }
#endif
    else { DestroyThreadpoolEnvironment(&env); CloseThreadpool(wp); }

    Res r; memset(&r,0,sizeof(r)); r.wrk=wrk;
    int n=tree.n; std::vector<double> lat; lat.reserve(n);
    for(int i=0;i<n;i++) if(args[i].start.QuadPart) lat.push_back(qpc_ms(args[i].enq,args[i].start));
    finish(r,qpc_ms(t0,t1),c0,c1,lat,n);
    return r;
}

/* ===================== driver ===================== */
static const char* NM[3]={"oficial","TBB","WinTP"};
#define NADAPT 3

struct Row { char name[64]; Res r[NADAPT]; };
static std::vector<Row> g_rows;
static void record_row(const char* name, Res* res){
    Row row; memset(&row,0,sizeof(row)); strncpy(row.name,name,sizeof(row.name)-1);
    for(int i=0;i<NADAPT;i++) row.r[i]=res[i];
    g_rows.push_back(row);
}
static const char* f6(char* buf, double v){ snprintf(buf,32,"%.6f",v); return buf; }
static void write_tsv(const char* path){
    FILE* f=fopen(path,"w"); if(!f){ fprintf(stderr,"nao abriu %s\n",path); return; }
    fprintf(f,"scenario\tadapter\twrk\twall_ms\tp50_ms\tp99_ms\tp999_ms\tmax_ms\tmax10_ms\tMtask_s\ttasks_s\tcores\tcpu_pct\n");
    char b[16][32];
    for(const Row& row: g_rows) for(int i=0;i<NADAPT;i++){ const Res& r=row.r[i];
        fprintf(f,"%s\t%s\t%d\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n",
            row.name, NM[i], r.wrk, f6(b[0],r.wall_ms), f6(b[1],r.p50), f6(b[2],r.p99), f6(b[3],r.p999),
            f6(b[4],r.maxv), f6(b[5],r.max10), f6(b[6],r.mtask_s), f6(b[7],r.tasks_s), f6(b[8],r.cores), f6(b[9],r.cpu_pct)); }
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
    printf("%-8s %4s %9s %9s %9s %9s %9s %9s %10s %12s %7s %7s\n",
        "adapter","wrk","wall_ms","p50us","p99us","p999us","maxms","max10ms","Mtask/s","tasks/s","cores","cpu%");
    printf("---------------------------------------------------------------------------------------------------------------------\n");
    for(int i=0;i<NADAPT;i++)
        printf("%-8s %4d %9.2f %9.3f %9.3f %9.3f %9.3f %9.3f %10.4f %12.0f %7.2f %7.1f\n",
            NM[i],res[i].wrk,res[i].wall_ms,res[i].p50*1000,res[i].p99*1000,res[i].p999*1000,res[i].maxv,res[i].max10,
            res[i].mtask_s,res[i].tasks_s,res[i].cores,res[i].cpu_pct);
    record_row(title,res);
}
static void bench_flat(const Scen& s, int reps, int cpus){
    std::vector<Res> per[NADAPT];
    for(int rep=0;rep<reps;rep++) for(int a=0;a<NADAPT;a++){
        Res r = a==0?flat_pool(s): a==1?flat_tbb(s,cpus): flat_wintp(s,cpus);
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

    fprintf(stderr,"\n## flat-externo\n"); fflush(stderr);
    { Scen s={"flat-externo",Nflat,0,0,0,0}; bench_flat(s,reps,g_cpus); }

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
        {"rapida/baixa",         20000,  0,  0,   0,   0},
        {"rapida/media",         60000,  0,  0,   0,   0},
        {"rapida/alta",         120000,  0,  0,   0,   0},
        {"rapida/ultra",        180000,  0,  0,   0,   0},
        {"media/baixa",          20000, 15, 25,   0,   0},
        {"media/media",          60000, 15, 35,   0,   0},
        {"media/alta",          120000, 15, 45,   0,   0},
        {"media/ultra",         180000, 15, 55,   0,   0},
        {"mista/baixa",          20000,  0,  5,  16, 250},
        {"mista/media",          60000,  0,  5,  16, 300},
        {"mista/alta",          120000,  0,  5,  16, 350},
        {"mista/ultra",         180000,  0,  5,  16, 400},
        {"satur/alta",          160000,  0,  0,   0,   0},
        {"satur/ultra",         240000,  0,  0,   0,   0},
        {"rajada-curta/media",   60000,  0,  0,   0,   0},
        {"rajada-mista/media",   60000,  0,  5,  12, 300},
        {"mista-massiva/alta",  180000,  0,  8,  24, 500},
        {"mista-massiva/ultra", 240000,  0,  8,  24, 600},
        {"lento-misto",           4000,  0,  0,   2, 8000},
    };
    int nscn=(int)(sizeof(SCN)/sizeof(SCN[0]));
    for(int i=0;i<nscn;i++){ fprintf(stderr,"\n## %s\n",SCN[i].name); fflush(stderr); bench_flat(SCN[i],reps,g_cpus); }

    write_tsv("thread_pool_bench_results.tsv");
    return 0;
}
