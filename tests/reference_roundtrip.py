#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""VaptVupt — reference-decoder default-format regression guard.

Sprint 134 found that both reference decoders (Python and JavaScript)
raised NotImplementedError on lit_fmt = 4 (HUFFMAN4) — the literal
format the encoder selects BY DEFAULT for blocks with >= 1024 literals
— so the differential cross-check silently never covered default
encoder output. This guard makes that gap impossible to reintroduce:

  1. Builds deterministic fixtures whose blocks are literal-heavy
     enough that the encoder picks HUFFMAN4 literals.
  2. Compresses each with ./vaptvupt in balanced and extreme mode.
  3. Walks the .vv container and asserts at least one 'S'/'T' block
     with lit_fmt == 4 was actually produced (no vacuous pass).
  4. Decodes every stream with the Python reference decoder and
     compares byte-for-byte against the original.
  5. If node is available, repeats the decode check with the
     JavaScript reference decoder (tests/reference_decode_check.js).

Exit 0 on success; non-zero with a diagnostic on any failure.
"""

import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
VV = os.environ.get("VV_BIN", os.path.join(ROOT, "vaptvupt"))
JS_CHECK = os.path.join(HERE, "reference_decode_check.js")

sys.path.insert(0, os.path.join(ROOT, "reference"))
import vv_decoder  # noqa: E402


# ─────────────────────────────────────────────────────────────────
# Fixtures — deterministic, literal-heavy so HUFFMAN4 wins the race.
# ─────────────────────────────────────────────────────────────────

def gen_pseudo_text(n=262144, seed=42):
    """Word soup with a wide vocabulary: many literals, some matches."""
    rng = random.Random(seed)
    words = [
        "the", "quick", "brown", "fox", "jumps", "over", "lazy", "dog",
        "compression", "entropy", "huffman", "stream", "block", "offset",
        "sequence", "literal", "match", "window", "decoder", "encoder",
    ] + [f"tok{i:03d}" for i in range(400)]
    parts = []
    size = 0
    while size < n:
        w = rng.choice(words)
        parts.append(w)
        size += len(w) + 1
    return (" ".join(parts))[:n].encode()


def gen_json_records(n_records=3000, seed=7):
    rng = random.Random(seed)
    lines = []
    for i in range(n_records):
        lines.append(
            '{"id":%d,"uuid":"%032x","score":%.4f,"tag":"t%d"}'
            % (i, rng.getrandbits(128), rng.random(), i % 50))
    return ("\n".join(lines)).encode()


def gen_terminal_ll_split():
    """Fixture whose final literal run crosses the 65535-byte LL ceiling."""
    data = bytearray(b"A" * 1048839)
    state = 42
    for _ in range(65536):
        state = (state * 1103515245 + 12345) & 0xFFFFFFFF
        data.append((state >> 16) & 0xFF)
    return bytes(data)


def gen_x86_bcj(n=65536):
    data = bytearray((i * 29 + 7) & 0xFF for i in range(n))
    for i in range(0, n - 4, 8):
        data[i:i + 5] = b"\xE8\x00\x00\x00\x00"
    return bytes(data)


def gen_arm64_bcj(n=65536):
    data = bytearray((i * 13 + 3) & 0xFF for i in range(n))
    for i in range(0, n - 3, 4):
        insn = 0x94000000 if i & 4 else 0x90000000
        data[i:i + 4] = insn.to_bytes(4, "little")
    return bytes(data)


FIXTURES = [
    ("pseudo-text", gen_pseudo_text()),
    ("json-records", gen_json_records()),
    ("terminal-ll-split", gen_terminal_ll_split()),
]

FILTER_FIXTURES = [
    ("bcj-x86", gen_x86_bcj(), ["--bcj"], 0x04, True),
    ("bcj-arm64", gen_arm64_bcj(), ["--bcj-arm64"], 0x08, True),
    ("bcj-x86-no-checksum", gen_x86_bcj(), ["--bcj", "--fast"], 0x04, False),
    ("bcj-arm64-no-checksum", gen_arm64_bcj(),
     ["--bcj-arm64", "--fast"], 0x08, False),
]


def gen_zero_progress_frame():
    """Malformed SEQ frame whose LL symbol never advances literal input."""
    payload = (
        struct.pack("<I", 1) + b"\x00" + struct.pack("<I", 1) + b"X" +
        struct.pack("<I", 0) + struct.pack("<H", 0) + struct.pack("<H", 0) +
        struct.pack("<H", 2) + b"\x01\x00" + b"\x00" * 6 +
        struct.pack("<I", 0)
    )
    header = struct.pack("<IBBBBQ", 0x56560100, 1, 0, 1, 16, 1)
    block_header = struct.pack("<I", 3 | (1 << 2) | (1 << 3))
    return header + block_header + (1 + len(payload)).to_bytes(3, "little") + \
        b"S" + payload


def token_frame(tokens, dsz, window_log=16, history=0, following_block=False):
    """Classic token payload with optional history and an adjacent RAW block."""
    frame = struct.pack("<IBBBBQ", 0x56560100, 1, 0, 0, window_log, history + dsz)
    if history:
        frame += struct.pack("<I", 2 | (history << 3)) + b"Q"
    frame += struct.pack("<I", 1 | (0 if following_block else 4) | (dsz << 3))
    frame += len(tokens).to_bytes(3, "little") + tokens
    if following_block:
        frame += struct.pack("<I", 4)  # Last, empty RAW block.
    return frame


def check_token_bounds(tmpdir, have_node):
    """Require rejection, not merely output mismatch, for malformed tokens."""
    fixtures = [
        ("missing-match-extension", token_frame(b"\x1fQ\x01\x00", 20), None),
        ("unterminated-match-extension", token_frame(b"\x1fQ\x01\x00\xff", 275), None),
        ("extension-crosses-block", token_frame(b"\x1fQ\x01\x00", 20,
                                               following_block=True), None),
        ("literal-crosses-block", token_frame(b"\x40Q", 4, following_block=True), None),
        ("offset-crosses-block", token_frame(b"\x10Q\x01", 5, history=2048,
                                            following_block=True), None),
        ("literal-exceeds-output", token_frame(b"\x50QQQQQ", 4), None),
        ("match-exceeds-output", token_frame(b"\x10Q\x01\x00", 4), None),
        ("trailing-incomplete-token", token_frame(b"\x10Q\x00", 1), None),
        ("terminated-zero-extension", token_frame(b"\x1fQ\x01\x00\x00", 20), b"Q" * 20),
        ("terminated-255-extension", token_frame(b"\x1fQ\x01\x00\xff\x00", 275), b"Q" * 275),
        ("exact-output", token_frame(b"\x10Q\x01\x00", 5), b"Q" * 5),
    ]
    for window_log in (10, 17):
        limit = 1 << window_log
        width = 2 if window_log <= 16 else 3
        for beyond in (0, 1):
            tokens = b"\x00" + (limit + beyond).to_bytes(width, "little")
            fixtures.append((f"window-{window_log}-beyond-{beyond}",
                             token_frame(tokens, 4, window_log, limit + 32),
                             None if beyond else b"Q" * (limit + 36)))
            # Single-symbol LL/ML/OF tables encode one match with offset
            # 2**window_log + beyond; only the offset extra bits vary.
            for tag, match_length in ((b"S", 4), (b"T", 3)):
                payload = b"\x00" * 9 + struct.pack("<I", 1)
                for symbol in (0, window_log + 3, 0):
                    payload += struct.pack("<HBB", 2, 1, symbol)
                payload += b"\x00" * 6 + struct.pack("<II", 4, beyond)
                frame = struct.pack("<IBBBBQ", 0x56560100, 1, 0, 1, window_log,
                                    limit + 32 + match_length)
                frame += struct.pack("<I", 2 | ((limit + 32) << 3)) + b"Q"
                frame += struct.pack("<I", 7 | (match_length << 3))
                frame += (len(payload) + 1).to_bytes(3, "little") + tag + payload
                fixtures.append((f"{tag.decode()}-window-{window_log}-beyond-{beyond}",
                                 frame, None if beyond else b"Q" * (limit + 32 + match_length)))

    failures = 0
    js_reject = """
const vv = require(process.argv[1]);
const fs = require('fs');
try { vv.decompress(new Uint8Array(fs.readFileSync(process.argv[2]))); process.exit(1); }
catch (e) { if (e.name !== 'CorruptError') { console.error(String(e)); process.exit(2); } }
"""
    for name, blob, expected in fixtures:
        comp = os.path.join(tmpdir, name + ".vv")
        plain = os.path.join(tmpdir, name + ".expected")
        with open(comp, "wb") as f:
            f.write(blob)
        with open(plain, "wb") as f:
            f.write(expected or b"")
        c = subprocess.run([VV, "-t", comp], capture_output=True, timeout=5)
        ok = (c.returncode == 0) == (expected is not None)
        try:
            out = bytes(vv_decoder.decompress(blob))
            ok &= expected is not None and out == expected
        except (vv_decoder.CorruptError, ValueError):
            ok &= expected is None
        if have_node:
            args = (["node", "-e", js_reject,
                     os.path.join(ROOT, "reference", "vv_decoder.js"), comp]
                    if expected is None else ["node", JS_CHECK, comp, plain])
            js = subprocess.run(args, capture_output=True, timeout=5)
            ok &= js.returncode == 0
        print(f"  {'ok  ' if ok else 'FAIL'}  {name}: C/Python/JS token bounds")
        failures += not ok
    return failures


# ─────────────────────────────────────────────────────────────────
# Container walk: collect lit_fmt values of 'S'/'T' entropy blocks.
# ─────────────────────────────────────────────────────────────────

def collect_lit_fmts(buf):
    """Walk frames/blocks per FORMAT.md; return list of lit_fmt bytes
    seen in 'S' (0x53) / 'T' (0x54) ENTROPY blocks."""
    fmts = []
    pos = 0
    n = len(buf)
    while pos + 16 <= n:
        magic = int.from_bytes(buf[pos:pos + 4], "little")
        if magic != 0x56560100:
            break
        flags = buf[pos + 5]
        pos += 16
        while pos + 4 <= n:
            bh = int.from_bytes(buf[pos:pos + 4], "little")
            btype = bh & 3
            last = (bh >> 2) & 1
            dsz = (bh >> 3) & 0x1FFFFF
            pos += 4
            if btype == 0:                       # RAW
                pos += dsz
            elif btype == 2:                     # RLE
                pos += 1
            else:                                # COMPRESSED / ENTROPY
                csz = buf[pos] | (buf[pos + 1] << 8) | (buf[pos + 2] << 16)
                pos += 3
                if btype == 3 and csz >= 6 and buf[pos] in (0x53, 0x54):
                    # payload: tag, [4B lit_count], [1B lit_fmt]
                    fmts.append(buf[pos + 5])
                pos += csz
            if last:
                break
        if flags & 1:
            pos += 12                            # footer
    return fmts


# ─────────────────────────────────────────────────────────────────
# Main
# ─────────────────────────────────────────────────────────────────

def main():
    if not (os.path.isfile(VV) and os.access(VV, os.X_OK)):
        sys.exit(f"FATAL: {VV} not found. Run `make` first.")
    have_node = shutil.which("node") is not None

    tmpdir = tempfile.mkdtemp(prefix="vv_refrt_")
    failures = 0
    huf4_seen = 0
    try:
        failures += check_token_bounds(tmpdir, have_node)
        for name, data in FIXTURES:
            plain = os.path.join(tmpdir, name)
            with open(plain, "wb") as f:
                f.write(data)
            for mode in ("balanced", "extreme"):
                comp = os.path.join(tmpdir, f"{name}.{mode}.vv")
                subprocess.run([VV, "-c", "-m", mode, "-o", comp, plain],
                               check=True, capture_output=True)
                blob = open(comp, "rb").read()

                fmts = collect_lit_fmts(blob)
                huf4_seen += sum(1 for x in fmts if x == 4)

                # Python reference decode, byte-exact
                try:
                    out = bytes(vv_decoder.decompress(blob))
                    if out != data:
                        print(f"  FAIL  {name}/{mode}: python reference "
                              f"mismatch ({len(out)} vs {len(data)} bytes)")
                        failures += 1
                    else:
                        print(f"  ok    {name}/{mode}: python byte-exact "
                              f"(lit_fmts={sorted(set(fmts))})")
                except Exception as e:                     # noqa: BLE001
                    print(f"  FAIL  {name}/{mode}: python reference raised "
                          f"{type(e).__name__}: {str(e)[:100]}")
                    failures += 1

                # JS reference decode, byte-exact
                if have_node:
                    r = subprocess.run(["node", JS_CHECK, comp, plain],
                                       capture_output=True, text=True)
                    if r.returncode != 0:
                        print(f"  FAIL  {name}/{mode}: js reference: "
                              f"{(r.stderr or r.stdout).strip()[:100]}")
                        failures += 1
                    else:
                        print(f"  ok    {name}/{mode}: js byte-exact")

        # Architecture filters checksum the forward-transformed bytes and
        # invert only after validation. Require both independent references
        # to reproduce the original caller-visible bytes.
        for name, data, options, expected_flag, expect_checksum in FILTER_FIXTURES:
            plain = os.path.join(tmpdir, name)
            comp = os.path.join(tmpdir, f"{name}.balanced.vv")
            with open(plain, "wb") as f:
                f.write(data)
            subprocess.run([VV, "-c", "-m", "balanced", *options,
                            "-o", comp, plain],
                           check=True, capture_output=True)
            blob = open(comp, "rb").read()
            if (len(blob) < 16 or (blob[5] & 0x0C) != expected_flag or
                    bool(blob[5] & 0x01) != expect_checksum):
                print(f"  FAIL  {name}: encoder flags differ from expected")
                failures += 1
                continue
            try:
                out = bytes(vv_decoder.decompress(blob))
                if out != data:
                    print(f"  FAIL  {name}: python BCJ inverse mismatch")
                    failures += 1
                else:
                    print(f"  ok    {name}: python BCJ byte-exact")
            except Exception as e:                     # noqa: BLE001
                print(f"  FAIL  {name}: python reference raised "
                      f"{type(e).__name__}: {str(e)[:100]}")
                failures += 1
            if have_node:
                r = subprocess.run(["node", JS_CHECK, comp, plain],
                                   capture_output=True, text=True)
                if r.returncode != 0:
                    print(f"  FAIL  {name}: js BCJ reference: "
                          f"{(r.stderr or r.stdout).strip()[:100]}")
                    failures += 1
                else:
                    print(f"  ok    {name}: js BCJ byte-exact")

        # A malformed LL-only SEQ can decode litlen=0 forever after all
        # matches are exhausted. All decoders must trip their progress bound
        # promptly rather than hang.
        zero_progress = os.path.join(tmpdir, "zero-progress.vv")
        expected = os.path.join(tmpdir, "zero-progress.expected")
        with open(zero_progress, "wb") as f:
            f.write(gen_zero_progress_frame())
        with open(expected, "wb") as f:
            f.write(b"X")
        try:
            c = subprocess.run([VV, "-t", zero_progress], timeout=2,
                               capture_output=True)
            if c.returncode == 0:
                print("  FAIL  zero-progress: C decoder accepted malformed frame")
                failures += 1
            else:
                print("  ok    zero-progress: C decoder rejected promptly")
        except subprocess.TimeoutExpired:
            print("  FAIL  zero-progress: C decoder timed out")
            failures += 1
        try:
            py = subprocess.run(
                [sys.executable, os.path.join(ROOT, "reference", "vv_decoder.py"),
                 zero_progress],
                timeout=2, capture_output=True)
            if py.returncode == 0:
                print("  FAIL  zero-progress: python decoder accepted malformed frame")
                failures += 1
            else:
                print("  ok    zero-progress: python decoder rejected promptly")
        except subprocess.TimeoutExpired:
            print("  FAIL  zero-progress: python decoder timed out")
            failures += 1
        if have_node:
            try:
                r = subprocess.run(["node", JS_CHECK, zero_progress, expected],
                                   timeout=2, capture_output=True, text=True)
                if r.returncode == 0:
                    print("  FAIL  zero-progress: js decoder accepted malformed frame")
                    failures += 1
                else:
                    print("  ok    zero-progress: js decoder rejected promptly")
            except subprocess.TimeoutExpired:
                print("  FAIL  zero-progress: js decoder timed out")
                failures += 1

        if huf4_seen == 0:
            print("  FAIL  no lit_fmt=4 (HUFFMAN4) block was produced — "
                  "the guard did not exercise the default literal format "
                  "(fixtures or encoder selection changed?)")
            failures += 1
        else:
            print(f"  lit_fmt=4 blocks exercised: {huf4_seen}")

        if not have_node:
            print("  (node not found — JS reference check skipped)")
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)

    if failures:
        print(f"\n✗ reference-decoder guard FAILED ({failures} failure(s))")
        return 1
    print("\n✓ reference decoders byte-exact on default-format output")
    return 0


if __name__ == "__main__":
    sys.exit(main())
