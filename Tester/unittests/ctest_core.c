#include "ctest_core.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static TLogDestino g_destino = 0;

void t_start(TestResult* r)
{
    if (!r) return;
    r->ok = 1;
    r->skipped = 0;
    r->checks = 0;
    r->falhas = 0;
    r->msg[0] = '\0';
}

void t_checkf(TestResult* r, int cond, const char* file, int line, const char* fmt, ...)
{
    char detalhe[384];
    va_list ap;
    const char* base;

    if (!r) return;
    r->checks++;
    if (cond) return;

    va_start(ap, fmt);
    vsnprintf(detalhe, sizeof(detalhe), fmt, ap);
    va_end(ap);
    base = strrchr(file, '\\');
    if (!base) base = strrchr(file, '/');
    base = base ? base + 1 : file;

    r->falhas++;
    t_logf("  [FALHA] %s(%d): %s\n", base, line, detalhe);
    if (r->ok)
    {
        r->ok = 0;
        snprintf(r->msg, sizeof(r->msg), "%s(%d): %s", base, line, detalhe);
    }
}

void t_failf(TestResult* r, const char* file, int line, const char* fmt, ...)
{
    char detalhe[384];
    va_list ap;
    const char* base;

    if (!r) return;
    va_start(ap, fmt);
    vsnprintf(detalhe, sizeof(detalhe), fmt, ap);
    va_end(ap);

    // So o nome do arquivo: o caminho inteiro empurra a mensagem para fora da tela.
    base = strrchr(file, '\\');
    if (!base) base = strrchr(file, '/');
    base = base ? base + 1 : file;

    r->ok = 0;
    snprintf(r->msg, sizeof(r->msg), "%s(%d): %s", base, line, detalhe);
}

void t_skipf(TestResult* r, const char* fmt, ...)
{
    va_list ap;
    if (!r) return;
    va_start(ap, fmt);
    vsnprintf(r->msg, sizeof(r->msg), fmt, ap);
    va_end(ap);
    r->ok = 1;          // pulado nao e falha
    r->skipped = 1;
}

void t_log_destino(TLogDestino destino)
{
    g_destino = destino;
}

void t_logf(const char* fmt, ...)
{
    char linha[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(linha, sizeof(linha), fmt, ap);
    va_end(ap);

    // Um destino so: o executor do VS tambem captura o stdout, e os dois juntos
    // duplicavam cada linha na saida do teste.
    if (g_destino) { g_destino(linha); return; }
    fputs(linha, stdout);
    fflush(stdout);
}
