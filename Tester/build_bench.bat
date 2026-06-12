@echo off
REM Build do benchmark do thread_pool (cl /O2, x64). Saida: thread_pool_bench.exe
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 ( echo vcvars64 FALHOU & exit /b 1 )

set SRC=..\Xplatbase\Xplatbase\src
cl /nologo /O2 /MD /D_CRT_SECURE_NO_WARNINGS /I "%SRC%" /I "..\Xplatbase\Xplatbase\include" ^
   thread_pool_bench.c ^
   "%SRC%\thread_pool.c" ^
   "%SRC%\ring_queue.c" ^
   "%SRC%\atomics.c" ^
   "%SRC%\thread_wait.c" ^
   "%SRC%\thread_activity_win.c" ^
   "%SRC%\xplatbase.c" ^
   "%SRC%\event_handler.c" ^
   /Fe:thread_pool_bench.exe /Fo:bench_obj\ 2>&1
if errorlevel 1 ( echo BUILD FALHOU & exit /b 1 )
echo BUILD OK
