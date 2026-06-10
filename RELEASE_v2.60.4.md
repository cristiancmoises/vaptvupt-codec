# VaptVupt v2.60.4

**Security release — decode-safety fix. Upgrade is recommended for every
consumer that decodes into exactly-content-sized buffers** (which the API
permits and downstream libraries do).

## The defect

High-severity out-of-bounds heap **write** in the AVX2 decode fast path,
reachable through the public API on a **valid** stream.

`match_copy_32_hot` — the fast-path match copy for offsets ≥ 32 — performs
unconditional 32-byte AVX2 stores (the lz4 decode trick). Its safety margin
(72 bytes) is checked only at loop entry; `op` advances by the literal length
*within* the iteration, so a long literal run followed by a fast-path match at
the end of the stream stores past `op_end` when the output buffer is sized to
exactly `content_size`. Two variants existed:

1. `n <= 32`: a single 32-byte store with < 32 bytes of room left.
2. `n > 32`: a final 32-byte tail store for the `n % 32` remainder, needing
   `((n + 31) & ~31)` bytes of room.

Up to 31 bytes are written past the buffer (ASan: "WRITE of size 32" in
`decode_block_tokens_w16`; symptom in consumers: `malloc(): invalid size`
aborts). Non-AVX2 builds are unaffected. The codec's own fuzzers allocate
decode buffers with slack and never reached it; it surfaced through
libvaptvupt's binding tests, which allocate at exactly `content_size`.

The v2.60.3 "decode path fully audited" conclusion was incomplete: that audit
covered match/literal *length* bounds, not the SIMD store *width* against a
tight output buffer. SECURITY.md (Document version 1.8) corrects the record.

## The fix

Two layers, both required (the first alone was insufficient — the second
variant reproduced after it):

1. Both fast-path call sites gate the wide store on room:
   `(op_end - op) >= 32` → `match_copy_32_hot`, else the exact-tail
   `match_copy_32`.
2. `match_copy_32_hot`'s `n > 32` branch now finishes with an exact tail
   (16-byte store + `memcpy`) instead of a 32-byte over-store.

On a valid stream all variants copy the same `mlen` bytes: **compressed output
is byte-identical to every prior release** (ratio gate ± 0 on all fixtures),
and the ~99 % common case keeps the branch-free wide store.

## Validation

- New regression `tests/test_exact_buffer_decode.c` (TEST22):
  **20 136/20 136** exact-buffer roundtrips under ASan+UBSan, covering size
  sweeps, long-match (n > 32) sweeps, repetitive data, and the original
  trigger fixture across all three modes. Reverting *either* fix layer makes
  the test reproduce the OOB.
- Full suite: 22 test binaries, differential 5576/5576, fuzz 5200/5200
  consistent, ratio gate ± 0, OOM sweep PASS, `make test` exit 0.
- `make verify`: 5/5 CBMC/Eva proofs SUCCESSFUL.
- `-Wall -Wextra -Werror` clean; ASan + UBSan clean.

No API, ABI, or wire-format change. Encoder untouched.

## Credits

Found via downstream integration testing (libvaptvupt v1.6.0 binding suites).

— Cristian Cezar Moisés, securityops.co. In Code We Trust.
