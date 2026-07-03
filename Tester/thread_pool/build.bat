@echo off
setlocal
cd /d "%~dp0"

call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 ( echo vcvars64 FALHOU & exit /b 1 )

set SRC=..\..\Xplatbase\Xplatbase\src
set TBB_DIR_DEFAULT=E:\tools\vcpkg\bin\installed\x64-windows
if "%TBB_DIR%"=="" set TBB_DIR=%TBB_DIR_DEFAULT%

if not exist "%TBB_DIR%\include\tbb\task_arena.h" ( echo TBB nao encontrado em "%TBB_DIR%" & exit /b 1 )
if not exist obj mkdir obj

set FLAGS=/nologo /O2 /MD /EHsc /std:c++17 /D_CRT_SECURE_NO_WARNINGS /DTBB_AVAILABLE /I "%SRC%" /I "..\..\Xplatbase\Xplatbase\include" /I "%TBB_DIR%\include"

set SRCS=thread_pool_bench.cpp %SRC%\thread_pool.c %SRC%\ring_queue.c %SRC%\atomics.c %SRC%\thread_wait.c %SRC%\thread_handler.c %SRC%\list_hander.c %SRC%\memory_handler.c %SRC%\memory_pool.c %SRC%\thread_activity_win.c %SRC%\event_handler.c
set LIBS=/link /LIBPATH:"%TBB_DIR%\lib" tbb12.lib DbgHelp.lib

cl %FLAGS% %SRCS% /Fe:thread_pool_bench.exe /Fo:obj\ %LIBS%
if errorlevel 1 exit /b 1

if exist "%TBB_DIR%\bin\tbb12.dll" copy /Y "%TBB_DIR%\bin\tbb12.dll" . >nul
echo BUILD OK: thread_pool_bench.exe
exit /b 0
