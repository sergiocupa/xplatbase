#!/bin/bash
#  MIT License - Modified for Mandatory Attribution
#  Copyright(c) 2026 Sergio Paludo - github.com/sergiocupa
#
#  Testes portaveis do pool de tarefas no Linux: x64, ARM64 (emulado pelo qemu) e x64 com
#  ThreadSanitizer. O TSan e o que prova a atomicidade dos contadores que o monitor le
#  (unittests/test_pool_atomico.c): acesso comum entre threads vira relatorio de corrida e o
#  executor sai com 66. O resumo lista cada corrida pelas duas pontas (funcao arquivo:linha).
#
#  Build fora de /mnt (no WSL o disco do Windows e lento): $XPB_BUILD ou ~/.cache/xplatbase-build.
#  $XPB_SRC e $XPB_INC trocam a copia da lib compilada (padrao: a deste repositorio).
#  Precisa de gcc, gcc-aarch64-linux-gnu e qemu-user-static.
#
#  uso: bash roda_pool_atomico.sh [x64|arm64|tsan|todas] [repeticoes]   (padrao: todas 3)

set -uo pipefail
AQUI=$(cd "$(dirname "$0")" && pwd)
RAIZ=$(cd "$AQUI/../.." && pwd)
# $XPB_SRC/$XPB_INC: outra copia da lib (ex.: o HEAD extraido, para comparar antes x depois)
SRC=${XPB_SRC:-$RAIZ/Xplatbase/Xplatbase/src}
INC=${XPB_INC:-$RAIZ/Xplatbase/Xplatbase/include}
UT=$RAIZ/Tester/unittests
B=${XPB_BUILD:-$HOME/.cache/xplatbase-build}
ALVO=${1:-todas}
REP=${2:-3}
RC=0

FONTES=""
for f in atomics event_handler list_hander mem_leak_watch memory_pool numeric ring_queue \
         string_handler thread_activity_linux thread_handler thread_pool thread_wait \
         xplatbase xplat_instance; do
    [ -f "$SRC/$f.c" ] && FONTES="$FONTES $SRC/$f.c"
done
TESTE="$AQUI/executa_pool.c $UT/ctest_core.c $UT/test_pool_atomico.c"
echo "lib: $SRC"

compila() {   # $1 = nome, $2 = compilador, demais = opcoes
    local nome=$1 cc=$2; shift 2
    mkdir -p "$B"
    echo "== $nome: $cc $* ($(date +%T))"
    if ! nice -n 10 "$cc" "$@" -std=gnu11 -pthread -D_GNU_SOURCE -I"$SRC" -I"$INC" -I"$UT" \
            $FONTES $TESTE -o "$B/pool_$nome" -lm -ldl > "$B/$nome.log" 2>&1; then
        echo "compilacao falhou: $B/$nome.log"; grep -E "error|erro" "$B/$nome.log" | head -20
        return 1
    fi
    grep -E "warning:" "$B/$nome.log" | sort -u | head -10 || true
}

# Uma linha por corrida: as duas pontas (o acesso atual e o anterior), quadro #0 de cada.
resumo_tsan() {   # $1 = arquivo de saida do TSan
    local n
    n=$(grep -c "WARNING: ThreadSanitizer: data race" "$1" || true)
    echo "   corridas de dados relatadas pelo TSan: $n"
    awk '
        /WARNING: ThreadSanitizer: data race/ { em=1; pontas=""; next }
        em && /^  (Read|Write|Atomic|Previous)/ { pega=1; next }
        em && pega && /#0 / { n=split($3, a, "/"); pontas=pontas (pontas==""?"":"  x  ") $2 " " a[n]; pega=0; next }
        em && /^SUMMARY/ { print "     " pontas; em=0 }
    ' "$1" | sort | uniq -c | sort -rn | head -20
}

if [ "$ALVO" = x64 ] || [ "$ALVO" = todas ]; then
    if compila x64 gcc -O2 -g; then
        "$B/pool_x64" "$REP" || RC=1
    else RC=1; fi
fi
if [ "$ALVO" = arm64 ] || [ "$ALVO" = todas ]; then
    if compila arm64 aarch64-linux-gnu-gcc -O2 -g; then
        file "$B/pool_arm64" | sed 's/, BuildID.*//'
        qemu-aarch64-static -L /usr/aarch64-linux-gnu "$B/pool_arm64" "$REP" || RC=1
    else RC=1; fi
fi
if [ "$ALVO" = tsan ] || [ "$ALVO" = todas ]; then
    if compila tsan gcc -O1 -g -fno-omit-frame-pointer -fsanitize=thread; then
        # setarch -R: o TSan do WSL nao aceita o mapa de memoria com ASLR ("unexpected memory mapping")
        TSAN_OPTIONS="halt_on_error=0 exitcode=66 history_size=4" \
            setarch "$(uname -m)" -R "$B/pool_tsan" 1 2> "$B/tsan.txt"
        s=$?
        resumo_tsan "$B/tsan.txt"
        echo "   saida do executor com TSan: $s (66 = TSan achou corrida; relatorio: $B/tsan.txt)"
        [ $s -eq 0 ] || RC=1
    else RC=1; fi
fi
echo "== fim ($(date +%T)), saida $RC"
exit $RC
