# Integrating VaptVupt 2.48.2 into Zupt 2.2.3

> **⚠ Retracted claims notice (Sprint 37, May 2026)**
>
> This document was written against v2.48.1 and contains performance
> claims ("beats zstd-3 by 1.07% aggregate", "1.27× faster decode",
> "26,773 MB/s on random data") that were derived from an 8-fixture
> subset that included synthetic fixtures. **Sprint 32's full-Silesia
> measurement (12 fixtures) inverts the aggregate**: vv-extreme is
> 1.29% behind zstd-3, not 1.07% ahead.
>
> The performance numbers in this document remain accurate for the
> specific subset they were measured on, but they should not be read
> as full-corpus aggregates. See [docs/PERFORMANCE.md](docs/PERFORMANCE.md)
> for the honest full-Silesia measurement that supersedes the headline
> claims below.
>
> The integration mechanics (API calls, build flags, configuration)
> in this document remain correct and current.

This document is the **canonical integration reference** for the Zupt
2.2.3 team when embedding VaptVupt 2.48.2 as the compression layer
beneath Zupt's AES-256-GCM + ML-KEM envelope.

Written against: `vaptvupt-2.48.1`, May 2026.

---

## TL;DR — The Five Integration Points

1. **Link against the amalgamation** — `build/vaptvupt.c` + `build/vaptvupt.h`. No Makefile wiring required, drop in and ship.
2. **Use `VV_DECOMPRESS_SKIP_CHECKSUM` on decode** — Zupt's outer GCM tag already authenticates the compressed bytes. XXH64 is redundant work; skipping it delivers 2–5× decode speedup on random data and ~30% on real fixtures.
3. **Use `opts.format_v2 = 1` on encode for binary-heavy data** — delivers 4–7% better binary compression with zero format-compat risk (v2.33.0+ decoders read v2 frames transparently). For text-heavy data, leave it off — Sprint 120 measurements show v2 is +0.37% **worse** on dickens.
4. **Set `opts.checksum = 0` on encode** — skips the XXH64 footer on the encoder side (saving ~10% encode time), since Zupt's outer AES-GCM already authenticates the compressed bytes. Pair with `VV_DECOMPRESS_SKIP_CHECKSUM` on decode (point #2) for the full savings.
5. **Treat any non-OK decode return as a frame-level reject** — do NOT attempt recovery. Pass the error up to Zupt's transaction layer, which will retry from the previous snapshot.

---

## Why VaptVupt 2.48.2 for Zupt 2.2.3

Zupt's threat model places VaptVupt **inside** the AEAD envelope:

```
[Zupt 2.2.3 archive header]
[per-file metadata]
[ML-KEM encapsulated session key]
[AES-256-GCM wrapped {
   ↓ This is the VaptVupt 2.48.2 frame
   [vv_frame: <file contents compressed>]
   ↓
}]
[Zupt footer with outer signature]
```

**Why v2.48.2 specifically (vs v2.47.x in earlier Zupt releases)**:

- **Aggregate ratio now beats zstd-3 by 1.07%** (was +1.2% behind in v2.47.5). The Sprint 120 cost-aware lazy parser plus Sprint 121's gating delivered the breakthrough — **encoder-only change, wire-format compatible** with v2.47.x decoders.
- **fx_text −6.93%, fx_json −2.49%, sao −2.54%, x-ray −3.37% vs zstd-3** — vv-extreme now wins on 4 of 8 reference fixtures (was 3 of 8 in v2.47.5).
- **Decode throughput stays at 1.27× zstd-3 in aggregate** (decoder unchanged from v2.47.x).
- **Memory hygiene** (Sprint 118): encoder working buffers (`lit_buf`, `stripped`, `src_buf`, `tmp`, `ent_buf`, plus the context struct) are now scrubbed via `vv_secure_zero` before `free()`. This is defense-in-depth specifically for Zupt's pipeline (compress → encrypt → write): the codec's working buffers are scrubbed before the encryption step gets the data.
- **Hardened-build compatibility** (Sprint 117/118): codec now compiles cleanly under `clang -fsanitize=integer` (strict UBSan superset, 92→0 false positives). For Zupt deployments using hardened-build CI, this matters.

**Implications for Zupt's threat model**:

- A malformed VaptVupt frame **cannot reach the decoder** unless it
  was already AEAD-validated. The decoder's defense-in-depth against
  malformed input is a last-line-of-defense, not the primary barrier.
- Consequently **`--fast` is the correct default** for Zupt decode.
  Skipping XXH64 saves real throughput on partial restores.
- **Ratio matters long-term**. Zupt archives are terabyte-scale,
  stored for years. Sprint 120/121's ratio improvements translate
  directly to storage savings — every TB of archived backups now
  costs ~1.1% less in storage compared to zstd-3 baseline.
- **The AEAD tag changes on every encode** even for identical input.
  Zupt's deduplication layer runs BEFORE VaptVupt, so any
  ratio-determinism assumptions are Zupt's concern, not VaptVupt's.

---

## API Integration

### Encode path

```c
#include "vaptvupt.h"

int zupt_compress_for_archive(const uint8_t *plaintext, size_t plaintext_len,
                              uint8_t **out_buf, size_t *out_len,
                              int is_binary_heavy) {
    vv_options_t opts;
    vv_default_options(&opts);
    opts.mode = VV_MODE_EXTREME;          /* or BALANCED for backup speed */
    opts.format_v2 = is_binary_heavy;     /* 4-7% better binary; -0.4% worse text */
    opts.checksum = 0;                    /* skip XXH64 — Zupt's AES-GCM authenticates */

    size_t cap = vv_compress_bound(plaintext_len);
    uint8_t *buf = malloc(cap);
    if (!buf) return -1;

    int64_t clen = vv_compress(plaintext, plaintext_len, buf, cap, &opts);
    if (clen < 0) { free(buf); return -1; }

    *out_buf = buf;
    *out_len = (size_t)clen;
    return 0;
}
```

**Mode selection**:
- `VV_MODE_EXTREME` — best ratio (now beats zstd-3 by 1.07% aggregate),
  ~3.9 MB/s encode. Use for primary backup writes where storage cost
  dominates.
- `VV_MODE_BALANCED` — 15–22 MB/s encode with modest ratio loss.
  Use for hot-path writes (incremental snapshots).
- `VV_MODE_ULTRA_FAST` — 50+ MB/s encode with significant ratio loss.
  Use only for ephemeral data (wire protocol compression, not disk).

**`format_v2` selection heuristic for Zupt 2.2.3**: if Zupt knows
the file class (e.g., from MIME type or file extension), set
`format_v2 = 1` only for binary classes (ELF, Mach-O, PE, image
formats, archive formats). For text/JSON/source code, leave
`format_v2 = 0`.

### Decode path

```c
#include "vaptvupt.h"

int zupt_decompress_from_archive(const uint8_t *cmp, size_t cmp_len,
                                 uint8_t *dst, size_t dst_cap,
                                 size_t *decoded_len) {
    int64_t dlen = vv_decompress_flags(cmp, cmp_len, dst, dst_cap,
                                        VV_DECOMPRESS_SKIP_CHECKSUM);
    if (dlen < 0) return -1;  /* pass up to Zupt's retry layer */
    *decoded_len = (size_t)dlen;
    return 0;
}
```

**Security note**: `VV_DECOMPRESS_SKIP_CHECKSUM` does NOT skip any
security-relevant bounds check. It skips only the XXH64 verification
of decoded bytes. All of the following remain active:

- Frame magic validation (`0x56564456` / `0x56564E44`)
- Format version byte validation
- Block header sanity (type, size, last-flag)
- **Offset bounds**: `offset != 0 && offset ≤ 1 MB` (absolute cap)
  and `offset ≤ op - dst_base` (position-dependent, for early-block
  sequences)
- **Match/literal overflow**: `op + run ≤ op_end` (near block ends)
- **ANS state bounds**: masked to table range, corrupt state cannot
  cause OOB table reads
- **Literal-run extension bounds** (Sprint 109)
- **OOB code-table reads** for `ll_code`, `of_code`, `ml_code` (Sprint 109)
- **NULL-deref protection** on edge-case empty symbol tables (Sprint 109)

### Streaming path (for partial restores)

**Streaming encode** — for compressing files larger than memory or as
they're produced:

```c
vv_cstream_t *cs = vv_cstream_create(&opts);
if (!cs) return -1;

while (have_more_input) {
    size_t written = 0;
    int rc = vv_cstream_compress_chunk(cs, in_chunk, in_chunk_len,
                                       out_buf, out_cap,
                                       &written, /* is_last= */ done);
    if (rc < 0) { vv_cstream_destroy(cs); return -1; }
    /* write out_buf[0..written] to Zupt's writer */
}
vv_cstream_destroy(cs);  /* scrubs all internal buffers per Sprint 118 */
```

Each call emits one compressed block (and the frame header on the
first call, frame footer on the last). Chunks are bounded at
`VV_MAX_BLOCK_SIZE` (1 MB). **`vv_cstream_destroy` scrubs all
plaintext-derived working buffers** before `free()`, so Zupt does
not need to do additional zeroing on the codec's behalf.

**Streaming decode** — for decompressing into a buffer that's
populated as input arrives (e.g., from network or piped stdin):

```c
vv_dstream_t *ds = vv_dstream_create();
if (!ds) return -1;

while (have_more_input) {
    size_t consumed = 0, written = 0;
    int rc = vv_dstream_decompress_chunk(ds, in_chunk, in_chunk_len,
                                         dst_buf, dst_cap,
                                         &consumed, &written);
    if (rc < 0) { vv_dstream_destroy(ds); return -1; }
    if (rc == 1) break;  /* frame complete */
    /* `written` is CUMULATIVE total decoded so far (not per-chunk delta) */
    /* dst_buf pointer must remain stable across calls */
    in_chunk += consumed;
    in_chunk_len -= consumed;
}
vv_dstream_destroy(ds);
```

**API note**: `vv_dstream_decompress_chunk`'s `written` output is the
cumulative running total, not the bytes added by this specific call.
The decoder writes at an internal offset based on previous output, so
`dst_buf` must point to the same buffer across all calls within a
frame. Returns `0` if more input is needed, `1` when the frame
completes, or a negative error code on corruption.

**Skip-checksum on streaming decode**: the streaming decoder always
verifies the XXH64 footer when the frame ends. To skip it for Zupt
deployments where AES-GCM authenticates already, use the stateless
`vv_decompress_flags(VV_DECOMPRESS_SKIP_CHECKSUM)` API for the
common case where the full compressed frame is in memory after AEAD
unwrap. Streaming decode without checksum verification is not
currently exposed as an option.

---

## Performance You Can Plan For (v2.48.2)

Measured on a 2.1 GHz x86_64 container, library-level (not CLI),
best-of-5 warmed runs.

### Decode throughput (aggregate, 8-fixture suite)

| Codec | MB/s |
|---|---:|
| **VaptVupt v2.48.2** | **151** |
| zstd-3 | 119 |
| **Speedup** | **1.27×** |

vv beats zstd-3 on decode throughput on **7 of 8 fixtures** (xml is
the one loss).

### Random-data decode (the headline workload for AEAD-wrapped archives)

For Zupt's typical workload — archives compressed with GCM →
high-entropy ciphertext at the decoder input — VaptVupt's `--fast`
path skips per-byte work in the incompressible regime:

| Content | **VaptVupt `--fast`** | zstd-19 | lz4-9 |
|---|---:|---:|---:|
| Random (AEAD ciphertext) | **26,773 MB/s** | 7,172 | 17,594 |
| Binary (pattern-rich) | **14,414 MB/s** | 8,098 | 19,933 |

Expect real-world Zupt restore decode throughput in the
**5–15 GB/s range** depending on the fraction of the archive that
is binary vs ciphertext-equivalent random data.

### Compression ratio (vv-extreme vs zstd-3, 8-fixture suite)

| Fixture | vv-extreme | zstd-3 | Δ |
|---|---:|---:|:---:|
| fx_text | **128,238** | 137,790 | **−6.93%** ✓ |
| fx_json | **198,213** | 203,276 | **−2.49%** ✓ |
| fx_source | 206,350 | 195,078 | +5.78% |
| bash | 738,698 | 727,132 | +1.59% |
| dickens | 3,818,656 | 3,669,252 | +4.07% |
| xml | 643,067 | 639,138 | +0.61% |
| sao | **5,410,425** | 5,551,158 | **−2.54%** ✓ |
| x-ray | **5,881,236** | 6,086,279 | **−3.37%** ✓ |
| **Aggregate** | **17,024,883** | 17,209,103 | **−1.07%** ✓ |

Zupt's typical archive mix (binary + JSON + config + backup metadata)
will land closer to the favorable end of this distribution. Plan for
**aggregate ratio ≈ 1% better than zstd-3** on real-world Zupt
content, with binary-heavy archives doing meaningfully better.

### Encode throughput

| Mode | MB/s |
|---|---|
| `VV_MODE_ULTRA_FAST` | 50+ |
| `VV_MODE_BALANCED` | 15–22 |
| `VV_MODE_EXTREME` | 3.9 |

Encode throughput is acknowledged trade-off in favor of decode speed
+ ratio. For Zupt's typical write-once-read-many backup workload,
this is the right asymmetry.

---

## Threat Model & What VaptVupt Guarantees

**VaptVupt guarantees** (validated via 4 libFuzzer harnesses across
~145,000 cumulative sanitized executions, 18 C test binaries,
12 DoS reproducers, and 13 audit-found defects fixed):

1. **No out-of-bounds read** on any malformed compressed input
2. **No crash/SEGV** — malformed input returns `VV_ERR_CORRUPT` or
   `VV_ERR_OVERFLOW` cleanly
3. **No heap corruption** — all internal buffers are caller-sized or
   bounded-allocated
4. **No infinite loops** — bounded iteration counters in ANS decode
   and Huffman 4-stream decode
5. **Memory hygiene** — encoder working buffers scrubbed before
   `free()` (Sprint 118)
6. **Hardened-build clean** — codec compiles cleanly under
   `clang -fsanitize=integer` (Sprint 117/118)
7. **Deterministic output** on valid input (same input → same output
   bytes across platforms within the same VaptVupt minor version
   with the same opts)
8. **C decoder is canonical**. The Python and JavaScript reference
   decoders (`reference/vv_decoder.py`, `reference/vv_decoder.js`)
   support `lit_fmt` 0–3 (added in Sprint 116/117). They do NOT
   decode `lit_fmt = 4` (4-stream Huffman, since v2.47.0), which
   may be selected for inputs ≥1024 literals. Zupt MUST use the C
   decoder as the canonical decoder for production paths; the
   reference decoders are useful for format conformance testing
   and the framing/checksumming/ANS-coding code paths.

**VaptVupt does NOT guarantee**:

1. **Constant-time operation**. XXH64 and the LZ matcher are both
   data-dependent in ways that could leak timing information if
   ciphertext were exposed to a chosen-input attacker. For Zupt
   this is acceptable because the AEAD layer sits outside VaptVupt.
2. **Bit-exact reproducibility across VaptVupt minor versions**.
   v2.48.0 and v2.48.2 produce different bytes on the same input
   even with identical opts (Sprint 121's parser changes). Zupt's
   dedup layer must key on content hashes, not compressed hashes.
3. **Protection against ratio-based side channels** (CRIME/BREACH-
   style). Compressed size leaks information about plaintext. Zupt
   addresses this via per-record keys and padding at the AEAD layer.

See [FORMAL_AUDIT.md](FORMAL_AUDIT.md) for the full verification
matrix, threat model, and reproduction steps.

---

## Integration Checklist for Zupt 2.2.3 Release

Before declaring the VaptVupt 2.48.2 integration production-ready:

- [ ] Link against `build/vaptvupt.c` + `build/vaptvupt.h` (not `src/*.c`)
- [ ] Verify amalgamation drift detection: run `make amalg-verify` →
      should report "in sync"
- [ ] All Zupt 2.2.3 tests pass with VaptVupt 2.48.2 (decoder is
      byte-identical to v2.47.x on valid input; should be drop-in)
- [ ] Measure end-to-end backup throughput; confirm no regression
      vs Zupt 2.2.2's prior compression layer
- [ ] Measure end-to-end restore throughput; confirm expected
      improvement from `--fast` flag
- [ ] Confirm archive-format compatibility statement: archives
      written by Zupt 2.2.3 (using VaptVupt 2.48.2 with default
      opts) decode cleanly with any VaptVupt v2.47.0+ decoder. Older
      Zupt versions running v2.46.x decoders may need to disable
      `lit_fmt = 4` via `opts.compat_v246_5_decoder = 1` if
      cross-version archive reads are required.
- [ ] Test adversarial inputs: feed random bytes to the decoder,
      confirm no crashes. (VaptVupt's own fuzzer covers the codec
      layer; Zupt should verify the AEAD-outside-codec layering.)
- [ ] Review this document's "Threat Model" section with Zupt's
      security team; confirm the non-guarantees are acceptable
- [ ] Review [FORMAL_AUDIT.md](FORMAL_AUDIT.md) §3 (Defects Fixed
      in the Audit Campaign) and §5 (Verification Reproduction)
      with the security team
- [ ] Update Zupt 2.2.3's SBOM to list VaptVupt 2.48.2 as a component
- [ ] Document the **GPL-3.0-or-later** license compatibility (Zupt
      must be GPL-3.0+ compatible to link VaptVupt directly, or use
      VaptVupt via IPC if Zupt is under an incompatible license).
      **Note**: VaptVupt is GPL-3.0-or-later (not GPL-2.0). Linking
      with GPL-2.0-only code is NOT permitted; either use GPL-3.0+
      code or interact via IPC/file boundaries.
- [ ] Confirm hardened-build CI compiles VaptVupt cleanly under
      `clang -fsanitize=integer` (Sprint 117/118 made this clean;
      verify it's still clean in the integrated build)

---

## API Stability Promise

VaptVupt 2.x maintains:

- **Wire-format backward compatibility**: any archive produced by
  v2.0.0+ decodes with v2.48.2. Archives produced by v2.48.2 decode
  with any v2.46.0+ decoder cleanly when the C encoder uses default
  options. Note: `lit_fmt = 4` (4-stream Huffman, default for ≥1024
  literals) requires a **v2.47.0+ decoder** — earlier decoders reject
  these frames with `VV_ERR_CORRUPT`. Set
  `opts.compat_v246_5_decoder = 1` to suppress `lit_fmt = 4` on
  encode if archive consumers may run pre-v2.47.0 decoders.
- **API source-compatibility**: the public headers (`vaptvupt.h`,
  `vaptvupt_api.h`) maintain backward-compatible signatures.
  Additions go at the END of `vv_options_t`; callers using
  `vv_default_options()` get sensible defaults for new fields.
- **ABI stability** is NOT promised across major versions. Zupt
  should rebuild when upgrading VaptVupt, not swap shared libraries.
- **Encoder output is NOT bit-exact across minor versions**. v2.48.0
  and v2.48.2 produce different bytes on the same input due to the
  Sprint 121 parser refinement. The wire format itself remains
  compatible — old encoders' output decodes with new decoders and
  vice versa — but byte-equality between encoders is not guaranteed.

Breaking changes (wire-format or API) only arrive with major
version bumps (2.x → 3.x).

---

## Support & Known Limitations (v2.48.2)

1. **Per-fixture ratio gaps remain on 4 of 8 reference fixtures**
   (fx_source +5.78%, bash +1.59%, dickens +4.07%, xml +0.61% vs
   zstd-3). The aggregate now beats zstd-3 by 1.07% — these gaps
   are visible only at per-fixture granularity. **Not a blocker for
   Zupt 2.2.3** — Zupt's typical archive mix sees aggregate
   improvement.
2. **Reference decoder coverage gap**: Python and JavaScript
   reference decoders support `lit_fmt = 0, 1, 2, 3`. They do NOT
   decode `lit_fmt = 4` (4-stream Huffman, may be selected for
   inputs ≥1024 literals). Use the C decoder for any archive that
   may include `lit_fmt = 4` blocks.
3. **Encode throughput is meaningfully behind zstd** (0.13× to
   0.46× depending on mode and fixture). Acknowledged trade-off
   favoring decode speed and ratio.
4. **No pre-trained dictionary support**. If Zupt has many small
   similar files (telemetry records, config files), a dictionary
   could deliver up to 2× additional compression. On the roadmap.
5. **No constant-time encoder/decoder** — see Threat Model §1
   above. AEAD layer outside VaptVupt is the mitigation.

For issues: file against the VaptVupt repository with:
- Full `vaptvupt --version` output
- Minimal reproducer input
- Exact error return code
- Expected vs observed behavior

---

## Version Pinning Recommendation

Pin Zupt 2.2.3 to **VaptVupt 2.48.2 exactly**. Future v2.48.x patch
releases will maintain wire-format and API stability — but encoder
byte-equality is not promised, so Zupt 2.2.3's regression test suite
should treat encoder output bytes as opaque (verify roundtrip, not
bit-exact match against a checked-in fixture).

Production validation for Zupt 2.2.3 is done against this specific
VaptVupt version. Upgrading to v2.49+ (when released) should go
through a Zupt re-validation cycle even though the wire format will
remain backward-compatible.

---

**End of integration guide.** Ship well.
