param(
    [string]$Repo = 'E:\git\libs\xplatbase'
)

$ErrorActionPreference = 'Stop'
$manifested = 0
$run = 0

function Test-PContract {
    param([string]$Id, [bool]$Condition, [string]$Detail)
    $script:run++
    if ($Condition) {
        $script:manifested++
        Write-Host "  [MANIFESTOU] $Id - $Detail"
    } else {
        Write-Host "  [NAO MANIFESTOU] $Id - $Detail"
    }
}

$src = Join-Path $Repo 'Xplatbase\Xplatbase\src'
$pool = Get-Content -Raw (Join-Path $src 'thread_pool.c')
$header = Get-Content -Raw (Join-Path $src 'thread_pool.h')
$ring = Get-Content -Raw (Join-Path $src 'ring_queue.c')
$ringHeader = Get-Content -Raw (Join-Path $src 'ring_queue.h')
$wait = Get-Content -Raw (Join-Path $src 'thread_wait.c')
$linux = Get-Content -Raw (Join-Path $src 'thread_activity_linux.c')
$windows = Get-Content -Raw (Join-Path $src 'thread_activity_win.c')
$docs = Get-Content -Raw (Join-Path $Repo 'docs\thread_pool\index.html')
$mainTest = Get-Content -Raw (Join-Path $Repo 'Tester\thread_pool_test.c')
$detTest = Get-Content -Raw (Join-Path $Repo 'Tester\thread_pool_determinism_test.c')

Write-Host '================================================================'
Write-Host '  THREAD POOL - MANIFESTACAO P CONTRATUAL/ESTATICA'
Write-Host '================================================================'

Test-PContract 'P05 Linux TSC/ns' (
    $pool -match '(?s)#if defined\(__x86_64__\).*?__rdtsc\(\)' -and
    $linux -match '(?s)xthread_cycles_per_ns\(void\).*?return 1\.0'
) 'TSC em ciclos e conversao fixa de 1 ciclo/ns'

Test-PContract 'P06 pthread_create' (
    $pool -match 'pthread_t t;\s*pthread_create\(&t,\s*NULL,\s*fn,\s*arg\);\s*return t;'
) 'retorno de pthread_create ignorado'

Test-PContract 'P07 pthread_cancel' (
    $pool -match '(?s)pthread_cancel\(w->handle\).*?force_killed = 1;.*?WSTATE_STOPPED'
) 'estado STOPPED publicado sem join confirmando termino'

Test-PContract 'P09 exit code suite principal' (
    $mainTest -match 'thread_pool_test_run\(\);\s*return 0;'
) 'main retorna zero independentemente dos asserts'

Test-PContract 'P09 exit code deterministico' (
    $detTest -match '(?s)asserts saudaveis que passaram.*?return 0;'
) 'teste deterministico tambem retorna zero'

$handsBlocked = (
    $linux -match 'return eval->state == XTASK_STATE_LONG_BLOCKED' -and
    $windows -match 'return eval->state == XTASK_STATE_LONG_BLOCKED'
)
Test-PContract 'P11 documentacao thresholds' (
    $handsBlocked -and
    ($docs -match 'CPU.*HANDOFF' -or $docs -match 'consumindo CPU.*HANDOFF')
) 'documentacao promete handoff CPU, codigo seleciona LONG_BLOCKED'

Test-PContract 'P13 indices signed' (
    $ringHeader -match 'xatomic_int\s+head' -and
    $ringHeader -match 'xatomic_int\s+tail' -and
    $ring -match 't \+ 1' -and $ring -match 'h \+ 1'
) 'head/tail/seqno usam aritmetica signed no wrap'

Test-PContract 'P15 stats 32-bit' (
    $pool -match 'xatomic_int\s+stat_submitted' -and
    $header -match 'uint64_t\s+total_submitted'
) 'armazenamento interno 32-bit exposto como 64-bit'

Test-PContract 'P17 configuracao extrema' (
    $pool -match '\(pool->lane_capacity \+ reserve_target \+ xcpu_count\(\)\) \* 4' -and
    $pool -match 'while \(rc < reserve_ring_cap\) rc <<= 1'
) 'somas, multiplicacao e arredondamento signed sem checked arithmetic'

Test-PContract 'P19 timer global' (
    $wait -match 'timeBeginPeriod\(1\)' -and $wait -notmatch 'timeEndPeriod'
) 'timeBeginPeriod nao e balanceado'

Test-PContract 'P20 reserve_push ignorado' (
    $pool -match '(?s)static void worker_return_to_reserve.*?worker_leave_lane\(w\);\s*reserve_push\(pool,\s*w\);'
) 'worker sai da lane e o retorno de reserve_push e descartado'

Test-PContract 'P21 janela de contracao' (
    $pool -match '(?s)ring_queue_count\(&pool->lanes\[active - 1\]\.ring\) == 0;.*?atomic_cas\(&pool->active_lanes'
) 'count e CAS de contracao nao formam operacao atomica'

Write-Host '----------------------------------------------------------------'
Write-Host "  manifestados: $manifested / $run contratos executados"
Write-Host '----------------------------------------------------------------'
if ($manifested -ne $run) { exit 1 }
exit 0
