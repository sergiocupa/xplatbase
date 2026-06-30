#!/usr/bin/env bash
# Sanity: nossa lib compila no Linux/gcc?
set -u
SRC=/mnt/e/git/libs/xplatbase/Xplatbase/Xplatbase/src
cd "$SRC" || exit 1
ok=1
for f in memory_pool atomics thread_wait thread_handler list_hander memory_handler event_handler; do
  if gcc -c -O2 -pthread "$f.c" -o "/tmp/$f.o" 2>"/tmp/$f.err"; then
    echo "OK   $f"
  else
    echo "FAIL $f"
    head -8 "/tmp/$f.err"
    ok=0
  fi
done
echo "RESULT=$ok"
