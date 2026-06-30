#!/usr/bin/env bash
set -u
HERE=/mnt/e/git/libs/xplatbase/bench/linux
SRC=/mnt/e/git/libs/xplatbase/Xplatbase/Xplatbase/src
cd "$HERE" || exit 1

TH=${1:-8}
OPS=${2:-4000000}

echo "### building bench_linux (memop embedded) ###"
gcc -O2 -pthread -DXPLATBASE_NO_AUTO_INIT -include stdarg.h \
    bench_linux.c \
    "$SRC/memory_pool.c" "$SRC/atomics.c" "$SRC/thread_wait.c" \
    "$SRC/thread_handler.c" "$SRC/list_hander.c" "$SRC/memory_handler.c" \
    "$SRC/event_handler.c" \
    -o bench_linux 2>build.log
[ -x ./bench_linux ] || { echo "BUILD FAILED"; grep -iE "undefined|reference|error" build.log | head -25; exit 1; }
echo "ok"

run() { # label  preload-path-or-empty  mode
  local label="$1" preload="$2" mode="$3"
  echo
  echo "==================== $label ===================="
  if [ -n "$preload" ]; then
    BENCH_LABEL="$label" LD_PRELOAD="$preload" ./bench_linux "$mode" "$TH" "$OPS"
  else
    BENCH_LABEL="$label" ./bench_linux "$mode" "$TH" "$OPS"
  fi
}

JE=$(ls /usr/lib/x86_64-linux-gnu/libjemalloc.so* 2>/dev/null | head -1)
TC=$(ls /usr/lib/x86_64-linux-gnu/libtcmalloc.so* 2>/dev/null | head -1)
MI=$(ls "$HERE"/libmimalloc.so 2>/dev/null | head -1)
SN=$(ls "$HERE"/libsnmallocshim.so 2>/dev/null | head -1)

run "glibc"    ""    sys
[ -n "$JE" ] && run "jemalloc" "$JE" sys || echo "(jemalloc ausente)"
[ -n "$TC" ] && run "tcmalloc" "$TC" sys || echo "(tcmalloc ausente)"
[ -n "$MI" ] && run "mimalloc" "$MI" sys || echo "(mimalloc ausente - build pendente)"
[ -n "$SN" ] && run "snmalloc" "$SN" sys || echo "(snmalloc ausente - build pendente)"
run "memop"    ""    memop
