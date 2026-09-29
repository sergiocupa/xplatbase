//  Nucleo dos testes, em C (mesmo modelo do appserver/test/unittests).
//
//  A LOGICA de todo teste vive em arquivos .c, compilados como C, iguais a biblioteca que
//  eles testam -- mesmos tipos, mesmas convencoes, acesso direto as funcoes internas.
//  O unico C++ do projeto e o registro (registro.cpp): uma linha por teste, sem logica,
//  so para o Gerenciador de Testes do Visual Studio listar caso a caso.
//
//  Um teste e uma funcao 'void f(TestResult*)'. Ela comeca chamando t_start e usa T_ASSERT;
//  a primeira falha encerra a funcao com a mensagem, arquivo e linha.

#ifndef CTEST_CORE_H
#define CTEST_CORE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    int  ok;
    int  skipped;     // 1 = nao verificou nada (faltou recurso, versao de referencia, ...)
    int  checks;      // T_CHECK executados
    int  falhas;      // T_CHECK que falharam (a 1a vai para msg; todas para a saida)
    char msg[512];
}
TestResult;

void t_start(TestResult* r);
void t_failf(TestResult* r, const char* file, int line, const char* fmt, ...);

// Marca o teste como PULADO, com o motivo: verde calado nao diz o que foi exercitado.
void t_skipf(TestResult* r, const char* fmt, ...);
#define T_SKIP(r, ...) do { t_skipf((r), __VA_ARGS__); return; } while (0)

// Sempre com mensagem: quando falha, ela e a unica coisa que o Gerenciador mostra.
#define T_ASSERT(r, cond, ...)                                  \
    do {                                                        \
        if (!(cond)) {                                          \
            t_failf((r), __FILE__, __LINE__, __VA_ARGS__);      \
            return;                                             \
        }                                                       \
    } while (0)

// Checagem que NAO encerra o teste: registra a falha e segue. Para testes em matriz (centenas
// de combinacoes), em que ver TODAS as que falharam vale mais que parar na primeira. A 1a
// falha vira a mensagem do Gerenciador; todas vao para a saida do teste.
void t_checkf(TestResult* r, int cond, const char* file, int line, const char* fmt, ...);
#define T_CHECK(r, cond, ...) t_checkf((r), (cond) ? 1 : 0, __FILE__, __LINE__, __VA_ARGS__)

// Relatorio do teste (tabelas de benchmark, detalhes). Vai para o stdout e, quando o
// registro instala o destino, para a SAIDA do teste no Gerenciador -- o printf sozinho
// nao aparece la.
typedef void (*TLogDestino)(const char* linha);
void t_log_destino(TLogDestino destino);
void t_logf(const char* fmt, ...);

#ifdef __cplusplus
}
#endif

#endif
