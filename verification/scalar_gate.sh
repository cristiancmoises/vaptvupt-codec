#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Check the integer-only core independently of the userspace CLI's timers.
set -eu

scalar_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
scalar_tmp=$(mktemp -d "${TMPDIR:-/tmp}/vv-scalar.XXXXXX")
scalar_cc=${CC:-cc}
command -v nm >/dev/null 2>&1 || { echo 'scalar gate: nm is required' >&2; exit 2; }
case $(uname -m) in
    x86_64|aarch64) scalar_arch='-mgeneral-regs-only' ;;
    *) echo 'scalar gate: general-register check supports x86-64 and AArch64' >&2; exit 2 ;;
esac
scalar_flags='-O2 -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -DVV_DISABLE_SIMD=1 -DVV_ANS_TEST_HOOKS -D_POSIX_C_SOURCE=199309L'
scalar_objects=
for scalar_name in vv_encoder vv_decoder vv_simd vv_xxh64 vv_huffman vv_ans vv_bcj vaptvupt_api; do
    "$scalar_cc" $scalar_flags $scalar_arch -ffreestanding -fno-builtin \
        -fno-tree-vectorize -fstack-usage -I"$scalar_root/include" \
        -c "$scalar_root/src/$scalar_name.c" -o "$scalar_tmp/$scalar_name.o"
    scalar_objects="$scalar_objects $scalar_tmp/$scalar_name.o"
done

# A disabled dispatch path must not retain mutable dispatch state or CPU probes.
if nm "$scalar_tmp/vv_simd.o" | awk '/g_copy_|vv_init_simd|vv_has_avx2/ { bad=1 } END { exit !bad }'; then
    echo 'scalar gate: runtime SIMD dispatch remains in the object' >&2
    exit 1
fi
for scalar_test in scalar roundtrip streaming huffman ans api_contract exact_buffer_decode entropy_workspace fast_workspace xxh64 decoder_bounds; do
    "$scalar_cc" $scalar_flags -I"$scalar_root/include" \
        "$scalar_root/tests/test_$scalar_test.c" $scalar_objects \
        -o "$scalar_tmp/test_$scalar_test"
    "$scalar_tmp/test_$scalar_test"
done
printf 'scalar gate PASS: general-register core, eleven test suites\n'
printf 'Build and stack-usage diagnostics retained in %s\n' "$scalar_tmp"
printf 'This is a userspace portability check, not a Linux kernel build or stack-budget approval.\n'
