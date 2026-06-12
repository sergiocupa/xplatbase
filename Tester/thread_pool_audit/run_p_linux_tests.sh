#!/usr/bin/env bash
set -uo pipefail

repo="${1:-/mnt/e/git/libs/xplatbase}"
here="$(cd "$(dirname "$0")" && pwd)"
src="$repo/Xplatbase/Xplatbase/src"
inc="$repo/Xplatbase/Xplatbase/include"
exe="$here/thread_pool_p_linux_tests"
build_log="$here/thread_pool_p_linux_build.log"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

compile_pool() {
  local source_root="$1"
  local include_root="$2"
  local output="$3"
  gcc -std=gnu11 -O2 -pthread -fgnu89-inline \
  -include string.h -include x86intrin.h \
  -I"$source_root" -I"$include_root" \
  "$here/thread_pool_p_linux_tests.c" \
  "$source_root/thread_pool.c" \
  "$source_root/ring_queue.c" \
  "$source_root/atomics.c" \
  "$source_root/thread_wait.c" \
  "$source_root/thread_activity_linux.c" \
  -Wl,--wrap=pthread_create \
  -o "$output"
}

echo "== Linux original build contract =="
set +e
compile_pool "$src" "$inc" "$exe.original" >"$build_log" 2>&1
original_build=$?
set -e
if [[ $original_build -ne 0 ]]; then
  echo "  [MANIFESTOU] P22 backend Linux nao compila com GCC - exit=$original_build"
  grep -E -m 8 'error:|fatal error:' "$build_log" || true
else
  echo "  [NAO MANIFESTOU] P22 backend Linux nao compila com GCC"
  rm -f "$exe.original"
fi

echo
echo "== Instrumented temporary build for P05-P07 =="
mkdir -p "$tmp/src" "$tmp/include"
cp -a "$src/." "$tmp/src/"
cp -a "$inc/." "$tmp/include/"

# Ajustes somente na copia temporaria. Eles liberam a compilacao para que os
# testes dos defeitos Linux posteriores possam ser executados.
sed -i 's/XPLATBASE_WIN void platform_init()/XPLATBASE_API void platform_init()/' \
  "$tmp/include/xplatbase.h"
sed -i 's/__attribute__((constructor)) void my_init()/__attribute__((constructor)) static void my_init()/' \
  "$tmp/include/xplatbase.h"
sed -i 's/#define XPL_FN   static void\*/#define XPL_FN   void*/' \
  "$tmp/src/thread_pool.c"
sed -i '/#include "xthread_activity.h"/d' \
  "$tmp/src/thread_activity_linux.c"

set +e
compile_pool "$tmp/src" "$tmp/include" "$exe" >"$build_log.instrumented" 2>&1
instrumented_build=$?
set -e
if [[ $instrumented_build -ne 0 ]]; then
  echo "  [BLOQUEADO] copia temporaria nao compilou - exit=$instrumented_build"
  grep -E -m 12 'error:|fatal error:' "$build_log.instrumented" || true
  exit 1
fi
echo "  [OK] copia temporaria compilada; biblioteca original nao foi modificada"

fail=0

run_expected_zero() {
  local id="$1"
  local mode="$2"
  if timeout 8s "$exe" "$mode"; then
    echo "  [MANIFESTOU] $id"
  else
    local code=$?
    echo "  [NAO MANIFESTOU] $id - exit=$code"
    fail=1
  fi
}

echo
echo "== Dynamic Linux tests =="
run_expected_zero "P05 Linux TSC/ns" p05

set +e
timeout 8s "$exe" p06
p06_exit=$?
set -e
if [[ $p06_exit -eq 66 || $p06_exit -eq 124 || $p06_exit -ge 128 ]]; then
  echo "  [MANIFESTOU] P06 pthread_create ignorado - exit=$p06_exit"
else
  echo "  [NAO MANIFESTOU] P06 pthread_create ignorado - exit=$p06_exit"
  fail=1
fi

run_expected_zero "P07 pthread_cancel otimista" p07
exit "$fail"
