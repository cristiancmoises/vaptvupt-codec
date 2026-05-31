# Sprint 45 — Negative result: large-window scaling breaks roundtrip

**Codec shipped: v2.51.1 UNCHANGED (md5 50e74ea832f71fa21754bcec75fd8c99).**
The large-window change was reverted.

## Goal
Extreme mode caps the window at wlog≈18-20 (256KB-1MB) via the adaptive
trial. Multi-MB fixtures with long-range structure (nci 33MB, webster
41MB, mozilla 51MB) lose matches a larger window would capture. Scale
wlog with file size up to 2^24 (the 3-byte offset limit).

## Measured potential (real, but from a binary that fails roundtrip)
- nci:     2,947,193 → 2,566,597  (-12.9%)  ← would flip nci from loss to win
- dickens: 3,555,103 → 3,406,822  (-4.2%)
- x-ray:   ~5.5MB compressed (improved)

These are large gains. The lever is worth getting right (see Lever A in
PROGRAM_PROMPT.md).

## The bug
Raising wlog to 24 broke roundtrip even on a 9MB file (dickens), which
rules out the obvious hypotheses:
- NOT 3-byte offset overflow: 3 bytes hold 2^24 = 16MB; dickens offsets
  are < 9MB.
- NOT >16MB chain aliasing: dickens (9MB) has no aliasing at wlog=24 and
  still fails.

Decode returns -2 (generic corrupt). The bug is in the large-window encode
path being exercised for the first time — the old wlog≤20 extreme path
rarely emitted offsets in the 1-16MB range that the larger window now
produces frequently.

## Root-cause candidates (for the fixing sprint)
1. Decoder match-copy at large offsets (`vv_decoder.c` ~181-248) — a path
   only exercised when the encoder emits offset ≥ ~1MB, which the prior
   extreme path didn't.
2. `opt_collect` chain validation — verify the candidate's full match is
   re-validated before emit, not just the 4-byte `pos4==b` probe.
3. off_bytes mismatch between encoder and decoder at the larger window.

## Decision
Reverted to shipped v2.51.1. WIP saved at
`/tmp/vv_encoder_sprint45_largewindow_wip.c`. The fix is the recommended
next sprint (Lever A in PROGRAM_PROMPT.md): binary-search the smallest
failing input, run under ASan/UBSan to pinpoint the OOB/logic error, fix,
re-validate, ship as v2.52.0.

## Why this is a complete sprint
Per the program rules: a ratio win that breaks roundtrip does not ship.
This sprint produced (a) a precise measurement of the lever's potential,
(b) a reproducible bug with a bounded root-cause list, (c) the saved WIP,
and (d) the next-step diagnosis. Shipping a corrupt codec to claim a ratio
win would violate the core discipline.
