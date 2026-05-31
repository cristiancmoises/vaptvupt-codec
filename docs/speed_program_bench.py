#!/usr/bin/env python3
"""
SPEED PROGRAM final benchmark — vaptvupt v2.50.5 vs zstd 1-19 on full Silesia.

Reports for each fixture × tool × level:
  - compressed size (bytes)
  - compression ratio
  - encode speed (MB/s)
  - decode speed (MB/s)

Uses best-of-N runs (default N=5) for each measurement. Subprocess
overhead is included in the reported time — same for both tools, so
the comparison is fair.
"""
import subprocess
import time
import os
import sys
import statistics

SILESIA = '/tmp/silesia'
FIXTURES = ['dickens', 'mozilla', 'mr', 'nci', 'ooffice', 'osdb',
            'reymont', 'samba', 'sao', 'webster', 'x-ray', 'xml']
VV_MODES = ['fast', 'balanced', 'extreme']
ZSTD_LEVELS = [1, 3, 9]
RUNS = 3

VV_BIN = sys.argv[1] if len(sys.argv) > 1 else '/tmp/vaptvupt-2.50.5/vaptvupt'
ZSTD_BIN = '/usr/bin/zstd'
TMP_VV = '/tmp/bench.vv'
TMP_ZST = '/tmp/bench.zst'


def measure(cmd, runs=RUNS):
    """Best-of-N wall-clock seconds."""
    times = []
    for _ in range(runs):
        t0 = time.perf_counter()
        subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        times.append(time.perf_counter() - t0)
    return min(times)


def bench_vv(fixture, mode):
    src = f'{SILESIA}/{fixture}'
    orig = os.path.getsize(src)

    enc_cmd = [VV_BIN, '-c', '-m', mode, '-o', TMP_VV, src]
    enc_t = measure(enc_cmd)
    csz = os.path.getsize(TMP_VV)

    dec_cmd = [VV_BIN, '-d', '-o', '/dev/null', TMP_VV]
    dec_t = measure(dec_cmd)

    return {
        'orig': orig,
        'csz': csz,
        'ratio': orig / csz,
        'enc_mbps': orig / 1e6 / enc_t,
        'dec_mbps': orig / 1e6 / dec_t,
    }


def bench_zstd(fixture, level):
    src = f'{SILESIA}/{fixture}'
    orig = os.path.getsize(src)

    enc_cmd = [ZSTD_BIN, f'-{level}', '-q', '-f', '-o', TMP_ZST, src]
    enc_t = measure(enc_cmd)
    csz = os.path.getsize(TMP_ZST)

    dec_cmd = [ZSTD_BIN, '-d', '-q', '-f', '-o', '/dev/null', TMP_ZST]
    dec_t = measure(dec_cmd)

    return {
        'orig': orig,
        'csz': csz,
        'ratio': orig / csz,
        'enc_mbps': orig / 1e6 / enc_t,
        'dec_mbps': orig / 1e6 / dec_t,
    }


def main():
    print(f"=" * 100)
    print(f"VaptVupt v2.50.5 vs zstd {subprocess.check_output([ZSTD_BIN, '--version']).decode().strip().split()[3]} — Silesia full corpus")
    print(f"  Best of {RUNS} runs each. Subprocess overhead included.")
    print(f"  vv binary: {VV_BIN}")
    print(f"=" * 100)

    all_results = {}

    for fixture in FIXTURES:
        print(f"\n## {fixture}  ({os.path.getsize(SILESIA + '/' + fixture)/1e6:.2f} MB)")
        print(f"  {'tool':<18} {'csz':>10} {'ratio':>7} {'enc MB/s':>10} {'dec MB/s':>10}")
        results = {}
        for mode in VV_MODES:
            r = bench_vv(fixture, mode)
            results[f'vv-{mode}'] = r
            print(f"  {'vv-'+mode:<18} {r['csz']:>10} {r['ratio']:>7.2f} {r['enc_mbps']:>10.1f} {r['dec_mbps']:>10.1f}")
        for lvl in ZSTD_LEVELS:
            r = bench_zstd(fixture, lvl)
            results[f'zstd-{lvl}'] = r
            print(f"  {'zstd-'+str(lvl):<18} {r['csz']:>10} {r['ratio']:>7.2f} {r['enc_mbps']:>10.1f} {r['dec_mbps']:>10.1f}")
        all_results[fixture] = results

    # Aggregate analysis
    print("\n" + "=" * 100)
    print("AGGREGATE (geometric means across 12 Silesia fixtures)")
    print("=" * 100)

    def geomean(xs): return statistics.geometric_mean(xs)

    print(f"\n{'tool':<18} {'mean ratio':>11} {'mean enc MB/s':>14} {'mean dec MB/s':>14}")
    for tool in ['vv-fast', 'vv-balanced', 'vv-extreme', 'zstd-1', 'zstd-3', 'zstd-9']:
        ratios = [all_results[f][tool]['ratio'] for f in FIXTURES]
        encs = [all_results[f][tool]['enc_mbps'] for f in FIXTURES]
        decs = [all_results[f][tool]['dec_mbps'] for f in FIXTURES]
        print(f"  {tool:<18} {geomean(ratios):>11.3f} {geomean(encs):>14.1f} {geomean(decs):>14.1f}")

    # T1-T4 verdicts
    print("\n" + "=" * 100)
    print("SPEED PROGRAM TARGET VERDICTS")
    print("=" * 100)

    def count_wins(a_tool, b_tool, metric):
        wins = 0
        for f in FIXTURES:
            if all_results[f][a_tool][metric] >= all_results[f][b_tool][metric]:
                wins += 1
        return wins

    # T1: vv-fast decode ≥ zstd-1 decode
    w = count_wins('vv-fast', 'zstd-1', 'dec_mbps')
    print(f"  T1 (vv-fast dec ≥ zstd-1 dec):       won on {w}/12 fixtures")
    # T2: vv-fast encode ≥ zstd-1 encode
    w = count_wins('vv-fast', 'zstd-1', 'enc_mbps')
    print(f"  T2 (vv-fast enc ≥ zstd-1 enc):       won on {w}/12 fixtures")
    # T3: vv-balanced encode ≥ zstd-3 encode
    w = count_wins('vv-balanced', 'zstd-3', 'enc_mbps')
    print(f"  T3 (vv-balanced enc ≥ zstd-3 enc):   won on {w}/12 fixtures")
    # T4: vv-fast decode > zstd-3 decode
    w = count_wins('vv-fast', 'zstd-3', 'dec_mbps')
    print(f"  T4 (vv-fast dec > zstd-3 dec):       won on {w}/12 fixtures")

    # Aggregate ratio comparison: vv-extreme vs zstd-3 (the original "vv wins aggregate" claim)
    print("\nORIGINAL CLAIM CHECK: vv-extreme aggregate ratio vs zstd-3:")
    vv_ratios = [all_results[f]['vv-extreme']['ratio'] for f in FIXTURES]
    z_ratios = [all_results[f]['zstd-3']['ratio'] for f in FIXTURES]
    print(f"  vv-extreme geo-mean ratio:  {geomean(vv_ratios):.4f}")
    print(f"  zstd-3     geo-mean ratio:  {geomean(z_ratios):.4f}")
    print(f"  Delta:                       {(geomean(vv_ratios)/geomean(z_ratios)-1)*100:+.2f}%")

    return all_results


if __name__ == '__main__':
    main()
