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

# The injector must precede libasan in LD_PRELOAD so its malloc wrapper sees
# allocations.  ASan normally rejects that ordering; disabling only that
# startup-order diagnostic retains its allocation and UB checks.
if ldd "$VV_BIN" 2>/dev/null | grep -q 'libasan'; then
  ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}verify_asan_link_order=0"
  export ASAN_OPTIONS
fi

# A compressible input that exercises the matcher, entropy coder, and the BCJ
# copy path (a small ELF-like blob; content need not be a real binary).
INPUT="$TMP/in.bin"
i=0; : > "$INPUT"
while [ "$i" -lt 64 ]; do printf 'In Code We Trust. VaptVupt OOM robustness probe. ' >> "$INPUT"; i=$((i+1)); done
head -c 200000 /dev/urandom >> "$INPUT" 2>/dev/null || true

count_allocs() {
  report="$(VV_OOM_REPORT=1 VV_OOM_FAIL=99999999 LD_PRELOAD="$TMP/oom_inject.so" \
    "$VV_BIN" "$@" 2>&1 >/dev/null)" || {
      echo "  FAIL: could not initialize OOM injector" >&2
      [ -n "$report" ] && printf '%s\n' "$report" >&2
      return 1
    }
  total="$(printf '%s\n' "$report" | sed -n 's/^OOM_ALLOC_COUNT=//p')"
  case "$total" in
    ''|*[!0-9]*)
      echo "  FAIL: OOM injector did not report an allocation count" >&2
      return 1
      ;;
  esac
  printf '%s\n' "$total"
}

sweep() {
  desc="$1"; shift
  if ! total="$(count_allocs "$@")"; then
    return 1
  fi
  crashes=0; unexpected=0; n=0
  while [ "$n" -lt "$total" ]; do
    # Allocation failures normally make the CLI return 1, which is an
    # expected clean rejection.  Capture the real status without letting
    # `set -e` abort the sweep; `command || true; rc=$?` would record the
    # status of `true` and silently turn every crash into a pass.
    if LD_PRELOAD="$TMP/oom_inject.so" VV_OOM_FAIL="$n" \
         "$VV_BIN" "$@" >/dev/null 2>&1; then
      cmd_rc=0
    else
      cmd_rc=$?
    fi
    if [ "$cmd_rc" -ge 128 ]; then
      echo "  FAIL: crash ($desc) at allocation #$n (signal $((cmd_rc-128)))"
      crashes=$((crashes+1))
    elif [ "$cmd_rc" -ne 0 ] && [ "$cmd_rc" -ne 1 ]; then
      # The CLI uses 1 for a clean codec/allocation rejection. Any other
      # non-zero status is an infrastructure or runtime failure.
      echo "  FAIL: unexpected exit $cmd_rc ($desc) at allocation #$n"
      unexpected=$((unexpected+1))
    fi
    n=$((n+1))
  done
  echo "  $desc: swept $total allocation points, $crashes crashes, $unexpected unexpected exits"
  [ "$crashes" -eq 0 ] && [ "$unexpected" -eq 0 ]
}

echo "OOM-robustness sweep:"
"$VV_BIN" -c -m balanced --bcj -o "$TMP/valid.vv" "$INPUT" 2>/dev/null
# Establish that the randomized fixture is a valid codec round-trip before
# fault injection. Otherwise a latent encoder bug is misreported later as an
# injector-initialization failure when count_allocs probes decompression.
if ! "$VV_BIN" -d -o "$TMP/valid.out" "$TMP/valid.vv" 2>/dev/null ||
   ! cmp -s "$INPUT" "$TMP/valid.out"; then
  echo "  FAIL: baseline OOM fixture did not round-trip before injection" >&2
  exit 1
fi
result=0
sweep "compress (balanced --bcj)"  -c -m balanced --bcj -o "$TMP/o.vv"  "$INPUT"   || result=1
sweep "decompress"                 -d              -o "$TMP/o.out" "$TMP/valid.vv"  || result=1
if [ "$result" -eq 0 ]; then echo "  OOM sweep PASS (no crash on any single allocation failure)"; else echo "  OOM sweep FAILED"; fi
exit $result
