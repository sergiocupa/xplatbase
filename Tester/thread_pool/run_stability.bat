@echo off
rem roda o bench 2x para analise de estabilidade (run1 = TSV ja existente)
set DIR=E:\git\libs\xplatbase\Tester\thread_pool
cd /d %DIR%
if exist runs_done.marker del runs_done.marker
%DIR%\thread_pool_bench.exe 3 > bench_run2.log 2>&1
copy /Y thread_pool_bench_results.tsv thread_pool_bench_results_run2.tsv >nul
%DIR%\thread_pool_bench.exe 3 > bench_run3.log 2>&1
copy /Y thread_pool_bench_results.tsv thread_pool_bench_results_run3.tsv >nul
echo RUNS_DONE > runs_done.marker
