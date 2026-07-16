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


FIXTURES = [
    ("pseudo-text", gen_pseudo_text()),
    ("json-records", gen_json_records()),
]


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
