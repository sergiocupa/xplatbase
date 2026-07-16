//  MIT License - Modified for Mandatory Attribution
//
//  Copyright(c) 2025 Sergio Paludo
//
//  github.com/sergiocupa
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED.

#include "mem_leak_watch.h"
#include "memory_pool.h"
#include "thread_handler.h"
#include "thread_wait.h"
#include "atomics.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _MSC_VER
#include <share.h>
#endif

#ifndef XPLATBASE_WIN
#include <unistd.h>
#endif

#ifdef XPLATBASE_WIN
/* THREAD_BASIC_INFORMATION/NtQueryInformationThread sao "quasi-documentados"
 * (ntdll, nao expostos de forma estavel em todo SDK/winternl.h) -- declarados
 * localmente com a ABI estavel conhecida e resolvidos via GetProcAddress em
 * vez de linkar direto contra ntdll.lib (evita depender de um import lib que
 * nem sempre esta no caminho padrao do linker). */
typedef struct MLW_CLIENT_ID { PVOID UniqueProcess; PVOID UniqueThread; } MLW_CLIENT_ID;
typedef struct MLW_THREAD_BASIC_INFORMATION
{
    LONG          ExitStatus;
    PVOID         TebBaseAddress;
    MLW_CLIENT_ID ClientId;
    ULONG_PTR     AffinityMask;
    LONG          Priority;
    LONG          BasePriority;
}
MLW_THREAD_BASIC_INFORMATION;

typedef LONG (NTAPI *MlwNtQueryInformationThread_t)(HANDLE, LONG, PVOID, ULONG, PULONG);
static MlwNtQueryInformationThread_t g_NtQueryInformationThread;

static boolean mlw_resolve_ntdll(void)
{
    if (g_NtQueryInformationThread) return true;
    {
        HMODULE ntdll = GetModuleHandleA("ntdll.dll");
        if (!ntdll) return false;
        g_NtQueryInformationThread = (MlwNtQueryInformationThread_t)
            GetProcAddress(ntdll, "NtQueryInformationThread");
    }
    return g_NtQueryInformationThread != NULL;
}
#endif

/* ===========================================================================
 * Estado do modulo
 * ===========================================================================
 * Todo o buffer de trabalho do scan (snapshot de spans, handles de thread,
 * bitmap de alcancavel) usa malloc/free puro, de proposito -- nunca
 * memop_alloc_raw. O que esta sendo escaneado E o memory_pool; alocar nele
 * enquanto se varre ele mesmo seria misturar o observador com o observado. */

static MemLeakWatchConfig g_cfg;
static xatomic_int  g_running;     /* 0/1: thread de monitor ativa */
static xwait_t      g_wait;
static Thread*      g_thread;
static xmutex_t     g_scan_lock;   /* 1 scan por vez (timer x chamada manual) */
static xmutex_t     g_log_lock;
static FILE*        g_log_file;
static xatomic_int  g_sym_inited;  /* SymInitialize: uma vez, sob demanda */

typedef enum { MLW_INFO = 0, MLW_WARN = 1, MLW_CRITICAL = 2 } MlwLevel;

static const char* mlw_level_name(MlwLevel lvl)
{
    switch (lvl) {
        case MLW_WARN:     return "WARN";
        case MLW_CRITICAL: return "CRITICAL";
        default:            return "INFO";
    }
}

/* ===========================================================================
 * Caminho de log default: SEMPRE absoluto (pasta do executavel), nunca
 * relativo ao diretorio de trabalho corrente -- CWD varia conforme quem
 * inicia o processo (servico, IDE, terminal), o que tornava o log dificil
 * de achar. Calculado uma vez, guardado num buffer estatico. */
#ifdef XPLATBASE_WIN
static char g_default_log_path[MAX_PATH * 2];

static const char* mlw_default_log_path(void)
{
    char exe_path[MAX_PATH];
    DWORD n;
    char* slash;

    if (g_default_log_path[0]) return g_default_log_path;

    n = GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    if (n == 0 || n == MAX_PATH)
    {
        /* nao foi possivel resolver o caminho do executavel: cai pro CWD mesmo,
         * mas ainda assim resolve pra absoluto via GetFullPathNameA. */
        GetFullPathNameA("mem_leak_watch.log", sizeof(g_default_log_path), g_default_log_path, NULL);
        return g_default_log_path;
    }

    slash = strrchr(exe_path, '\\');
    if (slash) *slash = 0; else exe_path[0] = 0;
    snprintf(g_default_log_path, sizeof(g_default_log_path), "%s\\mem_leak_watch.log", exe_path);
    return g_default_log_path;
}
#else
static const char* mlw_default_log_path(void)
{
    static char buf[4096];
    if (buf[0]) return buf;
    if (!getcwd(buf, sizeof(buf))) { strcpy(buf, "."); }
    strncat(buf, "/mem_leak_watch.log", sizeof(buf) - strlen(buf) - 1);
    return buf;
}
#endif

/* ===========================================================================
 * Config / defaults
 * ===========================================================================
 * Limiar default e relativo a RAM fisica da maquina (nao um valor fixo em
 * bytes) -- e o que o pedido original descreve: avisar perto de usar toda a
 * memoria da maquina, nao um numero arbitrario que nao escala entre ambientes. */
void mem_leak_watch_default_config(MemLeakWatchConfig* out)
{
    uint64 total_phys = (uint64)4 * 1024 * 1024 * 1024;  /* fallback: 4GB se a query falhar */
    if (!out) return;

#ifdef XPLATBASE_WIN
    {
        MEMORYSTATUSEX ms; ms.dwLength = sizeof(ms);
        if (GlobalMemoryStatusEx(&ms)) total_phys = (uint64)ms.ullTotalPhys;
    }
#endif

    out->enabled = true;
#ifdef _DEBUG
    out->interval_ms = 1000;
#else
    out->interval_ms = 60000;
#endif
    out->warn_threshold_bytes = (total_phys * 70) / 100;
    out->crit_threshold_bytes = (total_phys * 90) / 100;
    out->log_path = mlw_default_log_path();
}

/* ===========================================================================
 * Log
 * ===========================================================================*/
static void mlw_log_open_locked(void)
{
    const char* path = g_cfg.log_path ? g_cfg.log_path : mlw_default_log_path();
    if (g_log_file) return;
#ifdef _MSC_VER
    /* _fsopen com _SH_DENYNO em vez de fopen_s: pede acesso compartilhado
     * explicito, pra permitir que outra ferramenta (tail -f, visualizador de
     * log, ou o proprio processo lendo de volta) leia o arquivo enquanto
     * mem_leak_watch continua com ele aberto pra escrita (append). Sem isso,
     * qualquer leitura concorrente falha com "Permission denied" (EACCES). */
    g_log_file = _fsopen(path, "a", _SH_DENYNO);
#else
    g_log_file = fopen(path, "a");
#endif
    if (g_log_file)
        fprintf(g_log_file, "ts_ms\tlevel\tkind\tspan\tsize\tused\tage_ms\tsite\n");
}

static void mlw_log(MlwLevel lvl, const char* kind, void* span_base, uint64 size,
                     uint32 used, uint64 age_ms, const char* site)
{
#ifdef XPLATBASE_WIN
    uint64 now = (uint64)GetTickCount64();
#else
    uint64 now = 0;
#endif
    thread_mutex_lock_inline(&g_log_lock);
    mlw_log_open_locked();
    if (g_log_file)
    {
        fprintf(g_log_file, "%llu\t%s\t%s\t%p\t%llu\t%u\t%llu\t%s\n",
            (unsigned long long)now, mlw_level_name(lvl), kind, span_base,
            (unsigned long long)size, used, (unsigned long long)age_ms,
            site ? site : "");
        fflush(g_log_file);
    }
    thread_mutex_unlock_inline(&g_log_lock);

    /* "scan_clean" (nada encontrado) so vai pro arquivo -- ecoar no console a
     * cada varredura sem achado transforma um monitor de baixo ruido em spam,
     * e pode intercalar com a saida de quem estiver rodando (ex.: quebra
     * tabelas de benchmark impressas em stdout ao mesmo tempo). So imprime no
     * console o que e de fato acionavel: candidato a vazamento ou critico. */
    if (strcmp(kind, "scan_clean") != 0)
    {
        fprintf(stderr, "[mem_leak_watch/%s] %s span=%p size=%llu used=%u age=%llums site=%s\n",
            mlw_level_name(lvl), kind, span_base, (unsigned long long)size, used,
            (unsigned long long)age_ms, site ? site : "");
    }
}

#ifdef XPLATBASE_WIN
/* ===========================================================================
 * Simbolizacao (endereco -> "func+offset") -- SO fora de qualquer thread
 * suspensa. SymInitialize e feito uma vez, sob demanda.
 * ===========================================================================*/
static void mlw_sym_ensure_init(void)
{
    int expected = 0;
    if (atomic_get_inline(&g_sym_inited)) return;
    if (atomic_cas_inline(&g_sym_inited, &expected, 1))
        SymInitialize(GetCurrentProcess(), NULL, TRUE);
}

static void mlw_format_frames(char* out, size_t out_cap, void* const* frames, int count)
{
    char sym_buf[sizeof(SYMBOL_INFO) + 256];
    PSYMBOL_INFO sym = (PSYMBOL_INFO)sym_buf;
    size_t used = 0;
    int i;

    if (count <= 0) { if (out_cap) out[0] = 0; return; }
    mlw_sym_ensure_init();

    for (i = 0; i < count && used < out_cap; i++)
    {
        DWORD64 disp = 0;
        int n;
        memset(sym_buf, 0, sizeof(sym_buf));
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 256;

        if (frames[i] && SymFromAddr(GetCurrentProcess(), (DWORD64)(uintptr_t)frames[i], &disp, sym))
            n = snprintf(out + used, out_cap - used, "%s%s+0x%llx",
                (i ? " <- " : ""), sym->Name, (unsigned long long)disp);
        else
            n = snprintf(out + used, out_cap - used, "%s0x%p", (i ? " <- " : ""), frames[i]);

        if (n <= 0) break;
        used += (size_t)n;
    }
}

/* ===========================================================================
 * Suspensao/varredura de raizes de UMA thread (uma por vez -- ver nota de
 * seguranca no .h: nunca todas suspensas ao mesmo tempo).
 * ===========================================================================*/
typedef struct MlwSpanTable
{
    MemSpanInfo* spans;
    boolean*     reachable;
    uint64       count;
}
MlwSpanTable;

#define MLW_SPAN_SIZE 65536u   /* == MEMOP_SPAN_SIZE em memory_pool.c (64KB) */

static int mlw_span_cmp(const void* a, const void* b)
{
    void* pa = ((const MemSpanInfo*)a)->base;
    void* pb = ((const MemSpanInfo*)b)->base;
    if (pa < pb) return -1;
    if (pa > pb) return 1;
    return 0;
}

/* Marca alcancavel qualquer span cujo range [base, base+64KB) contenha 'w'.
 * Busca binaria no array ordenado por base. Falso positivo (achar span onde
 * nao ha) e impossivel aqui -- range e exato; falso positivo de "isto e um
 * ponteiro de verdade" e o preco normal de varredura conservadora (ver .h). */
static void mlw_mark_candidate(MlwSpanTable* t, void* w)
{
    uint64 lo = 0, hi = t->count;
    if (!w) return;
    while (lo < hi)
    {
        uint64 mid = lo + (hi - lo) / 2;
        void* base = t->spans[mid].base;
        if (w < base) hi = mid;
        else if ((char*)w >= (char*)base + MLW_SPAN_SIZE) lo = mid + 1;
        else { t->reachable[mid] = true; return; }
    }
}

static void mlw_scan_thread_roots(HANDLE h, MlwSpanTable* t, void** out_rip_or_null)
{
    DWORD sus;
    CONTEXT ctx;
    MLW_THREAD_BASIC_INFORMATION tbi;
    NT_TIB* tib;
    void* rsp;
    void* base;
    ptrdiff_t span_bytes;
    void** p;

    if (out_rip_or_null) *out_rip_or_null = NULL;
    if (!mlw_resolve_ntdll()) return;

    sus = SuspendThread(h);
    if (sus == (DWORD)-1) return;   /* handle invalido/thread ja saiu: pula */

    memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(h, &ctx)) { ResumeThread(h); return; }

    memset(&tbi, 0, sizeof(tbi));
    if (g_NtQueryInformationThread(h, 0 /*ThreadBasicInformation*/,
            &tbi, sizeof(tbi), NULL) != 0 || !tbi.TebBaseAddress)
    {
        ResumeThread(h);
        return;
    }
    tib = (NT_TIB*)tbi.TebBaseAddress;

#if defined(_M_X64)
    rsp  = (void*)ctx.Rsp;
    base = tib->StackBase;
    if (out_rip_or_null) *out_rip_or_null = (void*)ctx.Rip;
    mlw_mark_candidate(t, (void*)ctx.Rax); mlw_mark_candidate(t, (void*)ctx.Rbx);
    mlw_mark_candidate(t, (void*)ctx.Rcx); mlw_mark_candidate(t, (void*)ctx.Rdx);
    mlw_mark_candidate(t, (void*)ctx.Rsi); mlw_mark_candidate(t, (void*)ctx.Rdi);
    mlw_mark_candidate(t, (void*)ctx.Rbp); mlw_mark_candidate(t, (void*)ctx.Rsp);
    mlw_mark_candidate(t, (void*)ctx.R8);  mlw_mark_candidate(t, (void*)ctx.R9);
    mlw_mark_candidate(t, (void*)ctx.R10); mlw_mark_candidate(t, (void*)ctx.R11);
    mlw_mark_candidate(t, (void*)ctx.R12); mlw_mark_candidate(t, (void*)ctx.R13);
    mlw_mark_candidate(t, (void*)ctx.R14); mlw_mark_candidate(t, (void*)ctx.R15);
#elif defined(_M_IX86)
    rsp  = (void*)(uintptr_t)ctx.Esp;
    base = tib->StackBase;
    if (out_rip_or_null) *out_rip_or_null = (void*)(uintptr_t)ctx.Eip;
    mlw_mark_candidate(t, (void*)(uintptr_t)ctx.Eax); mlw_mark_candidate(t, (void*)(uintptr_t)ctx.Ebx);
    mlw_mark_candidate(t, (void*)(uintptr_t)ctx.Ecx); mlw_mark_candidate(t, (void*)(uintptr_t)ctx.Edx);
    mlw_mark_candidate(t, (void*)(uintptr_t)ctx.Esi); mlw_mark_candidate(t, (void*)(uintptr_t)ctx.Edi);
    mlw_mark_candidate(t, (void*)(uintptr_t)ctx.Ebp); mlw_mark_candidate(t, (void*)(uintptr_t)ctx.Esp);
#else
    rsp = NULL; base = NULL;
#endif

    span_bytes = (char*)base - (char*)rsp;
    if (rsp && base && span_bytes > 0 && span_bytes < (64 * 1024 * 1024))
    {
        for (p = (void**)rsp; (char*)p < (char*)base; p++)
            mlw_mark_candidate(t, *p);
    }

    ResumeThread(h);
}
#endif /* XPLATBASE_WIN */

/* ===========================================================================
 * Enumeracao de threads: so copia o HANDLE (nao guarda Thread*, que pode ser
 * liberado por thread_join entre o snapshot e o uso -- ver .c de thread_handler).
 * ===========================================================================*/
typedef struct MlwHandleVec
{
    xthread_handle_t* items;
    uint32 count, cap;
}
MlwHandleVec;

static void mlw_handle_push(MlwHandleVec* v, xthread_handle_t h)
{
    if (v->count == v->cap)
    {
        uint32 nc = v->cap ? v->cap * 2 : 16;
        xthread_handle_t* ni = (xthread_handle_t*)realloc(v->items, nc * sizeof(xthread_handle_t));
        if (!ni) return;   /* falha de realloc: descarta silenciosamente esta thread do scan */
        v->items = ni; v->cap = nc;
    }
    v->items[v->count++] = h;
}

static void mlw_thread_cb(const Thread* thr, void* ctx)
{
    mlw_handle_push((MlwHandleVec*)ctx, thr->Thr);
}

/* ===========================================================================
 * Scan completo: snapshot -> suspende/varre uma thread por vez -> marca ->
 * resume -> (fora de qualquer suspensao) simboliza e reporta os nao-marcados.
 * ===========================================================================*/
static void mlw_run_scan(MlwLevel level)
{
#ifdef XPLATBASE_WIN
    MlwSpanTable t; memset(&t, 0, sizeof(t));
    MlwHandleVec hv; memset(&hv, 0, sizeof(hv));
    uint64 span_cap, span_n, i;
    DWORD self_tid = GetCurrentThreadId();
    void* rip_sample = NULL;
    boolean got_rip_sample = false;

    thread_mutex_lock_inline(&g_scan_lock);

    span_cap = memop_span_count() + 64;   /* folga p/ spans criados entre a contagem e a copia */
    t.spans = (MemSpanInfo*)malloc((size_t)span_cap * sizeof(MemSpanInfo));
    if (!t.spans) { thread_mutex_unlock_inline(&g_scan_lock); return; }
    span_n = memop_snapshot_spans(t.spans, span_cap);
    t.count = span_n;

    t.reachable = (boolean*)calloc((size_t)span_n ? (size_t)span_n : 1, sizeof(boolean));
    if (!t.reachable) { free(t.spans); thread_mutex_unlock_inline(&g_scan_lock); return; }

    qsort(t.spans, (size_t)span_n, sizeof(MemSpanInfo), mlw_span_cmp);

    thread_enum(mlw_thread_cb, &hv);

    for (i = 0; i < hv.count; i++)
    {
        HANDLE h = hv.items[i];
        void* rip = NULL;
        if (GetThreadId(h) == self_tid) continue;   /* nao suspende a si mesma */
        mlw_scan_thread_roots(h, &t, &rip);
        if (level == MLW_CRITICAL && !got_rip_sample && rip) { rip_sample = rip; got_rip_sample = true; }
    }
    free(hv.items);

    /* Daqui pra baixo nenhuma thread esta suspensa: seguro alocar/chamar dbghelp. */
    {
        uint64 leaked_n = 0, leaked_bytes = 0;
        char site_buf[512];

        for (i = 0; i < span_n; i++)
        {
            MemSpanInfo* s = &t.spans[i];
            uint64 size, age_ms, now_ms;
            if (t.reachable[i] || s->used == 0) continue;

            size = (uint64)s->stride * s->used;
            now_ms = (uint64)GetTickCount64();
            age_ms = (s->created_ms && now_ms > s->created_ms) ? (now_ms - s->created_ms) : 0;

            mlw_format_frames(site_buf, sizeof(site_buf), s->site_frames, s->site_frame_count);
            mlw_log(MLW_WARN, "leak_candidate", s->base, size, s->used, age_ms, site_buf);
            leaked_n++; leaked_bytes += size;
        }

        if (level == MLW_CRITICAL)
        {
            char rip_buf[256] = "";
            if (got_rip_sample) mlw_format_frames(rip_buf, sizeof(rip_buf), &rip_sample, 1);
            mlw_log(MLW_CRITICAL, "near_memory_limit", NULL, leaked_bytes, (uint32)leaked_n, 0, rip_buf);
        }
        else if (level == MLW_WARN && leaked_n == 0)
        {
            mlw_log(MLW_WARN, "scan_clean", NULL, 0, 0, 0, "nenhum candidato a vazamento nesta varredura");
        }
    }

    free(t.spans);
    free(t.reachable);
    thread_mutex_unlock_inline(&g_scan_lock);
#else
    (void)level;
    fprintf(stderr, "[mem_leak_watch] scan so implementado em XPLATBASE_WIN nesta versao\n");
#endif
}

void mem_leak_watch_scan_now(void)
{
    mlw_run_scan(MLW_WARN);
}

/* ===========================================================================
 * Timer (camada barata): so le memop_get_stats(), sem suspender thread
 * nenhuma. So escalona pro scan caro quando cruza um dos limiares.
 * ===========================================================================*/
#ifdef XPLATBASE_WIN
static DWORD WINAPI mlw_monitor_loop(void* arg)
#else
static void* mlw_monitor_loop(void* arg)
#endif
{
    (void)arg;
    for (;;)
    {
        if (!atomic_get_inline(&g_running)) break;

        thread_wait_prepare_inline(&g_wait);
        thread_wait_sleep_for_inline(&g_wait, (long long)g_cfg.interval_ms * 1000);

        if (!atomic_get_inline(&g_running)) break;

        {
            MemPoolStats st;
            memop_get_stats(&st);
            if (st.os_reserved_bytes >= g_cfg.crit_threshold_bytes)
                mlw_run_scan(MLW_CRITICAL);
            else if (st.os_reserved_bytes >= g_cfg.warn_threshold_bytes)
                mlw_run_scan(MLW_WARN);
        }
    }
#ifdef XPLATBASE_WIN
    return 0;
#else
    return NULL;
#endif
}

/* ===========================================================================
 * Start/stop
 * ===========================================================================*/
boolean mem_leak_watch_start(const MemLeakWatchConfig* cfg)
{
    MemLeakWatchConfig def;
    int expected = 0;
    int status = 0;

    if (!atomic_cas_inline(&g_running, &expected, 1))
        return false;   /* ja rodando */

    mem_leak_watch_default_config(&def);
    if (cfg)
    {
        g_cfg = *cfg;
        /* campos zerados no cfg do chamador = "usa o default" */
        if (g_cfg.interval_ms == 0)          g_cfg.interval_ms = def.interval_ms;
        if (g_cfg.warn_threshold_bytes == 0) g_cfg.warn_threshold_bytes = def.warn_threshold_bytes;
        if (g_cfg.crit_threshold_bytes == 0) g_cfg.crit_threshold_bytes = def.crit_threshold_bytes;
        if (!g_cfg.log_path)                 g_cfg.log_path = def.log_path;
    }
    else
    {
        g_cfg = def;
    }

    thread_mutex_init_inline(&g_scan_lock);
    thread_mutex_init_inline(&g_log_lock);

    memop_leak_watch_enable(g_cfg.enabled);
    if (!g_cfg.enabled) { atomic_set_inline(&g_running, 0); return false; }

    thread_wait_init(false);
    thread_wait_prepare_inline(&g_wait);

    g_thread = thread_create(mlw_monitor_loop, NULL, &status);
    if (!status)
    {
        atomic_set_inline(&g_running, 0);
        thread_wait_destroy_inline(&g_wait);
        thread_wait_shutdown();
        memop_leak_watch_enable(false);
        return false;
    }
    return true;
}

void mem_leak_watch_stop(void)
{
    int expected = 1;
    if (!atomic_cas_inline(&g_running, &expected, 0))
        return;   /* nao estava rodando */

    thread_wait_wake_inline(&g_wait);
    thread_join(&g_thread);
    thread_wait_destroy_inline(&g_wait);
    thread_wait_shutdown();
    memop_leak_watch_enable(false);

    thread_mutex_lock_inline(&g_log_lock);
    if (g_log_file) { fclose(g_log_file); g_log_file = NULL; }
    thread_mutex_unlock_inline(&g_log_lock);
}
