# Integrating the VaptVupt codec into a host application

This is the integration reference for embedding the VaptVupt compression codec
as the compression layer beneath an application's encryption envelope (for
example, a backup tool that wraps each frame in AES-256-GCM or an ML-KEM + AEAD
construction). It covers the API, the build, the flags that matter, and the
threat-model boundary. Numbers here point to measured data, not headline
claims — see `bench/COMPARISON.md` for the current full-Silesia measurement.

License: this codec library is GPL-3.0-or-later; the VaptVupt tool (formerly
Zupt) is dual-licensed AGPL-3.0 + commercial (contact sac@securityops.co).

---

## Five integration points

1. **Link against the amalgamation.** `make amalgamation` emits
   `build/vaptvupt.c` + `build/vaptvupt.h` — a single translation unit, no
   Makefile wiring required. Compile `build/vaptvupt.c` with `-mavx2` (or your
   target's SIMD flag) and include `build/vaptvupt.h`.

2. **Skip the checksum on decode when an AEAD already authenticates the bytes.**
   If the host wraps the compressed frame in an AEAD (GCM, ChaCha20-Poly1305),
   the outer tag already authenticates the ciphertext, so the codec's XXH64
   footer is redundant work on decode. Pass `VV_DECOMPRESS_SKIP_CHECKSUM`.
   Decode speedups are largest on high-entropy data; re-measure on your inputs.

3. **Drop the checksum on encode in the same situation.** Set
   `opts.checksum = 0` so the encoder omits the XXH64 footer. Pair it with
   `VV_DECOMPRESS_SKIP_CHECKSUM` on decode for the full saving. Only do this
   when an outer AEAD provides integrity — see the threat model below.

4. **Binary-heavy data gets `format_v2` automatically since v2.61.0.** The
   encoder auto-enables min_match=3 ('T' blocks) for binary-detected input in
   balanced/extreme, where it is a measured ratio win; text keeps 'S'. You
   only need `opts.format_v2 = 1` to *force* it for data the detector
   misses, and `opts.compat_v246_5_decoder = 1` to *suppress* it when your
   decode side may be older than v2.33.0. Measure per data class.

5. **Treat any non-OK decode return as a frame-level reject.** Do not attempt
   recovery inside the codec boundary. Propagate the error to the host's
   transaction layer and let it retry from the previous good snapshot.

---

## Minimal API

```c
#include "vaptvupt.h"

/* Encode */
vv_options_t opts;
vv_default_options(&opts);
opts.mode     = VV_MODE_BALANCED;   /* ULTRA_FAST | BALANCED | EXTREME      */
opts.checksum = 0;                  /* outer AEAD authenticates the bytes   */
/* opts.format_v2  = 1; */          /* force v2; auto for binary since v2.61 */
/* opts.filter_auto = 1; */         /* optional: auto x86/ARM64 BCJ on ELF  */

int64_t n = vv_compress(src, src_len, dst, dst_cap, &opts);
if (n < 0) { /* handle VV_ERR_*; insufficient dst is VV_ERR_DST_TOO_SMALL */ }

/* Decode (untrusted input) */
int64_t m = vv_decompress_ex(comp, comp_len, out, out_cap,
                             VV_DECOMPRESS_SKIP_CHECKSUM);
if (m < 0) { /* any negative return: reject this frame, do not recover */ }
```

Bound `dst_cap` with `vv_compress_bound(src_len)`. The decoder enforces its own
output and work limits; a hostile frame yields a clean negative return, never a
crash (see below).

---

## Build targets

- `make amalgamation` — single-file `build/vaptvupt.{c,h}` for drop-in embedding.
- `make` — the `vaptvupt` CLI and the static library pieces, `-Wall -Wextra -Werror`.
- `make test` — full suite (C suites, reference decoders, differential fuzzer,
  negative corpus, ratio gate, OOM sweep). Allow >= 850 s.
- `make verify` — CBMC proofs + Frama-C/Eva analyses of the BCJ filters and the
  decoder's length reader and block-header codec (needs `cbmc`, optionally
  `frama-c-base` + `z3`).

If you vendor `src/` directly instead of the amalgamation, run
`make amalgamation-check` in CI: it rebuilds the amalgamation and fails if it
has drifted from `src/`, so a security fix in `src/` cannot silently miss the
embedded copy.

---

## Threat model (state this to your users)

- A compressed frame (extension `.zupt`) provides **no confidentiality and no
  authentication on its own.** The XXH64 footer is an integrity check against
  accidental corruption, not a MAC: it does not detect deliberate forgery.
- For tamper-resistance the host **must** wrap the compressed bytes in an AEAD
  (AES-256-GCM, XChaCha20-Poly1305) or an authenticated PQ construction. When
  it does, the codec's checksum is redundant and can be skipped (points 2-3).
- What the codec alone guarantees on untrusted input is **safe decompression**:
  a malformed or adversarial frame results in a clean negative return code or a
  bounded resource use, never an out-of-bounds access, use-after-free, or
  unbounded hang. Enforced by the differential fuzzer, the ASan/UBSan
  corrupt-input sweep, the DoS reproducers, and the CBMC/Eva proofs of the
  parse hot path (see `SECURITY.md`).
- The incoming-frame decode path is the primary at-risk consumer in a backup
  pipeline; keep it behind the AEAD and treat decode errors as rejects.
