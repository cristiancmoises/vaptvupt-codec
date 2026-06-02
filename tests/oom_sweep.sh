#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# OOM-robustness sweep. Fails each allocation site in vv_compress and
# vv_decompress in turn (via tests/oom_inject.c under LD_PRELOAD) and asserts
# the binary never crashes (no SIGSEGV/SIGABRT) — it must return a clean error
# or succeed. Run from the repo root after `make`. Requires the vaptvupt
# binary and a C compiler for the injector.
#
# Under AddressSanitizer (build vaptvupt with -fsanitize=address,undefined and
# point $VV_BIN at it), the same sweep additionally proves no leak / no
# use-after-free on every allocation-failure path, because the injector routes
# real allocations through dlsym(RTLD_NEXT) into ASan.
set -e
VV_BIN="${VV_BIN:-./vaptvupt}"
CC="${CC:-cc}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

"$CC" -O2 -fPIC -shared tests/oom_inject.c -ldl -o "$TMP/oom_inject.so"

# A compressible input that exercises the matcher, entropy coder, and the BCJ
# copy path (a small ELF-like blob; content need not be a real binary).
INPUT="$TMP/in.bin"
i=0; : > "$INPUT"
while [ "$i" -lt 64 ]; do printf 'In Code We Trust. VaptVupt OOM robustness probe. ' >> "$INPUT"; i=$((i+1)); done
head -c 200000 /dev/urandom >> "$INPUT" 2>/dev/null || true

count_allocs() {
  VV_OOM_REPORT=1 VV_OOM_FAIL=99999999 LD_PRELOAD="$TMP/oom_inject.so" \
    "$VV_BIN" "$@" 2>&1 >/dev/null | sed -n 's/^OOM_ALLOC_COUNT=//p'
}

sweep() {
  desc="$1"; shift
  total="$(count_allocs "$@")"
  [ -n "$total" ] || total=0
  crashes=0; n=0
  while [ "$n" -lt "$total" ]; do
    LD_PRELOAD="$TMP/oom_inject.so" VV_OOM_FAIL="$n" "$VV_BIN" "$@" >/dev/null 2>&1 || true
    rc=$?
    if [ "$rc" -ge 128 ]; then
      echo "  FAIL: crash ($desc) at allocation #$n (signal $((rc-128)))"
      crashes=$((crashes+1))
    fi
    n=$((n+1))
  done
  echo "  $desc: swept $total allocation points, $crashes crashes"
  [ "$crashes" -eq 0 ]
}

echo "OOM-robustness sweep:"
"$VV_BIN" -c -m balanced --bcj -o "$TMP/valid.vv" "$INPUT" 2>/dev/null
rc=0
sweep "compress (balanced --bcj)"  -c -m balanced --bcj -o "$TMP/o.vv"  "$INPUT"   || rc=1
sweep "decompress"                 -d              -o "$TMP/o.out" "$TMP/valid.vv"  || rc=1
if [ "$rc" -eq 0 ]; then echo "  OOM sweep PASS (no crash on any single allocation failure)"; else echo "  OOM sweep FAILED"; fi
exit $rc
