#!/usr/bin/env bash
# bench/profile_decode.sh — profile the vv decoder with gprof.
#
# Builds an instrumented vaptvupt binary at /tmp/vv-prof, decodes
# dickens.vv ten times, and prints the gprof flat profile.
#
# Usage:
#   bash bench/profile_decode.sh [FIXTURE]
#
# FIXTURE defaults to /tmp/silesia/dickens. Set SILESIA env to override
# the corpus directory.

set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
SILESIA="${SILESIA:-/tmp/silesia}"
FIXTURE="${1:-$SILESIA/dickens}"

if [[ ! -r "$FIXTURE" ]]; then
    echo "error: fixture not found: $FIXTURE" >&2
    exit 1
fi

cd "$HERE"

echo "[1/4] Building profiled binary at /tmp/vv-prof..."
mkdir -p build_prof
rm -f build_prof/*.o /tmp/vv-prof
cc -pg -O2 -Wall -Wno-unused-parameter -msse4.2 -Iinclude \
   -D_POSIX_C_SOURCE=199309L \
   -c src/vv_simd.c -o build_prof/vv_simd.o
cc -pg -O2 -Wall -Wno-unused-parameter -msse4.2 -Iinclude \
   -D_POSIX_C_SOURCE=199309L \
   -c src/vv_decoder.c -o build_prof/vv_decoder.o
cc -pg -O2 -Wall -Wno-unused-parameter -Iinclude \
   -D_POSIX_C_SOURCE=199309L \
   src/main.c src/vv_encoder.c src/vv_xxh64.c src/vv_huffman.c \
   src/vv_ans.c src/vaptvupt_api.c \
   build_prof/vv_simd.o build_prof/vv_decoder.o -o /tmp/vv-prof

echo "[2/4] Pre-compressing fixture (balanced mode)..."
./vaptvupt -c -m balanced -o /tmp/_profile.vv "$FIXTURE" 2>&1 | tail -1

echo "[3/4] Decoding 10x with profiled binary..."
for i in $(seq 1 10); do
    /tmp/vv-prof -d -o /tmp/_profile.out /tmp/_profile.vv >/dev/null
done

echo "[4/4] gprof flat profile:"
echo "─────────────────────────────────────────────────"
gprof -b /tmp/vv-prof gmon.out 2>&1 | head -40

# Clean profile artifact
rm -f gmon.out /tmp/_profile.vv /tmp/_profile.out

echo ""
echo "Done. Top function above is the optimization target."
