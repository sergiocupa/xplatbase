@echo off
REM Build dos testes de regressao (POOL_TEST_HOOKS): determinism + bug suite.
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 ( echo vcvars64 FALHOU & exit /b 1 )

set SRC=..\Xplatbase\Xplatbase\src
set SRCS=%SRC%\thread_pool.c %SRC%\ring_queue.c %SRC%\atomics.c %SRC%\thread_wait.c %SRC%\thread_activity_win.c %SRC%\xplatbase.c %SRC%\event_handler.c
set FLAGS=/nologo /O2 /MD /DPOOL_TEST_HOOKS /D_CRT_SECURE_NO_WARNINGS /I "%SRC%" /I "..\Xplatbase\Xplatbase\include"
set LINKLIBS="C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0\um\x64\DbgHelp.Lib"

echo === determinism ===
if not exist reg_obj\det mkdir reg_obj\det
cl %FLAGS% thread_pool_determinism_test.c %SRCS% /Fe:det_test.exe /Fo:reg_obj\det\ %LINKLIBS% 2>&1 | findstr /C:"error" /C:"fatal"
if exist det_test.exe ( echo BUILD det OK ) else ( echo BUILD det FALHOU & exit /b 1 )

echo === bug suite ===
if not exist reg_obj\bug mkdir reg_obj\bug
cl %FLAGS% reg_bug_main.c thread_pool_bug_test.c %SRCS% /Fe:bug_test.exe /Fo:reg_obj\bug\ %LINKLIBS% 2>&1 | findstr /C:"error" /C:"fatal"
if exist bug_test.exe ( echo BUILD bug OK ) else ( echo BUILD bug FALHOU & exit /b 1 )
