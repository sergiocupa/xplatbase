@echo off
REM Build do benchmark comparativo (WSPool x WinThreadPool x TBB).
REM Detecta o oneTBB automaticamente; sem TBB, compila WSPool + WinThreadPool.
setlocal enabledelayedexpansion
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 ( echo vcvars64 FALHOU & exit /b 1 )

set SRC=..\Xplatbase\Xplatbase\src
set SRCS=%SRC%\thread_pool.c %SRC%\ring_queue.c %SRC%\atomics.c %SRC%\thread_wait.c %SRC%\thread_activity_win.c %SRC%\xplatbase.c %SRC%\event_handler.c
set INC=/I "%SRC%" /I "..\Xplatbase\Xplatbase\include"
set LIBS="C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0\um\x64\DbgHelp.Lib" psapi.lib
set TBBOPT=

REM --- auto-detecta TBB: vcpkg (TBB_DIR do vcxproj) e depois oneAPI ---
set "TBBROOT="
if exist "E:\tools\vcpkg\bin\installed\x64-windows\include\tbb\task_arena.h" set "TBBROOT=E:\tools\vcpkg\bin\installed\x64-windows"
if not defined TBBROOT if defined TBB_DIR if exist "%TBB_DIR%\include\tbb\task_arena.h" set "TBBROOT=%TBB_DIR%"
if not defined TBBROOT if exist "C:\Program Files (x86)\Intel\oneAPI\tbb\latest\include\tbb\task_arena.h" set "TBBROOT=C:\Program Files (x86)\Intel\oneAPI\tbb\latest"

if defined TBBROOT (
  echo TBB encontrado: !TBBROOT!
  set TBBOPT=/DBENCH_TBB /I "!TBBROOT!\include"
  set LIBS=!LIBS! "!TBBROOT!\lib\tbb12.lib"
) else (
  echo TBB NAO encontrado - compilando sem TBB ^(WSPool + WinThreadPool^)
)

if not exist cmp_obj mkdir cmp_obj
cl /nologo /O2 /MD /EHsc /std:c++17 /D_CRT_SECURE_NO_WARNINGS !TBBOPT! %INC% ^
   pool_compare_bench.cpp %SRCS% !LIBS! ^
   /Fe:pool_compare_bench.exe /Fo:cmp_obj\ 2>&1 | findstr /C:"error" /C:"fatal"
if exist pool_compare_bench.exe ( echo BUILD OK ) else ( echo BUILD FALHOU & exit /b 1 )
