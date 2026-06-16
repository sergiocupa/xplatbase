@echo off
setlocal
cd /d "%~dp0"

call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
  echo vcvars64 FALHOU
  exit /b 1
)

set SRC=..\Xplatbase\Xplatbase\src
set TBB_DIR_DEFAULT=E:\tools\vcpkg\bin\installed\x64-windows
if "%TBB_DIR%"=="" set TBB_DIR=%TBB_DIR_DEFAULT%

if not exist "%TBB_DIR%\include\tbb\task_group.h" (
  echo TBB nao encontrado em "%TBB_DIR%"
  echo Ajuste TBB_DIR para a raiz do oneTBB/vcpkg x64-windows.
  exit /b 1
)

if not exist cpu_bench_obj mkdir cpu_bench_obj

set SRCS=thread_pool_cpu_bench.cpp %SRC%\thread_pool.c %SRC%\ring_queue.c %SRC%\atomics.c %SRC%\thread_wait.c %SRC%\thread_activity_win.c %SRC%\xplatbase.c %SRC%\event_handler.c
set FLAGS=/nologo /O2 /MD /EHsc /std:c++17 /D_CRT_SECURE_NO_WARNINGS /DTBB_AVAILABLE /I "%SRC%" /I "..\Xplatbase\Xplatbase\include" /I "%TBB_DIR%\include"
set LIBS=/link /LIBPATH:"%TBB_DIR%\lib" tbb12.lib DbgHelp.lib

cl %FLAGS% %SRCS% /Fe:thread_pool_cpu_bench.exe /Fo:cpu_bench_obj\ %LIBS%
if errorlevel 1 exit /b 1

if exist "%TBB_DIR%\bin\tbb12.dll" copy /Y "%TBB_DIR%\bin\tbb12.dll" . >nul

echo BUILD OK: thread_pool_cpu_bench.exe
exit /b 0
