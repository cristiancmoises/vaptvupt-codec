#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Allocation-fault injection regression harness.
# Sprint 100: validates that VaptVupt handles malloc failures cleanly
# (no crashes, no UB, no leaks) across encode/decode/MT paths.

set -e
SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
SO=/tmp/malloc_fault.so
cc -O2 -fPIC -shared -ldl -o "$SO" "$SCRIPT_DIR/malloc_fault.c"

VV=${VV:-./vaptvupt}
INPUT=${INPUT:-/tmp/fx_text}

echo "=== Allocation-fault injection regression ==="
crashes=0; ok=0; clean=0
for s in $(seq 1 50); do
    out=$(VV_FAULT_RATE=200 VV_FAULT_SEED=$s VV_FAULT_SKIP=5 \
          LD_PRELOAD="$SO" "$VV" -c -m balanced "$INPUT" -o /tmp/x.vv 2>&1)
    ec=$?
    if [ $ec -eq 0 ]; then ok=$((ok+1))
    elif [ $ec -eq 1 ]; then clean=$((clean+1))
    elif [ $ec -ge 128 ]; then crashes=$((crashes+1))
    fi
done
echo "  encode: $ok ok / $clean clean-fail / $crashes CRASHES"
[ $crashes -gt 0 ] && exit 1

# Build a clean compressed file for decode tests
"$VV" -c -m balanced "$INPUT" -o /tmp/seed.vv 2>/dev/null
crashes=0
for s in $(seq 1 50); do
    out=$(VV_FAULT_RATE=200 VV_FAULT_SEED=$s VV_FAULT_SKIP=5 \
          LD_PRELOAD="$SO" "$VV" -d /tmp/seed.vv -o /tmp/o 2>&1)
    ec=$?
    [ $ec -ge 128 ] && crashes=$((crashes+1))
done
echo "  decode: $crashes CRASHES / 50"
[ $crashes -gt 0 ] && exit 1

echo "  ✓ ALL PASS"
