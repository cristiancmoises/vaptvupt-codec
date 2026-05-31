#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# CLI test for -w/--window: roundtrip across windows, validation, and the
# invariant that omitting -w is byte-identical to the default.
import os, subprocess, sys, tempfile

VV = os.environ.get("VV_BIN", "./vaptvupt")

def run(args, data=None):
    return subprocess.run([VV] + args, input=data, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE)

def main():
    if not os.path.exists(VV):
        print(f"  cli_window SKIP: {VV} not found"); return 0
    # build a compressible input with some long-range structure
    blk = (b"securityops vaptvupt codec line %d padding padding\n" % 0)
    data = b"".join(b"securityops vaptvupt codec line %d xyz\n" % (i % 4096) for i in range(60000))
    fails = 0
    with tempfile.TemporaryDirectory() as d:
        inp = os.path.join(d, "in.bin"); open(inp, "wb").write(data)
        # 1) roundtrip across a range of windows
        for w in (10, 12, 16, 20, 22, 24):
            cvv = os.path.join(d, f"c{w}.vv"); out = os.path.join(d, f"o{w}.bin")
            r = run(["-c", "-m", "balanced", "-w", str(w), "-o", cvv, inp])
            if r.returncode != 0:
                print(f"  FAIL: compress -w {w} rc={r.returncode}"); fails += 1; continue
            r = run(["-d", "-o", out, cvv])
            if r.returncode != 0 or open(out, "rb").read() != data:
                print(f"  FAIL: roundtrip -w {w}"); fails += 1
        # 2) validation: out-of-range must be rejected (nonzero exit)
        for bad in ("9", "25", "100"):
            r = run(["-c", "-w", bad, "-o", os.path.join(d, "x.vv"), inp])
            if r.returncode == 0:
                print(f"  FAIL: -w {bad} should be rejected"); fails += 1
        # 3) -w 0 == omitting -w (byte-identical, auto)
        a = os.path.join(d, "a.vv"); b = os.path.join(d, "b.vv")
        run(["-c", "-m", "balanced", "-o", a, inp])
        run(["-c", "-m", "balanced", "-w", "0", "-o", b, inp])
        if open(a, "rb").read() != open(b, "rb").read():
            print("  FAIL: -w 0 differs from default"); fails += 1
    if fails == 0:
        print("  cli_window PASS (roundtrip 6 windows, validation, -w 0 == default)")
        return 0
    print(f"  cli_window: {fails} failure(s)"); return 1

if __name__ == "__main__":
    sys.exit(main())
