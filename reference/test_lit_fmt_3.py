#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Round-trip regression test for the Python reference decoder against
C-encoded frames covering the `lit_fmt = 3` (single-stream Huffman)
literal format.

Sprint 114 (v2.47.6): added when `lit_fmt = 3` support was ported
from `src/vv_huffman.c` to `reference/vv_huffman.py`. Closes part
of `AUDIT.md` Section 8 item 6 (reference-decoder coverage gap).

Workflow:
    1. Build a corpus of inputs (text, binary, mixed)
    2. Encode each with the C binary using `compat_v246_5_decoder = 1`
       (forces `lit_fmt = 3` instead of the default `lit_fmt = 4`)
    3. Decode each with the Python reference
    4. Assert byte-equality with original input

Invoked from the repo root by `make test_python_ref` or directly:
    python3 reference/test_lit_fmt_3.py

Note: this test requires `/tmp/encode_compat`, a small C tool that
wraps `vv_compress` with the compat flag set. Build it with:

    cc -Iinclude tests/encode_compat.c \\
        src/vv_encoder.c src/vv_xxh64.c src/vv_huffman.c \\
        src/vv_ans.c src/vaptvupt_api.c src/vv_simd.c \\
        src/vv_decoder.c -mavx2 -O2 -o tests/encode_compat

(The Python encoder cannot produce `lit_fmt = 3` directly because
it only emits RAW/RLE blocks. The standard CLI `./vaptvupt -c`
defaults to `lit_fmt = 4` for inputs ≥1024 literals.)
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import vv_decoder  # noqa: E402  (sys.path hack above)
import vv_huffman  # noqa: E402


# Locate the encode_compat tool and the silesia corpus
ENCODE_COMPAT_CANDIDATES = [
    os.path.join(HERE, "..", "tests", "encode_compat"),
    os.path.join(HERE, "..", "encode_compat"),
    "/tmp/encode_compat",
]


def find_encode_compat() -> str | None:
    for c in ENCODE_COMPAT_CANDIDATES:
        if os.path.isfile(c) and os.access(c, os.X_OK):
            return c
    return None


def test_huffman_exhaustion() -> None:
    """Keep short/long-code exhaustion behavior aligned with the C decoder."""
    for code_length in (1, 15):
        single = bytes([0, code_length << 4]) + bytes((4 * code_length + 7) // 8)
        decoded, consumed = vv_huffman.vvh_decode(single, len(single), 4)
        assert decoded == bytes(4) and consumed == len(single)
        try:
            vv_huffman.vvh_decode(single[:-1], len(single) - 1, 4)
        except vv_huffman.HuffmanError as exc:
            assert "exhausted" in str(exc)
        else:
            raise AssertionError("truncated single-stream Huffman accepted")

        lane_size = (code_length + 7) // 8
        four = bytes([0, code_length << 4]) + bytes([lane_size, 0, 0]) * 3
        four += bytes(4 * lane_size)
        decoded, consumed = vv_huffman.vvh_decode4(four, len(four), 4)
        assert decoded == bytes(4) and consumed == len(four)
        for lane in range(4):
            truncated = bytearray(four[:-1])
            if lane:
                truncated[2 + (lane - 1) * 3] -= 1
            try:
                vv_huffman.vvh_decode4(truncated, len(truncated), 4)
            except vv_huffman.HuffmanError:
                pass
            else:
                raise AssertionError(f"truncated Huffman lane {lane} accepted")
    print("  PASS Huffman input exhaustion (short/long codes, all four lanes)")


def main() -> int:
    test_huffman_exhaustion()
    if "--huffman-only" in sys.argv[1:]:
        return 0
    encode_compat = find_encode_compat()
    if encode_compat is None:
        print(
            "SKIP: encode_compat tool not found. Build with:\n"
            "  cc -Iinclude tests/encode_compat.c src/*.c -mavx2 -O2 \\\n"
            "    -o tests/encode_compat\n"
            f"Searched: {ENCODE_COMPAT_CANDIDATES}",
            file=sys.stderr,
        )
        return 77  # Autoconf "test skipped" convention

    # Build corpus of test inputs covering several content types and sizes
    cases: list[tuple[str, bytes]] = [
        ("tiny_repetitive_300B", b"abc" * 100),
        ("medium_text_4kb", b"The quick brown fox jumps over the lazy dog. " * 90),
        ("large_text_64kb", b"Hello, world!\n" * 4682),
        ("256_byte_ramp", bytes(range(256))),
        ("100kb_repeating_phrase",
            (b"the quick brown fox jumps over the lazy dog. " * 2222)[:100000]),
        ("structured_records",
            b"".join(f"record_{i}: value={i*1.5}, tag=item_{i % 50}; ".encode()
                    for i in range(5000))[:500000]),
    ]

    # Add some real silesia fixture slices if available
    silesia_dir = "/tmp/silesia"
    if os.path.isdir(silesia_dir):
        for f in ("dickens", "xml", "sao"):
            p = os.path.join(silesia_dir, f)
            if os.path.isfile(p):
                with open(p, "rb") as fh:
                    cases.append((f"silesia_{f}_64kb", fh.read(65536)))

    # Add a binary
    if os.path.isfile("/bin/bash"):
        with open("/bin/bash", "rb") as fh:
            cases.append(("bash_binary", fh.read()))

    print(f"Running {len(cases)} round-trip tests for `lit_fmt = 3`...\n")

    passed = 0
    failed = 0
    with tempfile.TemporaryDirectory() as td:
        in_path = os.path.join(td, "input.bin")
        vv_path = os.path.join(td, "encoded.vv")

        for name, data in cases:
            try:
                with open(in_path, "wb") as fh:
                    fh.write(data)
                result = subprocess.run(
                    [encode_compat, in_path, vv_path],
                    capture_output=True, timeout=30,
                )
                if result.returncode != 0:
                    print(f"  FAIL {name}: encode_compat returned "
                          f"{result.returncode}")
                    failed += 1
                    continue
                with open(vv_path, "rb") as fh:
                    encoded = fh.read()
                decoded = vv_decoder.decompress(encoded)
                if decoded == data:
                    ratio = len(encoded) / max(len(data), 1)
                    print(f"  PASS {name}: {len(data)} → {len(encoded)} "
                          f"bytes ({ratio*100:.1f}%)")
                    passed += 1
                else:
                    print(f"  FAIL {name}: decoded {len(decoded)} bytes, "
                          f"expected {len(data)}")
                    if len(decoded) == len(data):
                        for i in range(len(data)):
                            if decoded[i] != data[i]:
                                print(f"     first diff at byte {i}: "
                                      f"got {decoded[i]:#x}, "
                                      f"expected {data[i]:#x}")
                                break
                    failed += 1
            except NotImplementedError as e:
                print(f"  SKIP {name}: {str(e)[:100]}")
                # NotImplementedError isn't a test failure if the C
                # encoder happened to choose lit_fmt=4 anyway (the
                # 32-byte race tolerance can pick lit_fmt=4 even
                # with compat flag for some inputs).
                # But with compat flag explicitly set, we expect
                # lit_fmt=3 always. Treat as failure.
                failed += 1
            except Exception as e:
                print(f"  FAIL {name}: {type(e).__name__}: {str(e)[:120]}")
                failed += 1

    print()
    print(f"Results: {passed} passed, {failed} failed")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
