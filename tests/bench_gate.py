#!/usr/bin/env python3
"""
VaptVupt — Compression Ratio Benchmark Gate.

Compresses a fixed set of synthetic fixtures with vaptvupt in each
mode and compares the output size against:
  1. A locked baseline (`tests/bench_baseline.json`, committed).
  2. gzip -9 on the same input, as an external sanity check.

The script fails (non-zero exit) if:
  - Any tested input compresses to more than `tolerance_bytes` larger
    than the committed baseline (default: 0 bytes — strict regression
    detection).
  - Ratios of extreme mode < balanced mode (contract violation, the
    v2.24.0 bug class).

Baseline file format (JSON):
  {
    "fixture_name": {
      "input_size": 1234,
      "ultra_fast": 567,
      "balanced": 345,
      "extreme": 234,
      "gzip9": 400
    },
    ...
  }

Usage:
    python3 tests/bench_gate.py                  # check against baseline
    python3 tests/bench_gate.py --update         # regenerate baseline
    python3 tests/bench_gate.py --tolerance 8    # allow +8 byte slack per fixture

On first run, or after intentional codec changes that change ratios,
use `--update` to write a new baseline. Review the git diff carefully
before committing — every change in the baseline is a shipped ratio
delta that users will experience.
"""

import argparse
import json
import os
import random
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
VV_ROOT = os.path.dirname(HERE)
VV_BINARY = os.path.join(VV_ROOT, 'vaptvupt')
BASELINE_PATH = os.path.join(HERE, 'bench_baseline.json')


# ─────────────────────────────────────────────────────────────────
# Fixture generators — deterministic, reproducible.
# ─────────────────────────────────────────────────────────────────

def _seeded_random(seed):
    """Return a random.Random with fixed seed — portable across Pythons."""
    return random.Random(seed)


def gen_text_simple(size_words=10000):
    """Lorem-like English text with small vocabulary."""
    rng = _seeded_random(42)
    words = ('the quick brown fox jumps over the lazy dog lorem ipsum '
             'dolor sit amet consectetur adipiscing elit').split()
    return ' '.join(rng.choice(words) for _ in range(size_words)).encode('utf-8')


def gen_text_varied(size_words=8000):
    """Text with a different word set for variety."""
    rng = _seeded_random(77)
    words = ('apple banana cherry date elderberry fig grape honeydew '
             'kiwi lemon mango nectarine orange peach quince raspberry').split()
    return ' '.join(rng.choice(words) for _ in range(size_words)).encode('utf-8')


def gen_text_large(size_words=15000):
    """Larger text corpus."""
    rng = _seeded_random(999)
    words = ('hello world foo bar baz qux quux corge grault garply waldo '
             'fred plugh xyzzy thud the and but or if then else').split()
    return ' '.join(rng.choice(words) for _ in range(size_words)).encode('utf-8')


def gen_json_small(n=1000):
    """JSON records with short schema."""
    rng = _seeded_random(42)
    items = [{'id': i, 'name': f'item_{i}',
              'category': rng.choice(['a', 'b', 'c'])}
             for i in range(n)]
    return json.dumps(items).encode('utf-8')


def gen_json_mixed(n=2000):
    """JSON with numeric + string + repeating tags."""
    rng = _seeded_random(7)
    items = [{'id': i, 'value': rng.random(), 'tag': f'tag_{i % 20}'}
             for i in range(n)]
    return json.dumps(items).encode('utf-8')


def gen_repeating(size=10000, pattern=b'the quick brown fox '):
    """Pure repetition — should compress extremely well."""
    return (pattern * (size // len(pattern) + 1))[:size]


def gen_binary_pattern(size=100000):
    """Deterministic binary-ish data via arithmetic progression."""
    return bytes(((i * 17 + (i >> 3)) & 0xFF) for i in range(size))


def gen_random(size=50000):
    """Pseudo-random bytes (incompressible)."""
    rng = _seeded_random(1)
    return bytes(rng.randrange(256) for _ in range(size))


def gen_csv(rows=2000):
    """CSV-like table with repeating schema."""
    rng = _seeded_random(13)
    lines = ['id,name,category,value,timestamp']
    for i in range(rows):
        lines.append(f'{i},item_{i},{rng.choice(["A","B","C","D"])},'
                     f'{rng.random():.4f},2026-04-{(i % 30) + 1:02d}')
    return '\n'.join(lines).encode('utf-8')


def gen_source_like(reps=100):
    """Source-code-like input with keywords and repeating structure."""
    template = '''
    static int func_{n}(const uint8_t *src, size_t src_len,
                         uint8_t *dst, size_t dst_cap, size_t *out_len) {{
        if (!src || !dst) return VV_ERR_PARAM;
        if (src_len > dst_cap) return VV_ERR_OVERFLOW;
        for (size_t i = 0; i < src_len; i++) {{
            dst[i] = src[i] ^ 0x{x:02X};
        }}
        *out_len = src_len;
        return VV_OK;
    }}
'''
    return ''.join(template.format(n=i, x=i & 0xFF)
                   for i in range(reps)).encode('utf-8')


# Fixture registry. Names must stay stable — they're keys in the baseline.
FIXTURES = [
    ('text-simple',      gen_text_simple,    {}),
    ('text-varied',      gen_text_varied,    {}),
    ('text-large',       gen_text_large,     {}),
    ('json-small',       gen_json_small,     {}),
    ('json-mixed',       gen_json_mixed,     {}),
    ('repeating',        gen_repeating,      {}),
    ('binary-pattern',   gen_binary_pattern, {}),
    ('random',           gen_random,         {}),
    ('csv',              gen_csv,            {}),
    ('source-like',      gen_source_like,    {}),
]


# ─────────────────────────────────────────────────────────────────
# Measurement
# ─────────────────────────────────────────────────────────────────

def compress_with_vaptvupt(data, mode):
    """Return output size of compressing `data` with `vaptvupt -m <mode>`."""
    # Keep the historical JSON key, but use the actual CLI mode name.
    # Older CLI versions silently treated "ultra_fast" as balanced.
    cli_mode, mode_hint = {
        'ultra_fast': ('fast', 0),
        'balanced': ('balanced', 1),
        'extreme': ('extreme', 2),
    }[mode]
    with tempfile.NamedTemporaryFile(delete=False) as f:
        f.write(data)
        in_path = f.name
    out_path = in_path + '.vv'
    try:
        subprocess.run([VV_BINARY, '-c', '-m', cli_mode, '-o', out_path, in_path],
                       check=True, capture_output=True)
        with open(out_path, 'rb') as output:
            header = output.read(16)
        if len(header) != 16 or header[6] != mode_hint:
            raise RuntimeError(f'{mode}: encoder did not emit requested mode {mode_hint}')
        return os.path.getsize(out_path)
    finally:
        for p in (in_path, out_path):
            try:
                os.remove(p)
            except OSError:
                pass


def compress_with_gzip9(data):
    """Return size of `gzip -9` output for `data`."""
    with tempfile.NamedTemporaryFile(delete=False) as f:
        f.write(data)
        path = f.name
    try:
        result = subprocess.run(['gzip', '-9', '-c', path],
                                capture_output=True, check=True)
        return len(result.stdout)
    finally:
        try:
            os.remove(path)
        except OSError:
            pass


def measure_fixture(name, gen_fn, kwargs):
    """Run all modes on one fixture. Returns a dict of sizes."""
    data = gen_fn(**kwargs)
    record = {'input_size': len(data)}
    for mode in ('ultra_fast', 'balanced', 'extreme'):
        try:
            record[mode] = compress_with_vaptvupt(data, mode)
        except subprocess.CalledProcessError as e:
            sys.exit(f"vaptvupt failed on {name} mode={mode}: {e.stderr.decode()}")
    try:
        record['gzip9'] = compress_with_gzip9(data)
    except FileNotFoundError:
        record['gzip9'] = None  # gzip not available
    return record


def measure_all(fixtures):
    return {name: measure_fixture(name, fn, kw) for name, fn, kw in fixtures}


# ─────────────────────────────────────────────────────────────────
# Gate logic
# ─────────────────────────────────────────────────────────────────

def check_contract_violations(current, baseline=None):
    """Verify extreme <= balanced across all fixtures.

    Returns (new_violations, known_violations). A violation is "known"
    if it was also present in the baseline (documented trade-off);
    "new" if it appeared in this run but not in baseline (regression).
    Only new violations should fail the gate.
    """
    new_violations = []
    known_violations = []
    for name, rec in current.items():
        bal = rec.get('balanced')
        ext = rec.get('extreme')
        if bal is not None and ext is not None and ext > bal:
            msg = (f"{name}: extreme ({ext}) > balanced ({bal}) — "
                   f"contract says extreme should never produce larger output")
            if baseline and name in baseline:
                b_bal = baseline[name].get('balanced')
                b_ext = baseline[name].get('extreme')
                if b_bal is not None and b_ext is not None and b_ext > b_bal:
                    known_violations.append(msg)
                    continue
            new_violations.append(msg)
    return new_violations, known_violations


def check_regressions(current, baseline, tolerance_bytes):
    """Compare current to baseline. Returns list of regression messages."""
    regressions = []
    missing = []
    improvements = []

    for name, cur_rec in current.items():
        if name not in baseline:
            missing.append(f"{name}: new fixture, not in baseline")
            continue
        base_rec = baseline[name]
        for mode in ('ultra_fast', 'balanced', 'extreme'):
            cur = cur_rec.get(mode)
            base = base_rec.get(mode)
            if cur is None or base is None:
                continue
            delta = cur - base
            if delta > tolerance_bytes:
                regressions.append(
                    f"{name}/{mode}: {cur} bytes, baseline {base} "
                    f"(+{delta} bytes regression, tolerance={tolerance_bytes})")
            elif delta < -10:  # track improvements ≥10 bytes
                improvements.append(
                    f"{name}/{mode}: {cur} bytes, baseline {base} "
                    f"({delta} bytes improvement)")
    return regressions, improvements, missing


# ─────────────────────────────────────────────────────────────────
# Output formatting
# ─────────────────────────────────────────────────────────────────

def print_table(current):
    print(f"\n  {'fixture':<18s} {'input':>8s} {'ult_fast':>9s} "
          f"{'balanced':>9s} {'extreme':>9s} {'gzip-9':>8s}")
    print('  ' + '-' * 70)
    for name, rec in current.items():
        gz = rec.get('gzip9')
        gz_str = f"{gz:>8d}" if gz is not None else f"{'n/a':>8s}"
        print(f"  {name:<18s} {rec['input_size']:>8d} "
              f"{rec['ultra_fast']:>9d} {rec['balanced']:>9d} "
              f"{rec['extreme']:>9d} {gz_str}")


# ─────────────────────────────────────────────────────────────────
# Main
# ─────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--update', action='store_true',
                    help='Regenerate baseline from current results')
    ap.add_argument('--tolerance', type=int, default=0,
                    help='Allowed regression per fixture, in bytes (default: 0)')
    ap.add_argument('--show-improvements', action='store_true',
                    help='Print fixtures that improved vs baseline')
    args = ap.parse_args()

    if not (os.path.isfile(VV_BINARY) and os.access(VV_BINARY, os.X_OK)):
        sys.exit(f"FATAL: {VV_BINARY} not found. Run `make` first.")

    print(f"Measuring {len(FIXTURES)} fixtures across 3 modes + gzip-9...")
    current = measure_all(FIXTURES)
    print_table(current)

    # Load baseline if present (needed for both violation check and regression check)
    baseline = None
    if os.path.isfile(BASELINE_PATH):
        with open(BASELINE_PATH) as f:
            baseline = json.load(f)

    # Check contract violations (extreme ≤ balanced). Split into new vs known.
    new_violations, known_violations = check_contract_violations(current, baseline)

    if known_violations:
        print("\n  ⓘ KNOWN contract violations (documented in baseline):")
        for v in known_violations:
            print(f"    {v}")

    if new_violations:
        print("\n  ✗ NEW CONTRACT VIOLATIONS:")
        for v in new_violations:
            print(f"    {v}")

    if args.update:
        with open(BASELINE_PATH, 'w') as f:
            json.dump(current, f, indent=2, sort_keys=True)
            f.write('\n')
        print(f"\n  Baseline updated → {BASELINE_PATH}")
        print(f"  Review the diff carefully before committing — every")
        print(f"  change is a shipped ratio delta users will experience.")
        return 1 if new_violations else 0

    if baseline is None:
        print(f"\n  No baseline found at {BASELINE_PATH}.")
        print(f"  Run with --update to create one.")
        return 1 if new_violations else 0

    regressions, improvements, missing = check_regressions(
        current, baseline, args.tolerance)

    if missing:
        print("\n  ⓘ New fixtures (not in baseline):")
        for m in missing:
            print(f"    {m}")

    if args.show_improvements and improvements:
        print("\n  ✓ IMPROVEMENTS vs baseline:")
        for i in improvements:
            print(f"    {i}")

    if regressions:
        print(f"\n  ✗ {len(regressions)} REGRESSION(S) vs baseline "
              f"(tolerance={args.tolerance}):")
        for r in regressions:
            print(f"    {r}")

    fail = bool(new_violations) or bool(regressions)
    if fail:
        print("\n  ✗ Gate FAILED.")
        if regressions:
            print("    To accept these changes: run with --update, review")
            print("    the baseline diff carefully, then commit.")
        return 1
    else:
        print(f"\n  ✓ Gate passed. All {len(current)} fixtures within "
              f"baseline ± {args.tolerance} bytes.")
        return 0


if __name__ == '__main__':
    sys.exit(main())
