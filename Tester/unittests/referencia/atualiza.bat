@echo off
REM Regrava thread_pool_antes.c com o thread_pool.c de ANTES da ultima alteracao commitada.
REM
REM Rode depois de commitar uma mudanca no pool: o bench ANTES x DEPOIS passa a comparar a
REM versao nova contra a imediatamente anterior. A 1a linha do arquivo registra de onde ele
REM veio, e e ela que aparece no relatorio do teste.
setlocal
cd /d "%~dp0"
set RAIZ=%~dp0..\..\..
set ARQ=Xplatbase/Xplatbase/src/thread_pool.c

for /f %%c in ('git -C "%RAIZ%" log -1 --format^=%%h -- %ARQ%') do set ULTIMO=%%c
if "%ULTIMO%"=="" (echo nao achei commit que alterou %ARQ% & exit /b 1)
for /f %%c in ('git -C "%RAIZ%" rev-parse --short %ULTIMO%~1') do set ANTERIOR=%%c
if "%ANTERIOR%"=="" (echo %ULTIMO% nao tem commit anterior & exit /b 1)

> thread_pool_antes.c echo // ANTES: thread_pool.c da revisao %ANTERIOR%, anterior ao commit %ULTIMO% (a ultima alteracao commitada do pool)
git -C "%RAIZ%" show %ANTERIOR%:%ARQ% >> thread_pool_antes.c
if errorlevel 1 (echo git show falhou & exit /b 1)
echo referencia: thread_pool.c de %ANTERIOR% (antes de %ULTIMO%)
