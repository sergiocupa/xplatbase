@echo off
setlocal
cd /d "%~dp0"

set REPS=3
if not "%~1"=="" set REPS=%~1

set CSV=thread_pool_cpu_bench_results.csv
if exist "%CSV%" del "%CSV%"

set TBB_DIR_DEFAULT=E:\tools\vcpkg\bin\installed\x64-windows
if "%TBB_DIR%"=="" set TBB_DIR=%TBB_DIR_DEFAULT%

if not exist ".\tbb12.dll" (
  if exist "%TBB_DIR%\bin\tbb12.dll" (
    copy /Y "%TBB_DIR%\bin\tbb12.dll" . >nul
  )
)

if not exist ".\tbb12.dll" (
  echo tbb12.dll nao encontrada.
  echo Rode build_thread_pool_cpu_bench.bat ou ajuste TBB_DIR para a raiz do TBB.
  echo Exemplo: set TBB_DIR=E:\tools\vcpkg\bin\installed\x64-windows
  exit /b 1
)

set SCENARIOS=rapida/baixa rapida/media rapida/alta rapida/ultra media/baixa media/media media/alta media/ultra mista/baixa mista/media mista/alta mista/ultra satur/alta satur/ultra rajada-curta/media rajada-mista/media mista-massiva/alta mista-massiva/ultra
set ADAPTERS=atual-v16 WinTP TBB

for %%A in (%ADAPTERS%) do (
  for %%S in (%SCENARIOS%) do (
    echo.
    echo === %%A :: %%S ===
    thread_pool_cpu_bench.exe --adapter %%A --scenario %%S --reps %REPS% --csv "%CSV%"
    if errorlevel 1 (
      echo FALHOU: %%A %%S
      exit /b 1
    )
  )
)

echo.
echo Resultado: %CSV%
exit /b 0
