# Integrating the VaptVupt codec into a host application

This is the integration reference for embedding the VaptVupt compression codec
as the compression layer beneath an application's encryption envelope (for
example, a backup tool that wraps each frame in AES-256-GCM or an ML-KEM + AEAD
construction). It covers the API, the build, the flags that matter, and the
threat-model boundary. Numbers here point to measured data, not headline
claims — see `bench/COMPARISON.md` for the page-sized one-shot API harness,
paired encoder measurements, and separately labeled historical CLI/corpus data.

Release alignment: **v2.65.11**. The wire layout is unchanged from v2.65.10;
valid encoded streams remain compatible. Malformed token extensions, offsets
beyond the declared window, invalid ANS normalization and truncated Huffman
bitstreams are rejected. Changing the streaming encoder's `format_v2` through
reset no longer produces corrupt long-match frames.
Fast mode ignores `format_v2` and retains the four-byte minimum match required
by plain tokens; the previous combination could produce an invalid frame.

License: this codec library is GPL-3.0-or-later. Zupt is a separate userspace
consumer, not a former name for this library or an interchangeable checkout
of the broader VaptVupt application. A kernel adaptation belongs in
vaptvupt-linux; Zupt does not require a kernel module to keep working.

---

## Five integration points

1. **Link against the amalgamation.** `make amalg` emits
   `build/vaptvupt.c` + `build/vaptvupt.h` — a single translation unit, no
   Makefile wiring required. Include `build/vaptvupt.h`; define
   `VV_DISABLE_SIMD=1` for a build without codec intrinsics/dispatch. An
   amalgamation compiled with `-mavx2` instead requires an AVX2-capable target.

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
   decode side may be older than v2.33.0. Fast mode always uses plain tokens
   with min_match=4 and ignores `format_v2`. Measure per data class.

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

Since v2.65.10, the encoder skips unused hash4 allocation: 512 KiB less requested memory at
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

## Literal-decoder workspaces

The lower-level headers `include/vv_huffman.h` and `include/vv_ans.h` expose
`*_decode_with_workspace` and `*_decode4_with_workspace`. Query the required
size/alignment with `vvh_decode_workspace_size()`,
`vvh_decode_workspace_alignment()`, `vva_decode_workspace_size()`, and
`vva_decode_workspace_alignment()`. The caller must provide exclusive storage
that meets those requirements and does not overlap compressed input or decoded
output. Workspace contents are unspecified after return; reuse is allowed
after either success or failure. NULL/misaligned storage returns `PARAM`,
insufficient capacity returns `OVERFLOW`, and `src_consumed` is mandatory.
The headers describe the zero-literal exception and complete pointer contract.

These helpers allocate no decode table. Existing wrappers retain their
signatures and allocate their own storage. The legacy order-1 ANS context
decoder is not covered. S/T block decoding reuses its existing 48 KiB
sequence-table arena for the literal stage before rebuilding sequence tables;
whole-frame decoding still allocates buffers. Single/four-stream ANS literal
decoding now uses the existing direct-table builder, removing its 4 KiB spread
scratch. GCC 14.3 `-O3` without LTO reports individual function frames dropping
from 4704 to 608 bytes and from 5024 to 960 bytes. Those figures exclude nested
callee frames and do not establish a kernel stack budget; the legacy ANS
context encoder still has a 128 KiB local normalization array.

For one-shot fast inputs up to 4 KiB, the encoder initializes only buckets
reachable from input positions and sizes the chain to the next power of two
covering the input, capped by the selected window. The full 18-bit hash
mapping is preserved. At 4 KiB with the default window this avoids requesting
240 KiB of chain storage; it is not an RSS measurement. Larger inputs and
streaming retain the full matcher setup. Paired timings and their controls
are in [bench/COMPARISON.md](bench/COMPARISON.md).

## Caller-owned FAST contexts (development API)

`vv_fast_context_size(max_input)` and `vv_fast_context_alignment()` describe
caller-owned storage for independent inputs of at most 64 KiB. Initialize
it with `vv_fast_context_init`, then reuse it through
`vv_fast_context_compress`. Initialization copies explicit FAST options;
NULL options, other modes, and BCJ/auto-filter requests are rejected.
The storage must be aligned, remain at a stable address, and be exclusive
to one operation at a time. It must not overlap input, output, or the
initialization arguments. There is no destroy operation: the caller frees
its storage when no call can still access it.

Each compression resets dictionary and repeat-offset state, including after
an error. It allocates nothing and has no heap fallback. Supply at least
`vv_compress_bound(src_len)` output bytes; smaller capacities are rejected
before output writes. The original one-shot capacity contract is unchanged.
Token scratch is cleared after parsing. This is not a guarantee that every
byte of caller storage is cleared; callers needing that policy must clear
their entire allocation before releasing it.

The output uses the existing v1 frame and RAW/plain-block grammar, without
entropy coding, BCJ, streaming state, or dictionaries shared between inputs.
FAST continues to ignore `format_v2`. The reuse API is intended to preserve
the corresponding one-shot encoder's bytes, not merely produce another
decodable representation. Its size and alignment queries are not fixed ABI
constants; existing public structures and entry points remain unchanged.

The context stores roots and chain links as unsigned 16-bit positions. Every
insert needs at least four bytes within the independent 64 KiB input, leaving
65535 available as the empty sentinel. The full 18-bit hash mapping, candidate
order and matching rules are unchanged. The primary map occupies 512 KiB;
ordinary one-shot and streaming matchers retain their 32-bit storage.
Workspace also contains metadata, an input-bounded power-of-two chain, and
token scratch. Neither this API nor
the scalar gate is a freestanding kernel library: the shared translation
units retain libc and excluded codec paths. The existing C implementation
also uses native-endian loads/stores for fields specified as little-endian;
big-endian correctness has not been established.

---

## Build targets

- `make amalg` — single-file `build/vaptvupt.{c,h}` for drop-in embedding.
- `make` — the `vaptvupt` CLI and the static library pieces, `-Wall -Wextra -Werror`.
- `make clean && make SIMD=0` — no codec SIMD intrinsics or runtime dispatch.
  Compiler-generated vector operations and system libc implementations are
  separate concerns; this switch alone is not a general-register-only build.
- `make scalar-test` — general-register-only core objects on x86-64/AArch64,
  linked to userspace libraries and exercised by ten suites. It retains
  stack-usage diagnostics but does not approve a kernel stack budget.
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

## Linux kernel readiness

v2.65.11 is not ready for upstream kernel inclusion. The public
GPL-3.0-or-later license does not provide the GPL-2.0-only-compatible rights
required for kernel code. A compatible licensing option needs authorization
from all relevant rights holders; the release changes no license. See the
[kernel licensing rules](https://docs.kernel.org/process/license-rules.html).

The implementation still depends on libc and dynamic allocation. Heap and
stack limits need explicit design and measurement: the legacy ANS context
encoder alone retains a 128 KiB local normalization array. There is no Kbuild/
Kconfig integration, KUnit coverage, or validated cross-architecture,
endianness, and worst-case memory budget. SIMD/FP register use has kernel
context restrictions beyond compiling without intrinsics; consult the
[floating-point API](https://docs.kernel.org/core-api/floating-point.html).
The scalar gate is a userspace portability check, not a kernel build.

Any future proposal needs a concrete subsystem use case and realistic
measurements against the existing codecs, including page-memory cost,
compression/decompression latency, incompressible data, and failure handling.
Synthetic cache-warm userspace throughput does not establish a zram or
filesystem benefit. A human must review the work, certify the DCO with their
own sign-off, and retain `Assisted-by` attribution for AI assistance; an agent
cannot certify on their behalf. Submission is a reviewable patch series to
the relevant maintainers, not a direct push to the mainline tree. Follow
[AI coding assistants](https://docs.kernel.org/process/coding-assistants.html)
and [submitting patches](https://docs.kernel.org/process/submitting-patches.html).

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
