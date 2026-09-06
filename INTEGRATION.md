# Integrating the VaptVupt codec into a host application

This is the integration reference for embedding the VaptVupt compression codec
as the compression layer beneath an application's encryption envelope (for
example, a backup tool that wraps each frame in AES-256-GCM or an ML-KEM + AEAD
construction). It covers the API, the build, the flags that matter, and the
threat-model boundary. Numbers here point to measured data, not headline
claims — see `bench/COMPARISON.md` for the paired v2.65.10 allocation
microbenchmark, the dated v2.65.9 deterministic suite, and the historical
11-file corpus.

Release alignment: **v2.65.10**. The wire layout is unchanged from v2.65.9;
valid encoded streams remain compatible. Malformed token extensions, offsets
beyond the declared window, invalid ANS normalization and truncated Huffman
bitstreams are rejected. Changing the streaming encoder's `format_v2` through
reset no longer produces corrupt long-match frames.

License: this codec library is GPL-3.0-or-later; the VaptVupt tool (formerly
Zupt) is dual-licensed AGPL-3.0 + commercial (contact sac@securityops.co).

---

## Five integration points

1. **Link against the amalgamation.** `make amalg` emits
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
   transaction layer and let it retry from the previous good snapshot. For a
   BCJ-filtered streaming frame, do not expose partial output: the inverse is
   applied to the complete output exactly once, after checksum validation or
   after the final checksumless block.

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
if (n < 0) { /* handle VV_ERR_*; insufficient dst is VV_ERR_OVERFLOW */ }

/* Decode (untrusted input) */
int64_t m = vv_decompress_flags(comp, comp_len, out, out_cap,
                                VV_DECOMPRESS_SKIP_CHECKSUM);
if (m < 0) { /* any negative return: reject this frame, do not recover */ }
```

Bound `dst_cap` with `vv_compress_bound(src_len)`. The decoder enforces its own
output and work limits. The API contract is to reject a hostile frame with a
clean negative return; release gates test that behavior under sanitizers and
fuzzing, while formal evidence covers only the selected bounded helpers stated
below.

Only `VV_MODE_ULTRA_FAST`, `VV_MODE_BALANCED`, and `VV_MODE_EXTREME` are valid
`opts.mode` values. Since v2.65.9, `vv_compress` returns `VV_ERR_PARAM` for any
other enum representation and when both `opts.filter_x86` and
`opts.filter_arm64` are set. Choose at most one architecture filter, or use
`filter_auto`, for one-shot compression. The streaming encoder cannot apply a
whole-frame transform while emitting blocks incrementally, so
`vv_cstream_create` returns NULL and `vv_cstream_reset` returns `VV_ERR_PARAM`
for an invalid mode or any BCJ option; use `vv_compress` for filtered frames.
A `window_log` outside 10..24 is likewise rejected before BCJ
allocation or transformation. One-shot decode, streaming decode, and frame-info
parsing reject an input header that sets both BCJ bits. The CLI's `-A 0` setting
is automatic, not disabled: fast uses factor 2 and balanced/extreme use factor
1.

`vv_dstream_decompress_chunk` reports cumulative `written`, including calls
after frame completion; `consumed` remains per-call. Keep the same output
buffer base for the stream. `vv_cstream_reset` now synchronizes the new
`format_v2` option with match-length limits and hash3 enablement. It allocates
required hash3 storage before accepting new options, returning `VV_ERR_NOMEM`
without replacing the current options if that allocation fails.

The decoder requires a terminating byte below 255 for each token length
extension and enforces the frame's advertised window for match offsets. ANS
literal frequencies must sum to the full normalization table before it is
built. Huffman bit writing avoids undefined shifts and reports output overflow;
decoding rejects truncated bitstreams. The CLI validates complete decimal
arguments and known modes, and a failed output flush/close returns failure.

The encoder skips unused hash4 allocation: 512 KiB less requested memory at
the default window and up to 64.25 MiB at `window_log=24`. Paired internal
measurements against v2.65.9 found +32.3% throughput for 1 KiB fast text and
+17.2%/+39.6% for 1 MiB random input in fast/balanced mode; 1 MiB text was
approximately unchanged. Compressed sizes/hashes matched in all measured
cases. These are allocation savings, not an RSS measurement; see
`bench/COMPARISON.md` for the in-process methodology.

The v2.65.9 sequence decoder also builds its tANS tables directly, reducing
per-block table scratch from 52 KiB to 48 KiB without changing the stream.
Paired pinned in-process measurements found +0.40% text and +1.21% JSON decode
(about +0.80% geometric mean), a modest result that should be re-measured for
the host workload.

SEQ carries a global match count, so an oversize literal run before a later
match is not representable as a midstream matchless entry. v2.65.9 rejects that
SEQ candidate internally and falls back to another lossless block type. This is
transparent to callers: a positive `vv_compress` result still roundtrips, and
valid wire output is unchanged.

---

## Build targets

- `make amalg` — single-file `build/vaptvupt.{c,h}` for drop-in embedding.
- `make` — the `vaptvupt` CLI and the static library pieces, `-Wall -Wextra -Werror`.
- `make test` — full suite (C suites, reference decoders, differential fuzzer,
  negative corpus, ratio gate, OOM sweep). It also checks current C output in
  Python/JavaScript, including checksum-on/off x86/AArch64 BCJ frames. Allow
  >= 850 s. Every Python subcommand propagates failures, including CLI contract
  checks for malformed options and failed output writes.
- `make verify` — CBMC proofs + Frama-C/Eva analyses of the BCJ filters and the
  decoder's length reader and block-header codec (needs `cbmc`, optionally
  `frama-c-base` + `z3`).

If you vendor `src/` directly instead of the amalgamation, run
`make amalg-verify` in CI: it rebuilds the amalgamation and fails if it
has drifted from `src/`, so a security fix in `src/` cannot silently miss the
embedded copy.

The v2.65.10 length-reader change has an updated formal harness, but CBMC was
unavailable during this release validation. Its historical proof does not
certify the modified reader; regression and sanitizer tests cover the change
dynamically. No new full formal-tool rerun is claimed. The historical evidence
remains limited to its recorded implementations and bounds in `FORMAL_AUDIT.md`
and `verification/README.md`.

---

## Threat model (state this to your users)

- A compressed frame (extension `.zupt`) provides **no confidentiality and no
  authentication on its own.** The XXH64 footer is an integrity check against
  accidental corruption, not a MAC: it does not detect deliberate forgery.
- For tamper-resistance the host **must** wrap the compressed bytes in an AEAD
  (AES-256-GCM, XChaCha20-Poly1305) or an authenticated PQ construction. When
  it does, the codec's checksum is redundant and can be skipped (points 2-3).
- The codec's **safe-decompression contract** is that a malformed or
  adversarial frame returns a clean error with bounded resource use, without an
  out-of-bounds access, use-after-free, or unbounded hang. Evidence is empirical
  for the full decoder (differential fuzzing, ASan/UBSan corrupt-input sweeps,
  and DoS reproducers) and bounded/formal for the selected helpers listed in
  `FORMAL_AUDIT.md`; it is not an unbounded proof of the entire decoder.
- The incoming-frame decode path is the primary at-risk consumer in a backup
  pipeline; keep it behind the AEAD and treat decode errors as rejects.
