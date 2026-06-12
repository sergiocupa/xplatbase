# Testes de manifestacao do thread pool

Esta pasta contem a evidencia anterior às correcoes. A expectativa dos testes
P e encontrar o defeito no codigo original:

- `[MANIFESTOU]` significa que a evidencia esperada foi observada.
- `[NAO MANIFESTOU]` significa que o teste nao comprovou o ponto.
- exit code zero significa que todos os casos executados manifestaram.

## Windows

Compile primeiro a solucao com os hooks preexistentes usados pelo teste
deterministico:

```powershell
msbuild Xplatbase\Xplatbase.sln /m /p:Configuration=Release /p:Platform=x64 /p:ThreadPoolTestDefines=POOL_TEST_HOOKS
```

Execucao normal:

```powershell
.\Tester\thread_pool_audit\run_p_tests.ps1
```

Execucao completa, incluindo P02 e P17 em processos filhos:

```powershell
.\Tester\thread_pool_audit\run_p_tests.ps1 -IncludeCrashTests
```

Os processos filhos usam `SEM_NOGPFAULTERRORBOX`; uma falha de acesso nesses
filhos e evidencia esperada, sem caixa de dialogo.

## Tipos de evidencia

- Dinamica Windows: P01, P02, P03, P04, P08, P12, P13, P14, P16, P17 e P18.
- Contratual/estatica: P05, P06, P07, P09, P11, P13, P15, P17, P19, P20 e P21.
- Dinamica Linux: P05, P06 e P07, executada separadamente.

P20 e P21 ficam como evidencia estatica nesta versao congelada. Torná-los
deterministicos em runtime exigiria instrumentar a biblioteca original, o que
nao faz parte deste commit de testes.

## Linux

Conforme combinado, nao roda por padrao:

```powershell
.\Tester\thread_pool_audit\run_p_tests.ps1 -IncludeLinuxTests
```
