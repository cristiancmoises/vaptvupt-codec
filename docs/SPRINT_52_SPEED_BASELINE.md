# Sprint 52 — Speed-corner baseline + fast-entropy experiment

**Codec: v2.52.0 UNCHANGED (md5 93a73ccd…).** Measurement + experiment
sprint that establishes the SPEED PROGRAM baseline and the central design
constraint for a fast-entropy mode.

## Why this sprint

The RATIO PROGRAM (closed Sprint 50) optimized extreme-mode ratio. The
"ultra extreme fast compression/decompression" goal is a DIFFERENT target
— the speed corner (fast/balanced vs lz4 and zstd-1) — never worked
before. lz4 1.9.4 was installed (absent in earlier containers), enabling
the first honest lz4 comparison.

## Baseline (best-of-3, this machine)

```
dickens (10.2 MB):    ratio   dec MB/s         xml (5.3 MB):   ratio   dec MB/s
  vv fast             1.992      282              vv fast       5.254      384
  vv balanced         2.647      216              vv balanced   7.998      390
  lz4 -1              1.585      528              lz4 -1        4.352      399
  zstd -1             2.391      391              zstd -1       7.660      538

samba (21.6 MB):      ratio   dec MB/s
  vv fast             3.117      372
  vv balanced         4.084      291
  lz4 -1              2.798      577
  zstd -1             3.918      532
```

Encode (dickens): vv fast 53 MB/s, vv balanced 9, lz4-1 225, zstd-1 119.

## Two measured defects

1. **vv decode is never fastest** — 282-384 MB/s vs lz4 399-577 and zstd-1
   391-538 on every fixture. The token format (3-byte offsets, varint
   lengths) is heavier to parse than lz4's byte-aligned scheme. This is
   wire-neutral to fix (faster decoder, same format) and is the headline
   opportunity.

2. **vv fast mode has no entropy coding** — VV_MODE_ULTRA_FAST emits raw
   LZ tokens, so ratio (1.99 on dickens) loses to zstd-1 (2.39). It is
   Pareto-dominated by zstd-1 (smaller AND faster both ways). No-man's-
   land: heavier format than lz4, no entropy like zstd-1.

## Fast-entropy experiment (env-gated, reverted)

Enabled the existing SEQ entropy path for ultra-fast mode. Required
allocating the entropy buffers (only allocated for mode>=BALANCED) — the
first attempt silently failed with VVA_ERR_OVERFLOW because ent_buf was
NULL, a reminder to verify the path actually engages, not just builds.

Once buffers were allocated:

```
dickens:                ratio   enc MB/s   dec MB/s   rt
  vv fast (shipped)     1.992       53        282     --
  vv fast + SEQ (expt)  2.470       20        185     OK   ← beats zstd-1 ratio, loses both speeds
  zstd -1               2.391      119        411     --
```

**The SEQ/ANS coder is too slow on BOTH ends to make a *fast* mode.** It
produces a balanced-like point (vv balanced is already 2.65 ratio). The
experiment proves entropy CAN lift fast-mode ratio above zstd-1 with
correct roundtrip — but via the wrong mechanism.

## Central design constraint (for SPEED_PROGRAM_PROMPT Lever S2)

A fast entropy mode must use **lightweight Huffman-only literal coding
with raw (non-entropy) sequences**, not the full ANS SEQ path. Huffman
decode is far cheaper than ANS, and the existing 4-stream Huffman
(`vvh_encode4`) is the building block. Offsets/lengths stay raw to keep
decode fast. This is a new wire tag (versioned, gated, fuzzed) — scoped in
the prompt.

## Decision

Reverted to clean v2.52.0. Experiment WIP saved at
/tmp/sprint52_fastent_experiment_vv_encoder.c. The baseline and the
"Huffman-only, not ANS" constraint are the foundation for the SPEED
PROGRAM. First sprint: Lever S1 (decode-path profiling/optimization) — the
highest-value, wire-neutral win.
