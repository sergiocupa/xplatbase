@echo off
rem ============================================================================
rem  build.bat - compila o benchmark de alocadores (memop vs rpmalloc/mimalloc).
rem
rem  Uso:   build.bat            (compila bench.exe)
rem         build.bat run        (compila e ja roda)
rem
rem  Depois:  bench.exe [threads] [ops_por_thread]      ex.: bench.exe 8 4000000
rem
rem  Flags relevantes:
rem    /DMEMOP_NO_STATS         build de performance (stats fora do hot path)
rem    /std:c11 /experimental:c11atomics   exigido pelo mimalloc (static.c)
rem    advapi32.lib             rpmalloc/mimalloc usam privilege tokens
rem ============================================================================
setlocal enabledelayedexpansion
cd /d "%~dp0"

rem --- localizar o vcvars64.bat: tenta caminhos conhecidos; vswhere como fallback ---
set "VCVARS="
for %%E in (
  "C:\Program Files\Microsoft Visual Studio\18\Community"
  "C:\Program Files\Microsoft Visual Studio\2022\Community"
  "C:\Program Files\Microsoft Visual Studio\2022\Professional"
  "C:\Program Files\Microsoft Visual Studio\2022\Enterprise"
  "C:\Program Files\Microsoft Visual Studio\2022\BuildTools"
  "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community"
  "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools"
) do if not defined VCVARS if exist "%%~E\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%%~E\VC\Auxiliary\Build\vcvars64.bat"

if not defined VCVARS (
  set "VSWHERE=C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
  if exist "!VSWHERE!" for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -property installationPath`) do set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
)

if not exist "%VCVARS%" (
  echo [erro] vcvars64.bat nao encontrado. Visual Studio com toolset C++ instalado?
  exit /b 1
)
call "%VCVARS%" >nul 2>&1

set "SRC=..\Xplatbase\Xplatbase\src"
set "INC=..\Xplatbase\Xplatbase\include"

cl /nologo /O2 /MT /std:c11 /experimental:c11atomics ^
   /DNDEBUG /DXPLATBASE_NO_AUTO_INIT /DMEMOP_NO_STATS ^
   /I "%INC%" /I mimalloc\include ^
   bench.c ^
   "%SRC%\memory_pool.c" "%SRC%\atomics.c" "%SRC%\thread_wait.c" ^
   "%SRC%\thread_handler.c" "%SRC%\list_hander.c" "%SRC%\memory_handler.c" ^
   "%SRC%\event_handler.c" ^
   rpmalloc\rpmalloc\rpmalloc.c mimalloc\src\static.c ^
   /Fe:bench.exe /link advapi32.lib
if errorlevel 1 ( echo. & echo [erro] BUILD FALHOU & exit /b 1 )

echo.
echo BUILD OK -^> bench.exe
if /i "%~1"=="run" ( echo. & "%~dp0bench.exe" )
