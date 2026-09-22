//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  xplat_instance: garante que existe UMA SO instancia do xplatbase no processo.
//
//  Por que isto e necessario: o pool de memoria, o registro de threads e o pool de tarefas
//  sao estado GLOBAL DE ARQUIVO (static). Cada modulo que linka o xplatbase estaticamente
//  ganha a propria copia -- e ai a contabilidade racha, o mem_leak_watch enxerga so um lado,
//  e liberar memoria cruzando a fronteira vira corrupcao SILENCIOSA. Com codec em DLL isso
//  deixa de ser hipotese.
//
//  Como se detecta: uma variavel global nao serve de deteccao, porque ela e exatamente o que
//  duplica. Precisa de algo que as duas copias enxerguem:
//    - Windows: mapeamento de arquivo NOMEADO, por processo (objeto do kernel, morre junto
//      com o processo -- nao ha resto de execucao anterior).
//    - POSIX:   arquivo por processo + flock. O lock cai sozinho quando o processo morre,
//      entao arquivo esquecido de uma execucao passada nao produz falso positivo.
//
//  Quem chama: memop_init(), que e o caminho obrigatorio de qualquer alocacao.

#ifndef _WIN32
  #define _GNU_SOURCE   /* dladdr/Dl_info: precisa vir ANTES de qualquer include */
#endif
#include "../include/xplatbase.h"
#include "memory_pool.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>   // abort

#ifndef XPLATBASE_WIN
  #include <sys/file.h>
  #include <sys/stat.h>
  #include <fcntl.h>
  #include <unistd.h>
  #include <dlfcn.h>
#endif

#define XPLAT_INSTANCE_ABI 1u

// Registro compartilhado entre as copias. Layout fixo: e lido por uma copia que pode ter sido
// compilada em outro momento, entao NAO mudar sem subir o ABI acima.
typedef struct
{
    uint32 Abi;
    uint64 Identity;        // endereco do marcador da copia que registrou primeiro
    char   Module[260];     // de onde ela veio (exe ou dll)
}
XplatInstanceRec;

// Marcador de identidade: cada copia da biblioteca tem o SEU, em endereco proprio. E o unico
// jeito confiavel de dizer "esta copia e diferente daquela".
static const char g_self_marker = 0;

static char g_my_module[260];
static XplatInstanceInfo g_first;      // quem chegou primeiro (preenchido so na duplicata)
static int  g_duplicate;               // 1 = ha outra instancia no processo
static int  g_registered;
static int  g_fatal =                  // duplicata aborta? padrao: sim em Debug
#ifdef _DEBUG
    1;
#else
    0;
#endif

#ifdef XPLATBASE_WIN
// Handles mantidos ABERTOS de proposito pela vida do processo: eles seguram o objeto nomeado
// vivo. Fechar aqui apagaria o registro e a segunda instancia passaria despercebida.
static HANDLE g_map, g_lock;
#else
static int g_fd = -1;
static char g_path[128];
#endif

// ---- caminho do modulo desta copia -----------------------------------------
static void instance_fill_module(void)
{
    g_my_module[0] = '\0';
#ifdef XPLATBASE_WIN
    {
        HMODULE h = 0;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)&g_self_marker, &h) && h)
            GetModuleFileNameA(h, g_my_module, (DWORD)sizeof(g_my_module));
    }
#else
    {
        Dl_info info;
        if (dladdr((void*)&g_self_marker, &info) && info.dli_fname)
            snprintf(g_my_module, sizeof(g_my_module), "%s", info.dli_fname);
    }
#endif
    if (g_my_module[0] == '\0') snprintf(g_my_module, sizeof(g_my_module), "%s", "(modulo desconhecido)");
}

static void instance_report(void)
{
    fprintf(stderr,
        "\n=== xplatbase: SEGUNDA INSTANCIA NO MESMO PROCESSO ===\n"
        "  primeira : %s\n"
        "  esta     : %s\n"
        "  Cada copia tem o proprio pool de memoria e o proprio registro de threads.\n"
        "  Liberar memoria entre elas corrompe o heap. Linke o xplatbase COMPARTILHADO\n"
        "  (XPLATBASE_USE_SHARED) em todos os modulos, ou nao linke em mais de um.\n"
        "======================================================\n\n",
        g_first.Module, g_my_module);
    fflush(stderr);
}

// ---- registro por plataforma ------------------------------------------------
#ifdef XPLATBASE_WIN
static void instance_register_os(void)
{
    char name[160], lock[160];
    unsigned long pid = (unsigned long)GetCurrentProcessId();
    XplatInstanceRec* rec;
    int existed;

    sprintf_s(name, sizeof(name), "Local\\xplatbase.instance.%lu",      pid);
    sprintf_s(lock, sizeof(lock), "Local\\xplatbase.instance.lock.%lu", pid);

    // Serializa criacao e leitura: sem isto, duas copias subindo ao mesmo tempo poderiam ler
    // o registro antes de ele ser preenchido.
    g_lock = CreateMutexA(NULL, FALSE, lock);
    if (g_lock) WaitForSingleObject(g_lock, INFINITE);

    g_map = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(XplatInstanceRec), name);
    existed = (g_map != NULL && GetLastError() == ERROR_ALREADY_EXISTS);

    if (g_map)
    {
        rec = (XplatInstanceRec*)MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(XplatInstanceRec));
        if (rec)
        {
            if (!existed)
            {
                rec->Abi      = XPLAT_INSTANCE_ABI;
                rec->Identity = (uint64)(uintptr_t)&g_self_marker;
                memcpy(rec->Module, g_my_module, sizeof(rec->Module));
            }
            else if (rec->Identity != (uint64)(uintptr_t)&g_self_marker)
            {
                g_duplicate         = 1;
                g_first.AbiVersion  = rec->Abi;
                g_first.Identity    = rec->Identity;
                memcpy(g_first.Module, rec->Module, sizeof(g_first.Module));
            }
            UnmapViewOfFile(rec);
        }
    }

    if (g_lock) ReleaseMutex(g_lock);
}

static void instance_release_os(void)
{
    if (g_map)  { CloseHandle(g_map);  g_map  = NULL; }
    if (g_lock) { CloseHandle(g_lock); g_lock = NULL; }
}
#else
static void instance_register_os(void)
{
    XplatInstanceRec rec;
    const char* tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    snprintf(g_path, sizeof(g_path), "%s/.xplatbase.instance.%d", tmp, (int)getpid());

    g_fd = open(g_path, O_CREAT | O_RDWR, 0600);
    if (g_fd < 0) return;   // sem deteccao possivel; nao e motivo para derrubar o processo

    // O lock e o teste: quem consegue e o primeiro. Arquivo esquecido de execucao anterior
    // nao engana, porque o lock morre com o processo que o tinha.
    if (flock(g_fd, LOCK_EX | LOCK_NB) == 0)
    {
        memset(&rec, 0, sizeof(rec));
        rec.Abi      = XPLAT_INSTANCE_ABI;
        rec.Identity = (uint64)(uintptr_t)&g_self_marker;
        snprintf(rec.Module, sizeof(rec.Module), "%s", g_my_module);
        if (ftruncate(g_fd, 0) == 0) { ssize_t w = write(g_fd, &rec, sizeof(rec)); (void)w; }
        return;   // mantem o fd aberto: e ele que segura o lock
    }

    // Nao conseguiu o lock: ja existe instancia viva neste processo.
    if (read(g_fd, &rec, sizeof(rec)) == (ssize_t)sizeof(rec) &&
        rec.Identity != (uint64)(uintptr_t)&g_self_marker)
    {
        g_duplicate        = 1;
        g_first.AbiVersion = rec.Abi;
        g_first.Identity   = rec.Identity;
        snprintf(g_first.Module, sizeof(g_first.Module), "%s", rec.Module);
    }
    close(g_fd);
    g_fd = -1;
}

static void instance_release_os(void)
{
    if (g_fd >= 0)
    {
        flock(g_fd, LOCK_UN);
        close(g_fd);
        unlink(g_path);
        g_fd = -1;
    }
}
#endif

// ---- API --------------------------------------------------------------------

// A politica precisa ser decidivel ANTES da carga do modulo: o xplatbase se inicializa
// sozinho (inicializador de CRT no Windows, constructor no POSIX), entao quando a duplicata
// e detectada ninguem teve chance de chamar xplat_instance_set_fatal. A variavel de ambiente
// XPLATBASE_DUPLICATE_FATAL (0/1) resolve isso -- e e o que permite ao teste PROVOCAR a
// duplicata de proposito e observar a deteccao, em vez de morrer na carga.
static void instance_read_policy(void)
{
#ifdef XPLATBASE_WIN
    char* v = NULL; size_t n = 0;
    if (_dupenv_s(&v, &n, "XPLATBASE_DUPLICATE_FATAL") == 0 && v)
    {
        if (v[0] == '0' || v[0] == '1') g_fatal = (v[0] == '1');
        free(v);
    }
#else
    const char* v = getenv("XPLATBASE_DUPLICATE_FATAL");
    if (v && (v[0] == '0' || v[0] == '1')) g_fatal = (v[0] == '1');
#endif
}

void xplat_instance_register(void)
{
    if (g_registered) return;
    g_registered = 1;

    instance_read_policy();
    instance_fill_module();
    g_first.Identity = (uint64)(uintptr_t)&g_self_marker;
    snprintf(g_first.Module, sizeof(g_first.Module), "%s", g_my_module);

    instance_register_os();

    if (g_duplicate)
    {
        instance_report();
        // Duplicata e defeito de BUILD, nao condicao de execucao: em Debug para na hora, onde
        // a pilha ainda mostra quem carregou o segundo modulo. O teste que provoca a duplicata
        // de proposito desliga isto antes (xplat_instance_set_fatal).
        if (g_fatal) abort();
    }
}

void xplat_instance_release(void)
{
    instance_release_os();
    g_registered = 0;
}

int xplat_instance_check(XplatInstanceInfo* first)
{
    xplat_instance_register();
    if (first) *first = g_first;
    return g_duplicate;
}

const char* xplat_instance_module(void)
{
    if (!g_registered) instance_fill_module();
    return g_my_module;
}

void xplat_instance_set_fatal(int fatal)
{
    g_fatal = fatal ? 1 : 0;
}
