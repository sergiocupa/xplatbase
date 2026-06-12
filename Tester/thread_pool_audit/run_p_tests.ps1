param(
    [string]$Repo = 'E:\git\libs\xplatbase',
    [int]$DeterminismRuns = 100,
    [switch]$IncludeCrashTests,
    [switch]$IncludeLinuxTests
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$vcvars = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'
$source = Join-Path $here 'thread_pool_p_tests.c'
$exe = Join-Path $here 'thread_pool_p_tests.exe'
$detSource = Join-Path $Repo 'Tester\thread_pool_determinism_test.c'
$detExe = Join-Path $Repo 'Tester\determinism_test.exe'
$libDir = Join-Path $Repo 'Xplatbase\x64\Release'
$includeSrc = Join-Path $Repo 'Xplatbase\Xplatbase\src'
$includePublic = Join-Path $Repo 'Xplatbase\Xplatbase\include'

$compile = "`"$vcvars`" >nul && cl /nologo /O2 /MD /W4 " +
    "/I`"$includeSrc`" /I`"$includePublic`" `"$source`" /Fe:`"$exe`" " +
    "/link /LTCG /LIBPATH:`"$libDir`" Xplatbase.lib Synchronization.lib " +
    "Winmm.lib legacy_stdio_definitions.lib"
$compileDeterminism = "`"$vcvars`" >nul && cl /nologo /O2 /MD /W4 " +
    "/DPOOL_TEST_HOOKS /I`"$includeSrc`" /I`"$includePublic`" " +
    "`"$detSource`" /Fe:`"$detExe`" /link /LTCG /LIBPATH:`"$libDir`" " +
    "Xplatbase.lib Synchronization.lib Winmm.lib legacy_stdio_definitions.lib"

Write-Host '== Build P tests =='
cmd.exe /d /s /c $compile
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
cmd.exe /d /s /c $compileDeterminism
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host ''
Write-Host '== P dynamic tests =='
$dynamicArgs = @()
if ($IncludeCrashTests) {
    Write-Warning 'P02 e P17 provocam access violations somente em processos filhos.'
    $dynamicArgs += '--dangerous'
}
& $exe @dynamicArgs
$dynamicExit = $LASTEXITCODE

Write-Host ''
Write-Host '== P contract tests =='
& (Join-Path $here 'thread_pool_p_contract_tests.ps1') -Repo $Repo
$contractExit = $LASTEXITCODE

Write-Host ''
Write-Host '== P10/P09 determinism and false-green =='
$flake = $false
$flakeExit = -1
$flakeRun = 0
for ($i = 1; $i -le $DeterminismRuns; $i++) {
    $text = (& $detExe 2>&1) -join "`n"
    $exitCode = $LASTEXITCODE
    if ($text -match '\[FALHOU\]') {
        $flake = $true
        $flakeExit = $exitCode
        $flakeRun = $i
        break
    }
}
if ($flake) {
    Write-Host "  [MANIFESTOU] P10 flake deterministico - run=$flakeRun"
    Write-Host "  [MANIFESTOU] P09 falso verde - exit=$flakeExit com [FALHOU]"
} else {
    Write-Host "  [NAO MANIFESTOU] P10/P09 em $DeterminismRuns execucoes"
}

Write-Host ''
Write-Host '== Linux WSL =='
if (!$IncludeLinuxTests) {
    Write-Host '  [PULADO] P05-P07 dinamicos: use -IncludeLinuxTests em uma sessao dedicada.'
} else {
    $wslOutput = (& wsl.exe --list --quiet 2>&1) -join ''
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace(($wslOutput -replace "`0", ''))) {
        Write-Host '  [BLOQUEADO] nenhuma distribuicao WSL esta acessivel nesta sessao.'
        exit 1
    } else {
        $linuxScriptWin = Join-Path $here 'run_p_linux_tests.sh'
        $linuxScript = ((& wsl.exe wslpath -a $linuxScriptWin 2>&1) -join '') -replace "`0", ''
        $linuxRepo = ((& wsl.exe wslpath -a $Repo 2>&1) -join '') -replace "`0", ''
        & wsl.exe bash $linuxScript $linuxRepo
        if ($LASTEXITCODE -ne 0) {
            Write-Host "  [NAO MANIFESTOU] runner Linux terminou com exit=$LASTEXITCODE"
            exit 1
        }
    }
}

Write-Host ''
Write-Host "dynamic_exit=$dynamicExit contract_exit=$contractExit flake=$flake flake_exit=$flakeExit"
if ($dynamicExit -ne 0 -or $contractExit -ne 0 -or !$flake -or $flakeExit -ne 0) {
    exit 1
}
exit 0
