# Xplatbase

Biblioteca em **C (C11)** com um conjunto de tipos base e funções fundamentais
para uso **multiplataforma** (Windows / Linux, x86-64 e ARM). Centraliza
alocação de memória, tratamento de falhas, criação e sincronização de threads,
estruturas lock-free e um pool de tarefas de alto desempenho.

O objetivo é ser a camada de base sobre a qual outros componentes são
construídos: sem dependências externas, com auto-inicialização no carregamento
do módulo (`.CRT$XCU` no Windows, `__attribute__((constructor))` no Linux) e
API estável exportada via `XPLATBASE_API`.

> Licença: MIT com atribuição obrigatória — © 2025 Sergio Paludo
> ([github.com/sergiocupa](https://github.com/sergiocupa)).

---

## Destaques

Três componentes concentram o trabalho de engenharia da biblioteca e têm
**benchmark completo** contra referências do mercado:

| Componente | O que é | Referência comparada |
|---|---|---|
| [**Thread Pool**](#-thread-pool) | Pool de tarefas work-stealing com core/reserva e workers elásticos | Intel TBB, Windows Thread Pool |
| [**Memory Pool**](#-memory-pool) | Alocador por *size-class* com lanes por thread e cache global | rpmalloc, mimalloc, CRT malloc |
| [**Mem Leak Watch**](#-mem-leak-watch) | Monitor de vazamento por alcançabilidade (estilo GC, sem coletar) | — (custo medido sobre o memory pool) |

---

## 🧵 Thread Pool

`src/thread_pool.h` — pool de tarefas *run-to-completion* com escalonamento
**work-stealing** em estilo arena (design consolidado V2.05):

- **Submit externo** vai para *G* filas MPMC compartilhadas (`shards = cores/4`),
  em round-robin. Qualquer worker puxa de qualquer shard — a tarefa não fica
  presa a um dono. Fila em ring **Vyukov** tipado inline; consumidores reservam
  até `POOL_BATCH` tarefas por CAS.
- **Spawn** (submit reentrante, de dentro de uma tarefa) vai para o slot **LIFO
  não-roubável** do worker (cache quente), com overflow para um deque
  **Chase-Lev local** (push/take sem CAS). Steal entre deques.
- **Core / reserva**: `n_core = cores*7/10` workers giram (spin) e são acordados
  pelo submit; os demais ficam *park-first* e só engajam sob backlog → CPU plano
  em ~75% com cauda baixa. Spawn usa todos os cores.
- **Worker elástico**: um monitor detecta workers presos em tarefas longas e,
  havendo backlog, acorda workers extras para drenar as tarefas curtas — eles se
  aposentam quando a carga passa. Protege a latência das curtas no perfil
  “rápida pode virar lenta”.

### API

```c
#include "thread_pool.h"

boolean pool_submit(pool_task_fn fn, void* arg);   // enfileira tarefa
void    pool_wait_idle(void);                      // espera drenar
void    pool_dims(int* workers, int* core);        // dimensões atuais

// variante com pool próprio (cores_override relativo)
ThreadPool* pool_create_relative(int cores_override);
void        pool_destroy_relative(ThreadPool* p);
boolean     pool_submit_relative(ThreadPool* p, pool_task_fn fn, void* arg);
```

Tunáveis por `-D`: `POOL_CORE_NUM/DEN` (7/10), `POOL_ELASTIC_NUM/DEN`,
`POOL_SHARD_DIV` (4), `POOL_SHARD_CAP`, `POOL_DEQUE_CAP`, `POOL_MON_MS` (5),
`POOL_BATCH` (2), `POOL_LIFO_CAP` (8).

### Benchmark

16 CPUs, 1M tarefas por cenário, **mediana de 10 repetições**, contra
**Intel TBB** e **Windows Thread Pool**.

![Vazão do thread pool](docs/img/tp_throughput.svg)

Vazão consistentemente **~2,1×–2,7× acima do TBB** e **~3,7×–4,4× acima do
Windows TP** no submit externo; em *spawn* recursivo empata com o TBB (~1,04×) e
supera o Windows TP em ~9,6×.

![Latência de cauda p99](docs/img/tp_tail_latency.svg)

O diferencial mais forte é a **cauda**: no submit externo o p99 fica em
sub-microsegundo (~0,8 µs) — cerca de **10× melhor que o TBB** (~7–9 µs) e
**40×–90× melhor que o Windows TP** (33–77 µs). O produtor só enfileira e os
workers se auto-servem em regime quente.

> **Nota sobre `mempool/media`:** é o cenário de tarefas mais curtas (cada tarefa
> faz um alloc/free do memory pool), onde a vantagem de *dispatch* do pool é
> máxima — daí a maior vazão relativa (~4,4× vs Windows TP). Duas ressalvas de
> honestidade: (1) as colunas `cpu%`/`cores` desse cenário saem como `0` — é
> artefato da granularidade (~15,6 ms) do contador de tempo-de-CPU do Windows,
> já que o run dura ~10 ms; a **vazão em wall-clock é estável** (10 reps entre
> 4,9 e 6,4 Mtask/s). (2) Em troca da vazão, esse cenário tem a **pior cauda** do
> pool (p99 ~234 µs), por isso ele fica de fora do gráfico de p99 acima.

Dados brutos: [`Tester/thread_pool/`](Tester/thread_pool/) — logs
`bench_run_latest.log` e TSV `thread_pool_bench_results*.tsv` (fonte:
`thread_pool_bench.cpp`; execute `thread_pool_bench.exe 10 1000000` para 10 reps).

---

## 🧠 Memory Pool

`src/memory_pool.h` — alocador de propósito geral por **size-class**, com
**lanes por thread** (fast path sem lock), refil de chunks de 64 KB a partir de
segmentos de 4 MB reservados do SO, **cache global** de chunks livres e **purga
automática** (com histerese) devolvendo segmentos ociosos ao SO.

- Fast path de ponteiro cru — `void*` em registrador, sem carregar o tamanho de
  volta:

```c
void  memop_init(void);
void  memop_shutdown(void);

void* memop_alloc_raw(uint64 size);     // hot path
void  memop_free_raw(void* ptr);

MemBuffer memop_alloc(uint64 size);     // variante com tamanho embutido
void      memop_free(MemBuffer* buf);

void memop_purge(void);                 // trim explícito (app-driven)
void memop_get_stats(MemPoolStats* out);
```

- Integra-se ao `thread_handler`: `memop_on_created_thread` /
  `memop_on_ended_thread` gerenciam o ciclo de vida das lanes por thread.
- Blocos `> 16 KB` (LARGE) hoje usam fallback para `malloc` (pendência de
  design documentada).
- Expõe *snapshot* de spans (`memop_snapshot_spans`) para o
  [mem_leak_watch](#-mem-leak-watch), sem acoplar o layout interno.

### Benchmark

16 CPUs / 8 threads, **mediana de 9 execuções**, contra **CRT malloc**,
**rpmalloc** e **mimalloc**. As variantes `memop-lw-*` são o mesmo alocador com o
[mem_leak_watch](#-mem-leak-watch) ligado por cima (intervalo curto `dbg` vs.
longo `prod`).

![Larson churn — vazão](docs/img/mp_larson.svg)

Sob *churn* multi-thread (Larson) é um **empate técnico**: todos os alocadores
ficam em ~132–140 Mop/s. A variância entre execuções é alta (o memop, por
exemplo, oscilou entre 107 e 149 Mop/s nas 9 rodadas), então uma execução única
não distingue os alocadores aqui — daí as medianas.

![64B fixo — custo por operação](docs/img/mp_smallfixed.svg)

No hot path de 64B fixo, alloc/free custam **~5 ns** por operação, competitivo
com os melhores alocadores e claramente à frente do mimalloc no `free` (~8,4 ns,
o único desvio estável do conjunto).

![Latência por chamada, working-set quente](docs/img/mp_latency.svg)

**Custo do mem_leak_watch:** as variantes `memop-lw-dbg`/`memop-lw-prod` ficam
praticamente sobrepostas ao `memop-pool` puro em todos os cenários — o monitor
**não onera o hot path** de forma mensurável (a diferença cabe dentro da
variância entre execuções).

Dados brutos: [`bench/mempool_bench_latest.log`](bench/mempool_bench_latest.log)
(9 execuções concatenadas + tabela de medianas ao final) e
[`bench/mempool_bench_medians.json`](bench/mempool_bench_medians.json). Fonte:
`bench/bench.c` — cenários A = Larson churn, B = 64B fixo, C = latência por
chamada. O harness que roda e agrega está descrito em [Reproduzir os
benchmarks](#reproduzir-os-benchmarks).

---

## 🔎 Mem Leak Watch

`src/mem_leak_watch.h` — monitor de vazamento **sem coleta**, sobre o
memory pool. Faz o mesmo rastreamento de alcançabilidade que um GC usaria para
decidir o que liberar (raízes → ponteiros alcançados → spans marcados), mas
**nunca libera nada**: o que não foi alcançado vira um aviso em log, com o
mini-backtrace de onde o span foi criado.

Duas camadas de custo:

- **check barato** (timer): só lê `memop_get_stats()`, sem suspender thread
  nenhuma. Roda com frequência alta sem problema.
- **scan caro** (só quando o check acusa risco): suspende cada thread
  registrada, uma de cada vez, lê registradores + pilha como raízes, percorre os
  spans do pool e reporta o que sobrou.

```c
MemLeakWatchConfig cfg;
mem_leak_watch_default_config(&cfg);   // intervalo/limiar por build
mem_leak_watch_start(&cfg);            // cfg=NULL usa defaults
...
mem_leak_watch_scan_now();             // varredura sob demanda
mem_leak_watch_stop();
```

**Limitações conhecidas (v1, documentadas e confirmadas em teste):**

- Só **Windows** por enquanto (`thread_activity_win` / `StackWalk64`).
- Só rastreia spans de *size-class* (≤ 16 KB); blocos LARGE ainda não entram.
- Não varre `.data`/`.bss` como raiz — só pilha + registradores de cada thread
  viva. Ponteiro cuja única referência viva seja variável global/estática
  aparece como falso “vazamento”.
- Só enxerga threads criadas via `thread_create()` desta lib; threads OS cruas
  (`CreateThread`/`pthread_create` direto, ou de outra lib como o TBB) ficam
  invisíveis ao scan.
- Marca por **span inteiro**, não por bloco individual.
- Usa `SuspendThread`/`GetThreadContext` — mitigado suspendendo **uma thread por
  vez** e sem alocar/chamar `dbghelp` com threads suspensas.

O custo em produção é ~zero no hot path — ver a variante `memop-lw-prod` no
[benchmark do memory pool](#benchmark-1).

---

## Módulos de apoio

| Módulo | Arquivo | Descrição |
|---|---|---|
| **Tipos base** | `include/xplatbase.h` | `boolean`/`byte`/`int16..uint64`, `BufferXPB`, `ListXPB`, `StringX`, `CallContextGlobalEvent`, macros `xpb_*` com `__FILE__/__LINE__`, auto-init de plataforma |
| **Atomics** | `atomics.h` / `atomics.c` | Atômicos inline por plataforma (`InterlockedXxx` no Windows, `__atomic_*` no Linux): load/store, add, CAS, ponteiros, fences |
| **Thread handler** | `thread_handler.h` | `thread_create`/`thread_join`/`thread_enum`, mutex, yield/sleep0, atômicos de 64 bits e fence — abstração fina sobre WinAPI/pthread |
| **Thread wait** | `thread_wait.h` | Espera/sinal de baixo custo (`WaitOnAddress`/`WakeByAddress` no Windows, futex no Linux) para park/wake de workers |
| **Thread activity** | `thread_activity.h` (+ `_win.c`/`_linux.c`) | Amostra atividade de CPU por thread (ciclos no Windows, ns de CPU no Linux) e classifica tarefas em NORMAL / LONG_CPU / LONG_BLOCKED — base do worker elástico |
| **Ring queue** | `ring_queue.h` / `ring_queue.c` | Fila MPMC estilo **Vyukov**, `head`/`tail` em linhas de cache separadas (sem false sharing) |
| **Event handler** | `event_handler.h` / `event_handler.c` | Captura de contexto de erro (`CallContextGlobalEvent`) e disparo antes de encerrar |
| **List handler** | `list_hander.c` | `ListXPB` genérica com `malloc` do CRT de propósito (estruturas de vida do processo, fora do pool resetável) |

> `string_handler` está em fase de projeto e **não é coberto** por esta
> documentação.

---

## Build e uso

A biblioteca se auto-inicializa no carregamento (`platform_init()` via
`.CRT$XCU` no Windows / constructor no Linux). Para desativar e chamar
manualmente, defina `XPLATBASE_NO_AUTO_INIT`.

```c
#include "xplatbase.h"
#include "thread_pool.h"
#include "memory_pool.h"

static void task(void* arg) { /* ... */ }

int main(void)
{
    void* p = memop_alloc_raw(128);
    pool_submit(task, p);
    pool_wait_idle();
    memop_free_raw(p);
    return 0;
}
```

- **Windows / MSVC**: projetos `*.vcxproj` em `Xplatbase/`, `bench/` e `Tester/`.
- **Linux / GCC-Clang**: scripts em `bench/linux/` (`run.sh`, `compile_test.sh`).

### Reproduzir os benchmarks

```bash
# memory pool (Windows): roda bench.exe 9x e agrega as medianas
#   -> bench/mempool_bench_latest.log + bench/mempool_bench_medians.json
cd bench && python run_mempool.py 9
# (uma execucao avulsa e so: ./bench.exe — mas o Larson tem variancia alta,
#  entao prefira as medianas do harness acima)

# thread pool (Windows): reps=10, N=1M -> Tester/thread_pool/thread_pool_bench_results.tsv
cd Tester/thread_pool && ./thread_pool_bench.exe 10 1000000 > bench_run_latest.log 2>&1
```

---

## Status

Projeto em fase de projeto e consolidação. Os três destaques
(thread pool, memory pool, mem leak watch) estão funcionais e com benchmark; os
módulos de apoio dão a base multiplataforma. Portabilidade Linux é parcial em
alguns caminhos (o mem_leak_watch é Windows-only na v1).
