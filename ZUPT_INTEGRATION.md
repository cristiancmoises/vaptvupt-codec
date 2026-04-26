# Integrating VaptVupt 2.46.1 into Zupt 2.1.6

This document is the **canonical integration reference** for the Zupt
2.1.6 team when embedding VaptVupt 2.46.1 as the compression layer
beneath Zupt's AES-256-GCM + ML-KEM envelope.

Written against: `vaptvupt-2.46.1`, April 22 2026.

---

## TL;DR — The Five Integration Points

1. **Link against the amalgamation** — `build/vaptvupt.c` + `build/vaptvupt.h`. No Makefile wiring required.
2. **Use `VV_DECOMPRESS_SKIP_CHECKSUM` on decode** — Zupt's outer GCM tag already authenticates the compressed bytes. XXH64 is redundant work; skipping it delivers 2-5× decode on random data.
3. **Use `opts.format_v2 = 1` on encode** — delivers 4-7% better binary compression with zero format-compat risk (v2.33.0+ decoders read v2 frames transparently).
4. **Set `opts.checksum = 0` on encode** — skips the XXH64 footer on the encoder side (saving ~10% encode time), since Zupt's outer AES-GCM already authenticates the compressed bytes. Pair with `VV_DECOMPRESS_SKIP_CHECKSUM` on decode (point #2) for the full savings.
5. **Treat any non-OK decode return as a frame-level reject** — do NOT attempt recovery. Pass the error up to Zupt's transaction layer, which will retry from the previous snapshot.

---

## Why VaptVupt 2.46.1 for Zupt 2.1.6

Zupt's threat model places VaptVupt **inside** the AEAD envelope:

```
[Zupt archive header]
[per-file metadata]
[ML-KEM encapsulated session key]
[AES-256-GCM wrapped {
   ↓ This is the VaptVupt frame
   [vv_frame: <file contents compressed>]
   ↓
}]
[Zupt footer with outer signature]
```

**Implications**:

- A malformed VaptVupt frame **cannot reach the decoder** unless it
  was already AEAD-validated. The decoder's defense-in-depth against
  malformed input is a last-line-of-defense, not the primary barrier.
- Consequently **`--fast` is the correct default** for Zupt decode.
  Skipping XXH64 saves real throughput on partial restores.
- **Ratio matters long-term**. Zupt archives are terabyte-scale,
  stored for years. v2.40.0's format-v2 binary gains translate
  directly to storage savings — libc.so.6 class binaries now compress
  within 4% of gzip-9 vs v1's 10% gap.
- **The AEAD tag changes on every encode** even for identical input.
  Zupt's deduplication layer runs BEFORE VaptVupt, so any
  ratio-determinism assumptions are Zupt's concern, not VaptVupt's.

---

## API Integration

### Encode path

```c
#include "vaptvupt.h"

int zupt_compress_for_archive(const uint8_t *plaintext, size_t plaintext_len,
                              uint8_t **out_buf, size_t *out_len) {
    vv_options_t opts;
    vv_default_options(&opts);
    opts.mode = VV_MODE_EXTREME;   /* or BALANCED for backup speed */
    opts.format_v2 = 1;            /* 4-7% better binary ratio */
    opts.checksum = 0;             /* skip XXH64 generation — Zupt's AES-GCM authenticates */

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
- `VV_MODE_EXTREME` — best ratio, ~18 MB/s encode. Use for primary
  backup writes where storage cost dominates.
- `VV_MODE_BALANCED` — 15-22 MB/s encode with modest ratio loss.
  Use for hot-path writes (incremental snapshots).
- `VV_MODE_ULTRA_FAST` — 50+ MB/s encode with significant ratio loss.
  Use only for ephemeral data (wire protocol compression, not disk).

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
- **Offset bounds**: `offset != 0 && offset ≤ 1 MB` (absolute cap,
  v2.39.0+) and `offset ≤ op - dst_base` (position-dependent, for
  early-block sequences)
- **Match/literal overflow**: `op + run ≤ op_end` (near block ends)
- **ANS state bounds**: masked to table range, corrupt state cannot
  cause OOB table reads

### Streaming path (for partial restores)

```c
vv_cstream_t *cs = vv_cstream_create(&opts);
while (have_more_input) {
    vv_cstream_push(cs, in_chunk, in_len);
    size_t out_produced;
    vv_cstream_pull(cs, out_buf, out_cap, &out_produced);
    /* write out_buf[0..out_produced] to Zupt's writer */
}
vv_cstream_finish(cs);
vv_cstream_destroy(cs);
```

Streaming decode (`vv_dstream_*`) follows the mirror pattern. Both
work with the skip-checksum flag via `vv_dstream_set_flags`.

---

## Performance You Can Plan For

Measured on a 2.1 GHz x86_64 container, library-level (not CLI):

### Decode throughput (best-of-30, warmed)

| Fixture class | Default | With `--fast` | vs zstd-19 | vs lz4-9 |
|---|---|---|---|---|
| Random (post-GCM) | 8 GB/s | **27 GB/s** | 3.7× | 1.2× |
| Binary (ELF) | 290 MB/s | 304 MB/s | 0.4× | 0.15× |
| JSON/text | 499 MB/s | 569 MB/s | 0.44× | 0.18× |
| Synthetic repeat | 2.0 GB/s | 2.0 GB/s | 1.1× | 0.88× |

**For Zupt's mixed workload** (archives compressed with GCM → random
bytes at the decoder input), `--fast` on random is the dominant
path. Expect real-world decode throughput in the **5-15 GB/s range**
depending on how much of the archive is already-compressed binary.

### Compression ratio (format v2, EXTREME mode)

| Fixture | VaptVupt v2 | gzip-9 | zstd-19 | lz4-9 |
|---|---|---|---|---|
| synth-text | 5.08× | 6.95× | **7.87×** | 4.83× |
| synth-json | 4.80× | 4.65× | **6.68×** | 3.46× |
| real-bash | 1.92× | 2.09× | **2.32×** | 1.83× |
| real-ls | 2.11× | 2.30× | **2.55×** | 2.00× |
| real-libc | 2.09× | 2.23× | **2.56×** | 1.94× |
| real-python | 2.64× | 2.84× | **3.46×** | 2.34× |

Zupt's typical archive (ELF binaries + config files + backup
metadata) will land in the binary/JSON range. Plan for **2-3×
compression ratio** on real-world content.

### Encode throughput

| Mode | MB/s (balanced target) |
|---|---|
| `VV_MODE_ULTRA_FAST` | 50+ |
| `VV_MODE_BALANCED` | 15-22 |
| `VV_MODE_EXTREME` | 6-10 |

---

## Threat Model & What VaptVupt Guarantees

**VaptVupt guarantees** (tested via 5,200-case differential fuzzer
+ 55-case adversarial suite + 495-case streaming fuzzer):

1. **No out-of-bounds read** on any malformed compressed input
2. **No crash/SEGV** — malformed input returns `VV_ERR_CORRUPT` or
   `VV_ERR_OVERFLOW` cleanly
3. **No heap corruption** — all internal buffers are caller-sized or
   bounded-allocated
4. **Deterministic output** on valid input (same input → same output
   bytes across platforms/versions)
5. **Cross-language round-trip** — frames produced by C encoder
   decode identically in Python and JavaScript reference decoders

**VaptVupt does NOT guarantee**:

1. **Constant-time operation**. XXH64 and the LZ matcher are both
   data-dependent in ways that could leak timing information if
   ciphertext were exposed to a chosen-input attacker. For Zupt
   this is acceptable because the AEAD layer sits outside VaptVupt.
2. **Bit-exact reproducibility across VaptVupt versions**. v2.37.0
   and v2.38.0 produce the same bytes on the same input with the
   same opts, but v2.34.0 vs v2.37.0 differ due to matcher tuning.
   Zupt's dedup layer must key on content hashes, not compressed
   hashes.
3. **Protection against ratio-based side channels** (CRIME/BREACH-
   style). Compressed size leaks information about plaintext. Zupt
   addresses this via per-record keys and padding at the AEAD layer.

---

## Integration Checklist for Zupt 2.1.6 Release

Before declaring the VaptVupt integration production-ready:

- [ ] Link against `build/vaptvupt.c` + `build/vaptvupt.h` (not `src/*.c`)
- [ ] All Zupt tests pass with VaptVupt 2.46.1 (should be drop-in)
- [ ] Measure end-to-end backup throughput; confirm no regression
      vs Zupt 2.1.5's prior compression layer
- [ ] Measure end-to-end restore throughput; confirm expected
      improvement from `--fast` flag
- [ ] Confirm archive-format is documented as "Zupt 2.1.6+ only"
      if format_v2 is enabled (older Zupt versions have v2.33.0+
      decoders and can read v2 frames, but the Zupt envelope itself
      may have changed)
- [ ] Test adversarial inputs: feed random bytes to the decoder,
      confirm no crashes. (VaptVupt's own fuzzer covers the codec
      layer; Zupt should verify the AEAD-outside-codec layering)
- [ ] Review this document's "Threat Model" section with Zupt's
      security team; confirm the non-guarantees are acceptable
- [ ] Update Zupt's SBOM to list VaptVupt 2.46.1 as a component
- [ ] Document the GPL-2.0-or-later license compatibility (Zupt must be
      GPL-2.0+ or use VaptVupt via IPC rather than linking)

---

## API Stability Promise

VaptVupt 2.x maintains:

- **Wire-format backward compatibility**: any archive produced by
  v2.0.0+ decodes with v2.40.0. Archives produced by v2.40.0 decode
  with any v2.33.0+ decoder (earlier decoders reject format-v2
  frames with `VV_ERR_CORRUPT`).
- **API source-compatibility**: the public headers (`vaptvupt.h`,
  `vaptvupt_api.h`) maintain backward-compatible signatures.
  Additions go at the END of `vv_options_t`; callers using
  `vv_default_options()` get sensible defaults for new fields.
- **ABI stability** is NOT promised across major versions. Zupt
  should rebuild when upgrading VaptVupt, not swap shared libraries.

Breaking changes (wire-format or API) only arrive with major
version bumps (2.x → 3.x).

---

## Support & Known Limitations

Open issues / known limitations as of v2.40.0:

1. **Text decode lags zstd** by ~2.5× (~500 MB/s vs zstd's 1,290).
   The per-sequence overhead is the bottleneck; Sprint 51+ targets
   this via match-copy branchless restructuring. Not a blocker for
   Zupt integration — most Zupt content isn't prose text.
2. **Binary ratio gap vs gzip-9** is 4-7%. Closing the last few pp
   would require optimal LZ parsing (3-5× encode slowdown) or
   Huffman secondary literal coding (v3 format change). Neither is
   scheduled.
3. **No pre-trained dictionary support**. If Zupt has many small
   similar files (telemetry records, config files), a dictionary
   could deliver up to 2× additional compression. On the roadmap.

For issues: file against VaptVupt repository with:
- Full `vaptvupt --version` output
- Minimal reproducer input
- Exact error return code
- Expected vs observed behavior

---

## Version Pinning Recommendation

Pin Zupt 2.1.6 to **VaptVupt 2.46.1 exactly**. Future 2.40.x patch
releases will maintain wire-format and API stability, but the
production validation for Zupt 2.1.6 is done against this specific
VaptVupt version. Upgrading to 2.41+ should go through Zupt 2.1.7+
with re-validation.

---

**End of integration guide.** Ship well.
