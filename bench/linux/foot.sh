#!/usr/bin/env bash
set -u
HERE=/mnt/e/git/libs/xplatbase/bench/linux
SRC=/mnt/e/git/libs/xplatbase/Xplatbase/Xplatbase/src
cd "$HERE" || exit 1
MB=${1:-512}

gcc -O2 -pthread -DXPLATBASE_NO_AUTO_INIT -include stdarg.h \
    footprint.c "$SRC/memory_pool.c" "$SRC/atomics.c" "$SRC/thread_wait.c" \
    "$SRC/thread_handler.c" "$SRC/list_hander.c" "$SRC/memory_handler.c" "$SRC/event_handler.c" \
    -o footprint 2>foot_build.log
[ -x ./footprint ] || { echo "BUILD FAILED"; grep -iE "error|undefined" foot_build.log | head; exit 1; }

JE=$(ls /usr/lib/x86_64-linux-gnu/libjemalloc.so* 2>/dev/null | head -1)
TC=$(ls /usr/lib/x86_64-linux-gnu/libtcmalloc.so* 2>/dev/null | head -1)
MI=$HERE/libmimalloc.so

echo "== Footprint: aloca ${MB}MB vivos, libera tudo, mede RSS (pool vivo) =="
BENCH_LABEL=glibc                       ./footprint sys   "$MB"
[ -n "$JE" ] && BENCH_LABEL=jemalloc LD_PRELOAD="$JE" ./footprint sys "$MB"
[ -n "$TC" ] && BENCH_LABEL=tcmalloc LD_PRELOAD="$TC" ./footprint sys "$MB"
[ -f "$MI" ] && BENCH_LABEL=mimalloc LD_PRELOAD="$MI" ./footprint sys "$MB"
BENCH_LABEL=memop                       ./footprint memop "$MB"
