//  Registro dos testes no Gerenciador de Testes do Visual Studio.
//
//  Este e o UNICO arquivo C++ de registro, e de proposito: ele nao tem logica de teste
//  nenhuma. Cada entrada e uma linha que chama a funcao correspondente e reporta o
//  resultado. A logica esta nos .c (e no bench_pool_bibliotecas.cpp, que precisa do TBB).
//
//  O framework (CppUnitTest) ja vem instalado com o Visual Studio e o adaptador dele e
//  embutido -- nao ha dependencia externa para baixar.
//
//  BENCHMARKS: PULADOS POR PADRAO (aparecem como "ignorados" no Gerenciador). Levam minutos
//  e medem desempenho, que nao e o que um "Executar Todos" quer. Para roda-los, compile com
//  a propriedade XpbBench=true -- por exemplo, a variavel de ambiente XpbBench=true antes de
//  abrir o Visual Studio (o MSBuild le variaveis de ambiente como propriedades), ou
//  msbuild ... -p:XpbBench=true. Isso define XPB_BENCH_ATIVO.

#include "CppUnitTest.h"
#include <string>

extern "C" {
#include "testes.h"
}

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace
{
    // A mensagem de falha vem em char* do lado C; o framework espera wchar_t*.
    std::wstring largura(const char* s)
    {
        std::string texto(s ? s : "");
        return std::wstring(texto.begin(), texto.end());
    }

    // O relatorio dos testes (t_logf) vai para a SAIDA do teste no Gerenciador.
    void para_o_gerenciador(const char* linha)
    {
        Logger::WriteMessage(linha);
    }

    void executa(void (*teste)(TestResult*))
    {
        TestResult r;
        t_log_destino(para_o_gerenciador);
        teste(&r);
        t_log_destino(nullptr);
        // Pulado aparece na saida do teste: verde silencioso nao diz o que foi exercitado.
        if (r.ok && r.skipped) Logger::WriteMessage((L"PULADO: " + largura(r.msg)).c_str());
        std::string msg = r.msg;
        // Checagens que seguem apos falhar (T_CHECK): a 1a e a mensagem; o total vai junto.
        if (r.falhas > 1)
            msg += " (+" + std::to_string(r.falhas - 1) + " outras falhas de " + std::to_string(r.checks) + " checagens; ver a saida)";
        Assert::IsTrue(r.ok != 0, largura(msg.c_str()).c_str());
    }
}

// Uma linha por teste. O nome aqui e o que aparece no Gerenciador.
#define CASO(nome, funcao) TEST_METHOD(nome) { executa(funcao); }

// Benchmark: igual ao CASO, mas ignorado a menos que o build defina XPB_BENCH_ATIVO.
#ifdef XPB_BENCH_ATIVO
  #define BENCH(nome, funcao) CASO(nome, funcao)
#else
  #define BENCH(nome, funcao)                               \
      BEGIN_TEST_METHOD_ATTRIBUTE(nome)                     \
          TEST_IGNORE()                                     \
      END_TEST_METHOD_ATTRIBUTE()                           \
      CASO(nome, funcao)
#endif

namespace Xplatbase
{
TEST_CLASS(PoolDeTarefas)
{
public:
    CASO (VigiaLigaEDesliga,       teste_pool_vigia_liga_e_desliga)
    CASO (PerfilEconomiaEPerformance, teste_pool_perfil_economia_e_performance)
    CASO (RajadasIntermitentesNaoGiram, teste_pool_rajadas_intermitentes_nao_giram)
    CASO (ParadoEconomiaAcordaPouco, teste_pool_parado_economia_acorda_pouco)
    CASO (TimerSoltoNoEconomiaParado, teste_pool_timer_solto_no_economia_parado)
    BENCH(BenchAntesXDepois,       teste_pool_bench_antes_depois)
    BENCH(BenchContraTbbEWinTP,    teste_pool_bench_bibliotecas)
    BENCH(TimerDoProcesso,         teste_pool_timer_do_processo)
};

TEST_CLASS(PoolDeMemoria)
{
public:
    CASO (AllocFreeBasico,         teste_memoria_alloc_free_basico)
    CASO (ClassesDeTamanho,        teste_memoria_classes_de_tamanho)
    CASO (LanePorThread,           teste_memoria_lane_por_thread)
    CASO (CrescimentoPorSpans,     teste_memoria_crescimento_por_spans)
    CASO (FreeRemotoReativaSpans,  teste_memoria_free_remoto_reativa)
    CASO (VazamentoSemColecao,     teste_memoria_vazamento_sem_colecao)
    BENCH(BenchAlocadores,         teste_memoria_bench_alocadores)
};

TEST_CLASS(Strings)
{
public:
    CASO (InitCreateRelease,       teste_string_init_create_release)
    CASO (Append,                  teste_string_append)
    CASO (Copy,                    teste_string_copy)
    CASO (EqualMatriz,             teste_string_equal_matriz)
    CASO (EqualPartMatriz,         teste_string_equal_part_matriz)
    CASO (IndexOfCharMatriz,       teste_string_indexof_char_matriz)
    CASO (IndexOfsMatriz,          teste_string_indexofs_matriz)
    CASO (SubstringMatriz,         teste_string_substring_matriz)
    CASO (Trim,                    teste_string_trim)
    CASO (Stop,                    teste_string_stop)
    CASO (SplitMatriz,             teste_string_split_matriz)
    CASO (AppendFormat,            teste_string_append_format)
};
}   // namespace Xplatbase
