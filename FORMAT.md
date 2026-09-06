# VaptVupt Wire Format Specification

**Version**: 1 (frame format version field = `0x01`)
**Endianness**: Little-endian for all multi-byte integers
**Status**: Stable since v1.0.0 of the reference encoder
**Reference implementation alignment**: v2.65.10 (no wire-layout change)

This document specifies the on-wire format produced by `vv_compress`,
`vv_compress_mt`, and `vv_cstream_*`. It is sufficient to implement
a compatible decoder in any language without consulting the
reference C source.

The reference implementation lives at `src/vv_decoder.c` of the
VaptVupt source tree.

---

## 1. File Structure

A `.vv` file is a concatenation of one or more **frames**:

```
+---------+---------+ ... +---------+
| Frame 0 | Frame 1 |     | Frame N |
+---------+---------+ ... +---------+
```

Single-frame output is produced by `vv_compress` and
`vv_cstream_*`. Multi-frame output is produced by `vv_compress_mt`
when input exceeds `chunk_size`. A decoder MUST handle both cases
identically — keep parsing frames until the input bytes are
exhausted.

Each frame is:

```
+----------------+--------+--------+ ... +--------+--------+
| Frame Header   | Block 0| Block 1|     | Block N| Footer*|
| (16 bytes)     |        |        |     |        |        |
+----------------+--------+--------+ ... +--------+--------+
                                              ^         ^
                                              |         |
                                       last_block flag  optional, depends
                                       set on Block N   on flags.has_checksum
```

---

## 2. Frame Header (16 bytes)

```
Offset  Size  Field           Description
------  ----  -----           -----------
  0      4    magic           uint32 LE = 0x56560100
                              (bytes 0x00 0x01 0x56 0x56 in stream order)
  4      1    version         Format version. MUST be 0x01.
  5      1    flags           Bit 0: has_checksum (1 = trailing footer present)
                              Bit 1: has_dict     (reserved; MUST be 0)
                              Bit 2: x86 BCJ filter was applied
                              Bit 3: AArch64 BCJ filter was applied
                                      (bits 2 and 3 are mutually exclusive)
                              Bits 4-7: reserved, SHOULD be 0
  6      1    mode_hint       Compressor mode used (0=ULTRA_FAST, 1=BALANCED,
                              2=EXTREME). Informational only — decoder
                              ignores this field for bitstream parsing.
  7      1    window_log      log2 of the LZ window size used by the encoder.
                              Valid range: 10 to 24.
                              **CRITICAL**: window_log determines offset
                              encoding width: ≤16 → 2-byte offsets,
                              ≥17 → 3-byte offsets in COMPRESSED blocks.
  8      8    content_size    Uncompressed length of this frame's content,
                              little-endian uint64. 0 if unknown
                              (streaming encode without total size).
                              **Informational only.** The C reference
                              decoder does not validate this against
                              the actual decoded length — it uses it
                              only as a pre-allocation hint. Stricter
                              decoders MAY validate and reject on
                              mismatch.
```

A decoder MUST reject any frame with:
- `magic != 0x56560100`
- `version != 1`
- `window_log < 10` or `window_log > 24`
- both BCJ bits set (`flags & 0x0C == 0x0C`)
- Trailing input that is too short for a complete frame

A decoder MAY reject a frame with:
- `flags & 0xF2 != 0` (reserved flag bits set; bit1 and bits4-7)

The reference decoder enforces the window range because a frame header is the
bound used to validate LZ offsets, and rejects the contradictory dual-BCJ
combination in one-shot decode, streaming decode, and frame-info parsing.
Reserved flag bits remain tolerated for forward compatibility; new encoders
MUST leave them clear. A conforming encoder MUST set at most one BCJ
architecture bit. The v2.65.9 `vv_compress` API enforces this by returning
`VV_ERR_PARAM` when both explicit filters are requested; this validation does
not change the flag layout.

---

## 3. Block Structure

Every block starts with a **4-byte little-endian packed header**:

```
Bits 0-1:    block_type (0=RAW, 1=COMPRESSED, 2=RLE, 3=ENTROPY)
Bit  2:      last_block flag (1 = this is the final block of the frame)
Bits 3-23:   decompressed_size — number of output bytes this block
             produces (max 2^21 - 1 = 2,097,151; encoder caps at
             VV_MAX_BLOCK_SIZE = 1,048,576)
Bits 24-31:  reserved; MUST be 0
```

A decoder reads blocks in order until `last_block == 1`. After
the last block, if `flags.has_checksum == 1`, the 12-byte footer
follows; otherwise the next frame (or EOF) follows immediately.

### 3.1 Block Type 0 — RAW

Stored uncompressed.

```
+----------------+----------------+
| Block header   | dsz raw bytes  |
| (4 bytes)      |                |
+----------------+----------------+
```

Output: copy `dsz` bytes verbatim.

### 3.2 Block Type 1 — COMPRESSED (LZ tokens, no entropy)

```
+----------------+--------------+----------------+
| Block header   | csz (3 LE)   | csz token bytes|
+----------------+--------------+----------------+
```

After the header, a 3-byte little-endian compressed-size field,
then `csz` bytes of LZ token stream.

The token stream encodes a sequence of (literal-run, match) pairs.
See **Section 5: LZ Token Format** below.

### 3.3 Block Type 2 — RLE

Single byte repeated `dsz` times.

```
+----------------+--------+
| Block header   | byte   |
| (4 bytes)      | (1B)   |
+----------------+--------+
```

Output: `memset(out, byte, dsz)`.

### 3.4 Block Type 3 — ENTROPY

```
+----------------+--------------+--------+----------------+
| Block header   | csz (3 LE)   | tag(1B)| csz-1 payload  |
+----------------+--------------+--------+----------------+
```

The first byte after the 3-byte size field is an **entropy tag**
selecting the entropy sub-format:

| Tag  | Hex  | Name      | Format                                        |
|------|------|-----------|-----------------------------------------------|
| 'H'  | 0x48 | HUFFMAN   | Legacy Huffman over literals + raw LZ tokens  |
| 'A'  | 0x41 | ANS       | tANS single-stream over literals (legacy)     |
| 'I'  | 0x49 | ANS4      | tANS 4-way interleaved over literals          |
| 'C'  | 0x43 | CTX       | tANS order-1 context model over literals      |
| 'S'  | 0x53 | SEQ       | Full sequence coding (lits + ML + OF + LL)    |
| 'T'  | 0x54 | SEQ_V2    | Like S, with min_match=3 (v2.33.0+)           |

**Note on encoder selection in current C reference (v2.x)**: The
modern encoder produces only `'S'`/`'T'` (SEQ/SEQ_V2) ENTROPY blocks
for inputs that warrant entropy coding. Tags `'H'`, `'A'`, `'I'`,
`'C'` are **legacy from earlier format versions** (v0.3 through v0.7)
and remain only for decoder backward-compat with files produced by
those versions. A reader targeting only current/default encoder output MAY
support `'S'`/`'T'` and the non-entropy block types, leaving the legacy tags as
a "decode-old-files" extension. Such a reader is current-output-compatible,
not a fully backward-compatible v1 decoder under the Stability Promise in §8;
it MUST be labeled with that narrower scope.

The v2.65.9 Python and JavaScript references decode current/default encoder
output, including `'S'`/`'T'`, HUFFMAN4 literals, and x86/AArch64 BCJ frames.
The C decoder remains canonical for the complete legacy `'H'`/`'A'`/`'I'`/`'C'`
surface: Python retains limited `'A'` support, while JavaScript intentionally
omits all four legacy tags.

**Adaptive `'T'` selection (encoder policy, v2.61.0+)**: Since
v2.61.0 the reference encoder *auto-selects* `'T'` (SEQ_V2,
min_match=3) for binary-detected input in balanced and extreme
modes — previously `'T'` was emitted only under the explicit
`format_v2` option. Consequence for implementers: a decoder MUST
support `'T'` to decode default v2.61.0+ output; supporting only
`'S'` is no longer sufficient for current-encoder compatibility.
Encoders needing output readable by pre-v2.33.0 decoders set
`vv_options_t::compat_v246_5_decoder`, which restricts selection to
`'S'` (and also suppresses `lit_fmt = 4`, see §3.4.1). This is an
encoder *policy* change only — the wire format is unchanged and the
frame `version` field remains 1.

Tags `H`, `A`, `I`, `C` all share a common framing:

```
+--------+---------+---------+----------+----------+
| tag(1B)| ll(2 LE)| el(2 LE)| el bytes | stripped |
|        | lit_cnt | ent_len | ent_data | LZ tokens|
+--------+---------+---------+----------+----------+
```

- `ll` (literal_count) is the number of literal bytes that were
  entropy-coded. The decoder produces this many literals.
- `el` (ent_len) is the length of the entropy-coded payload.
- `ent_data` is `el` bytes of entropy-coded literals.
- The remainder (`stripped`) is `csz - 1 - 2 - 2 - el` bytes of
  **stripped LZ tokens** — same token format as Block Type 1
  but with all literal bytes removed (literals come from
  entropy-decoded output instead).

Tag `S` (SEQ) is self-contained — it encodes literals, match
lengths, offsets, and literal-run lengths together via four ANS
streams. See `src/vv_ans.c::vva_decode_sequences` for the exact
parsing; details are out of scope for this document because the
SEQ format has its own internal structure (ANS table headers,
state init, four interleaved bitstreams).

One SEQ block stores a **global** `match_count`, not a per-LL-entry
`has_match` bit. The first `match_count` decoded LL entries therefore each have
a match; only later LL entries may be literal-only. A terminal literal run over
65,535 bytes can be represented as multiple trailing LL-only entries, and a
decoder continues until both `match_count` matches and `total_lits` literals
are complete. A nonterminal literal run over 65,535 bytes followed by another
match cannot be split by inserting a literal-only entry: that would move the
match/LL association and is not representable in this wire layout. The v2.65.9
reference encoder rejects that SEQ candidate and selects another lossless block
representation. This is encoder selection hardening, not a wire-format change.

#### 3.4.1 SEQ Block Literal Section (`lit_fmt`)

Within a SEQ block, the literal data section is preceded by a
**single-byte format selector** `lit_fmt` that determines how the
literals are encoded:

| `lit_fmt` | Name     | Encoding              | Min count | Decoder version |
|-----------|----------|-----------------------|-----------|-----------------|
| 0         | RAW      | Uncompressed bytes    | any       | v2.0.0          |
| 1         | ANS4     | tANS 4-way interleaved | any       | v2.0.0          |
| 2         | ANS1     | tANS single-stream    | any       | v2.0.0          |
| 3         | HUFFMAN  | Single-stream Huffman | any       | v2.46.0         |
| 4         | HUFFMAN4 | 4-stream interleaved Huffman | ≥1024 | **v2.47.0**     |

The encoder selects the format that produces the smallest output,
with one exception: `lit_fmt = 4` is preferred over `lit_fmt = 3`
when both are viable and within 32 bytes of each other, because
`lit_fmt = 4` decodes 1.5× faster via instruction-level parallelism
across the 4 independent streams.

Wire format for `lit_fmt = 4`:

```
+----------------------+-------------+-------------+-------------+
| Huffman code-length  | s1 (3 LE)   | s2 (3 LE)   | s3 (3 LE)   |
| header (existing)    | stream1 sz  | stream2 sz  | stream3 sz  |
+----------------------+-------------+-------------+-------------+
| stream0 bitstream | stream1 bitstream | stream2 | stream3      |
+----------------------+--------------------+---------+----------+
```

- The Huffman code-length header is the same format as `lit_fmt = 3`.
  It encodes a single shared Huffman code table used by all 4 streams.
- The 9-byte stream-size header gives the byte length of streams 1, 2,
  and 3. Stream 0's size is implicit: `total_lit_payload - 9 - hdr_sz
  - s1 - s2 - s3`.
- Each stream is byte-aligned at its start. Stream contents are
  LSB-first bit-reversed canonical Huffman codes (same encoding as
  `lit_fmt = 3`).
- Symbols are distributed round-robin: stream `s` encodes symbols at
  source indices `s, s+4, s+8, ...`. The decoder interleaves them
  back into the output by reading one symbol from each stream in
  turn (`stream0[i], stream1[i], stream2[i], stream3[i], ...`).

Decoder implementation: `src/vv_huffman.c::vvh_decode4`. The
decoder runs 4 independent bit-readers using a single shared decode
table. The hot loop performs 4 lookups per iteration with no
inter-stream data dependencies, allowing modern OoO CPUs to pipeline
them for ~1.5× decode speedup over single-stream Huffman.

**Backward compatibility**: encoders may suppress `lit_fmt = 4` via
the `vv_options_t::compat_v246_5_decoder` flag, producing output
readable by v2.46.5 and older decoders. With the flag set, the
encode race excludes `lit_fmt = 4` and the output uses only
`lit_fmt ∈ {0, 1, 2, 3}`.

---

## 4. Frame Footer (12 bytes, optional)

Present if and only if `frame_header.flags & 0x01 == 1`:

```
Offset  Size  Field           Description
------  ----  -----           -----------
  0      8    checksum        XXH64 (seed=0) of decompressed frame content
  8      4    footer_magic    0x56564E44 ("VVND")
```

A decoder MUST verify both:
- `footer_magic == 0x56564E44`
- `XXH64(decompressed_bytes) == checksum`

If either check fails, the entire frame is corrupt and decoding
MUST return an error.

For a BCJ-filtered frame, `decompressed_bytes` in the checksum definition means
the BCJ-forward-transformed bytes produced directly by block decoding. The
logical checksum is therefore evaluated on that representation, before the
architecture-specific inverse produces caller-visible bytes. The v2.65.9
reference streaming decoder retains the complete frame output and applies the
inverse exactly once after successful footer/checksum validation; when
`has_checksum` is zero, it applies the inverse after the final block. Another
implementation may process incrementally only if it hashes the transformed
bytes and carries enough BCJ position/boundary state to be exactly equivalent
to one whole-frame inverse; treating chunks or blocks as independent filters is
incorrect. This clarification does not change any on-wire bytes.

---

## 5. LZ Token Format

Used in Block Type 1 and (in stripped form) in Block Type 3 with
tags H/A/I/C. Each token is a **token byte** plus optional
extension fields:

```
Token byte:
  Bits 7-4: literal_length code (0-14, 15=extended via varint)
  Bits 3-0: match_length code  (0-14, 15=extended via varint)
            Real match length is computed as:
              if code < 15: matchlen = code + 4    (VV_MIN_MATCH bias)
              if code = 15: matchlen = code + 4 + varint = 19 + varint
            (Note: the +4 bias is ALWAYS applied. The varint is an
            extension term added on top, NOT a replacement.)
```

Followed by, in order:

1. **Extended literal length** (if litlen code == 15): extension-length varint (see §5.1)
   varint giving `litlen - 15` (additional bytes beyond the first 15).
   Total literal length = 15 + varint value.

2. **Literal bytes**: `litlen` raw bytes (in Block Type 1) or zero
   bytes (in Block Type 3, where literals come from the entropy
   stream).

3. **Offset**: 2 bytes little-endian if frame's `window_log <= 16`,
   3 bytes little-endian otherwise. An offset of 0 is **invalid**.

4. **Extended match length** (if matchlen code == 15): extension-length varint (see §5.1)
   varint. Total match length = 15 + 4 + varint value = 19 + varint.

The decoder then performs:
- Copy `litlen` literal bytes to output.
- Resolve the match: source = `dst_base + (current_position - offset)`,
  copy `matchlen` bytes from source to output.

**dst_base** for matches is the **start of the current frame's
decompressed output**. Matches MUST NOT reach into prior frames'
output — each frame is independently decodable.

If `current_position - offset < 0` (match would read before
`dst_base`), the bitstream is corrupt and decoding MUST error.

A token stream ends when the cumulative output reaches the
block header's `decompressed_size`.

### 5.1 Extension-Length Varint Format

**This is NOT standard LEB128.** It is the lz4-style "byte-sum" varint:

- Read bytes one at a time, summing them as `uint8_t` values.
- Stop reading when a byte's value is < 255.
- The total is the sum of all bytes read (including the final non-255 byte).
- The terminating byte is mandatory, even when it is zero. An empty
  extension or a run ending in `0xFF` is corrupt; the reader must stop at
  the compressed block's boundary. v2.65.10 enforces this in all C token
  paths and aligns the reference decoders' block bounds.

Pseudocode:

```c
size_t read_ext_len(const uint8_t **pp, const uint8_t *end) {
    size_t val = 0;
    while (*pp < end) {
        uint8_t b = *(*pp)++;
        val += b;
        if (b < 255) return val;
    }
    return SIZE_MAX; /* Unterminated: caller must reject before addition. */
}
```

Examples:
- `[0x00]` → 0
- `[0x05]` → 5
- `[0xFE]` → 254
- `[0xFF, 0x00]` → 255
- `[0xFF, 0x01]` → 256
- `[0xFF, 0xFF, 0x07]` → 517

Length encoding:
- `litlen` field where code == 15: `litlen = 15 + read_ext_len(...)`
- `matchlen` field where code == 15: `matchlen = (15 + 4) + read_ext_len(...) = 19 + read_ext_len(...)`

---

## 6. Multi-Frame Streams

A `.vv` stream is a concatenation of independent frames. Each
frame:
- Has its own header, blocks, and (optional) footer.
- Has its own `dst_base` for match resolution (start of its own
  decompressed output, NOT start of the file).
- Has its own checksum (covering only its own content).
- Has its own optional BCJ inverse, applied once at that frame's completion
  using the ordering in Section 4.

This allows:
- Parallel encoding (`vv_compress_mt`).
- Append-mode writes (concatenate a new frame to existing file).
- Random-access seeking (every frame is a recovery point).

A reader can compute the total decompressed size of a multi-frame
file by walking frame headers and summing each frame's
`content_size`. The reference CLI does this in `src/main.c`.

---

## 7. Worked Example — Decoding a Minimal Frame

Hex bytes for compressing the 11-byte string `"hello world"` with
default options (`vaptvupt -c -m balanced`):

```
Offset  Bytes                          Meaning
------  ------------------------------  -------
   0    00 01 56 56                    magic field, little-endian
                                          read as uint32 LE = 0x56560100
   4    01                              version = 1
   5    01                              flags = 1 (has_checksum)
   6    01                              mode_hint = BALANCED
   7    10                              window_log = 16
   8    0B 00 00 00 00 00 00 00         content_size = 11

  16    5C 00 00 00                    block header (LE uint32 = 0x0000005C):
                                          bits 0-1 = 0   → type RAW (0)
                                          bit 2    = 1   → last block
                                          bits 3+ = 0xB → dsz = 11
  20    68 65 6C 6C 6F 20 77 6F 72 6C 64
                                        11 raw bytes: "hello world"

  31    68 69 1E B2 34 67 AB 45         checksum (XXH64 LE)
  39    44 4E 56 56                     footer_magic = 0x56564E44 ("VVND")

Total: 43 bytes
```

A small input like 11 bytes compresses to **larger** than the
input — there is no LZ benefit and the frame overhead (16 + 4 + 12
= 32 bytes) dominates. This is expected behavior; encoders that
want to avoid expansion should compare `vv_compress` output size
against input size and store raw if larger.

To verify against your own implementation:

```sh
echo -n "hello world" > /tmp/h
./vaptvupt -c -m balanced -o /tmp/h.vv /tmp/h
od -A x -t x1z -v /tmp/h.vv
```

(The checksum value will differ in this specific example only if
the encoder used a different checksum seed — the format mandates
seed=0, so it should be reproducible byte-for-byte.)

---

## 8. Stability Promise

This format is **stable since v1.0.0** of the reference encoder
(committed in early 2026). All future v1 frames produced by any
v1.x encoder MUST be decodable by any v1.x decoder, in either
direction.

A future format version 2 — if needed — will:
- Increment the `version` field in the frame header.
- Use a different magic value or be detectable via the version byte.
- Be opt-in at encoder; decoders MAY choose to support both.

The current reference decoder rejects any frame with
`version != 1` so that future format changes do not silently
corrupt old decoders.

---

## 9. Quick Reference

| Value | Meaning |
|-------|---------|
| Frame magic | `0x00 0x01 0x56 0x56` in stream order (LE uint32 = `0x56560100`) |
| Footer magic | `0x44 0x4E 0x56 0x56` in stream order (LE uint32 = `0x56564E44`, "VVND") |
| Min frame size | 16 bytes (header) + 4 bytes (last empty block) = 20 |
| Max block dsz | 1,048,576 bytes (encoder cap) |
| Min match length | 4 bytes (VV_MIN_MATCH) |
| Hash function | xxhash-style 5-byte primary, 4-byte secondary |
| Checksum function | XXH64 with seed=0 over decompressed bytes |
| Entropy coder | tANS (table size 4096, log2=12) |

---

## 10. Differences from Other Codecs

| Aspect | VaptVupt | gzip/DEFLATE | zstd |
|--------|----------|--------------|------|
| Min match | 4 bytes | 3 bytes | 3 bytes |
| Block size | 1 MB | 32 KB | up to 128 MB |
| Entropy | tANS (selectable per block) | Huffman | tANS + Huffman |
| Checksum | XXH64 (per frame) | CRC-32 | XXH64 |
| Multi-frame | Native (concatenate) | Native | Native |
| Streaming | Yes (`vv_cstream_*`) | Yes | Yes |
| Random access | Per frame | No | Per frame |
