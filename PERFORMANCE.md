# VaptVupt Performance Characteristics

**Document version**: 1.1 (Sprint 121)
**Codebase measured**: v2.48.2
**Comparison baseline**: zstd 1.5.6
**Hardware**: x86_64 with AVX2

---

## 1. Headline Numbers

VaptVupt's stated performance goal is "beat zstd on decode speed across all file types". The aggregate decode-speed result is:

| Codec | Decode throughput (aggregate, 8 fixtures) |
|---|---|
| **VaptVupt v2.48.1** | **151 MB/s** |
| zstd-3 | 119 MB/s |
| **Speedup** | **1.27×** |

Decode-speed goal is **met on 7 of 8 fixtures**; xml is the one loss.

The codec is intentionally ratio-asymmetric and decode-asymmetric vs encode: better decode than encode, slightly worse aggregate ratio than zstd-3 (+1.2%). This trade-off is the right one for Zupt's deployment profile (write once, restore many times under time pressure).

---

## 2. Decode Speed (Detailed)

Measured with `time.perf_counter()` over 7 runs, median reported. 2 warmup runs per measurement. Wall-clock time including process startup.

| Fixture | Raw size | vv decode | zstd-3 decode | vv/zstd |
|---|---:|---:|---:|---:|
| fx_text | 761 KB | 6.1 ms / **125 MB/s** | 10.3 ms / 74 MB/s | **1.69×** |
| fx_json | 1.0 MB | 7.4 ms / **139 MB/s** | 10.2 ms / 100 MB/s | 1.38× |
| fx_source | 1.0 MB | 6.6 ms / **158 MB/s** | 11.4 ms / 92 MB/s | **1.71×** |
| bash | 1.4 MB | 10.8 ms / **134 MB/s** | 12.6 ms / 115 MB/s | 1.16× |
| dickens | 10.2 MB | 56.8 ms / **179 MB/s** | 81.3 ms / 125 MB/s | 1.43× |
| xml | 5.3 MB | 18.6 ms / 288 MB/s | 17.3 ms / **308 MB/s** | 0.93× |
| sao | 7.3 MB | 57.6 ms / **126 MB/s** | 77.2 ms / 94 MB/s | 1.34× |
| x-ray | 8.5 MB | 71.1 ms / **119 MB/s** | 77.8 ms / 109 MB/s | 1.09× |
| **Aggregate** | **35.5 MB** | **235.0 ms / 151 MB/s** | 298.0 ms / 119 MB/s | **1.27×** |

### Why decode is fast

VaptVupt's decode path uses three optimizations that account for most of the speedup vs zstd:

1. **4-stream interleaved Huffman** (Sprint 103-104): 4 independent bit-readers feeding a shared 12-bit decode table. Fills 4 ILP slots per cycle. Average 1.54× decode speedup over the prior 1-stream Huffman path.

2. **Manual hot-loop macro-inlining**: the LZ literal-copy + match-copy loop in `decode_block_tokens_impl` is split into "warmup" (pre-safe-zone) and "hot" (in-safe-zone) phases. The hot phase eliminates per-iteration bounds checks by relying on safe-zone slack (48 bytes input, 72 bytes output).

3. **AVX2 SIMD memcpy** for match copies ≥16 bytes. Uses a single 256-bit load + store per cycle.

### Why xml is slower

The xml fixture has many small repeats with very short offsets (3-byte structural patterns like `<a>`). zstd's match-copy primitive handles this case slightly better. The overhead of vaptvupt's 4-stream Huffman setup (one-time table build per block) is amortized over fewer literal bytes when match coverage is high. Future work could close this gap with a small-offset fast path.

---

## 3. Encode Speed (Detailed)

**vv is meaningfully slower than zstd on encode**. This is a known trade-off — vv prioritizes decode speed and ratio over encode speed.

| Fixture | Mode | vv encode | zstd encode | vv/zstd |
|---|---|---:|---:|---:|
| fx_text | balanced / -3 | 45.1 ms / 17 MB/s | 13.2 ms / 58 MB/s | 0.29× |
| fx_text | extreme / -9 | 120.5 ms / 6 MB/s | 35.5 ms / 21 MB/s | 0.29× |
| fx_json | balanced / -3 | 62.6 ms / 16 MB/s | 14.7 ms / 70 MB/s | 0.24× |
| fx_json | extreme / -9 | 151.4 ms / 7 MB/s | 59.7 ms / 17 MB/s | 0.39× |
| bash | balanced / -3 | 128.5 ms / 11 MB/s | 21.0 ms / 69 MB/s | 0.16× |
| dickens | balanced / -3 | 948.4 ms / 11 MB/s | 124.3 ms / 82 MB/s | 0.13× |
| dickens | extreme / -9 | 2654 ms / 4 MB/s | 460.7 ms / 22 MB/s | 0.17× |
| xml | balanced / -3 | 166.7 ms / 32 MB/s | 29.5 ms / 181 MB/s | 0.18× |

### Why encode is slow

Three structural choices favor ratio over encoder throughput:

1. **Lazy parsing** in `balanced` mode: at every position, the encoder looks one byte ahead to see if a longer match is available. Doubles the hash-table probe count vs greedy parsing.
2. **Optimal parsing** in `extreme` mode: full LZ77 cost minimization via dynamic programming over the entire block. Up to 10× slower than greedy.
3. **Separate hash chains for hash4 and hash5**: Sprint 14 fix that mandated separate chain arrays after the shared-chain approach caused silent corruption. Adds ~30% to encode time on text fixtures.

For Zupt's use case (backup once, restore many times) this trade-off is acceptable. For latency-sensitive use cases (real-time compression on the hot path), prefer zstd-3.

---

## 4. Compression Ratio

Measured on the standard 8-fixture benchmark suite. All values are compressed-size in bytes (smaller is better). **vv-extreme** is the recommended mode for ratio-priority workloads; **balanced** trades ~0.5% ratio for ~3× encode speed.

### Extreme mode (Sprint 121, v2.48.1)

| Fixture | Raw | vv-extreme | zstd-3 | vv/zstd |
|---|---:|---:|---:|---:|
| fx_text | 761,125 | 128,238 | 137,790 | **−6.93%** ✓ |
| fx_json | 1,024,000 | 198,213 | 203,276 | **−2.49%** ✓ |
| fx_source | 1,048,576 | 206,350 | 195,078 | +5.78% |
| bash | 1,446,024 | 738,698 | 727,132 | +1.59% |
| dickens | 10,192,446 | 3,818,656 | 3,669,252 | +4.07% |
| xml | 5,345,280 | 643,067 | 639,138 | +0.61% |
| sao | 7,251,944 | 5,410,425 | 5,551,158 | **−2.54%** ✓ |
| x-ray | 8,474,240 | 5,881,236 | 6,086,279 | **−3.37%** ✓ |
| **Aggregate** | **35,543,635** | **17,024,883** | **17,209,103** | **−1.07%** ✓ |

**VaptVupt-extreme beats zstd-3 on aggregate ratio by 1.07%** and beats it per-fixture on 4 of 8 fixtures (fx_text, fx_json, sao, x-ray). The remaining gaps (fx_source, bash, dickens, xml) have all narrowed substantially since v2.47.x and the trajectory is open.

### Trajectory (last 5 versions)

| Release | Aggregate vs zstd-3 | Per-fixture wins |
|---|---|---|
| v2.47.4 | +1.4% (behind) | 4 of 8 |
| v2.47.10 | +1.2% (behind) | 3 of 8 |
| v2.48.0 (Sprint 120) | **−0.13% (ahead)** | 4 of 8 |
| **v2.48.1 (Sprint 121)** | **−1.07% (ahead)** | 4 of 8 |

The Sprint 120 cost-aware lazy parser closed the +1.2% gap entirely; Sprint 121's `mlen<8` gate plus per-mode cost calibration extended the lead to −1.07%. See `CHANGELOG.md` for detail.

### vs gzip-9 and lz4

VaptVupt-extreme also beats gzip-9 on aggregate ratio (gzip-9 is consistently 5-15% larger than zstd-3). VaptVupt-fast beats lz4 on ratio across every fixture in the suite by 13-20%.

---

## 5. Memory Footprint

### Decoder
- Stateless (`vv_decompress`): zero internal allocation beyond the caller-provided output buffer. All scratch is stack-allocated.
- Streaming (`vv_dstream_*`): ~1 MB persistent context (decode tables + window buffer). Single malloc on `vv_dstream_create`.

### Encoder
- Stateless (`vv_compress`): ~16 MB hash tables + 1 MB literal buffer + 1 MB tokenized output. ~18 MB total per call. Released on return.
- Streaming (`vv_cstream_*`): same allocations, persistent across the stream.
- Multi-threaded (`vv_compress_mt`): per-thread allocation of ~18 MB. Recommended worker count: physical core count, capped at 8.

---

## 6. Recommended Configuration

For Zupt (secure backup):

```c
vv_options_t opts;
vv_default_options(&opts);
opts.mode = VV_MODE_BALANCED;     /* good ratio/speed trade-off */
opts.checksum = 1;                 /* default; 32-bit xxhash on plaintext */
opts.window_log = 0;               /* default 16 MB window; tune down for low-memory */

vv_compress(src, src_len, dst, dst_cap, &opts);
```

For latency-sensitive compression where decode speed matters most:
- Use `VV_MODE_BALANCED` (default). Ultra-fast mode is faster to encode but ratio is much worse.

For maximum ratio (slow encode, fast decode):
- Use `VV_MODE_EXTREME` and accept ~3× slower encode for ~2-3% better ratio on text.

---

## 7. Methodology Notes

- Measurements include process startup. For embedded library use (no startup cost), in-process throughput is ~10-20% higher than these numbers.
- Cold-cache effects: warmup runs are excluded from the median.
- The 8-fixture benchmark suite covers source code, JSON, XML, logs, binary, structured data, and astronomy data. Coverage gaps: video, audio, encrypted/random data (these compress poorly with any LZ-family codec).
- All measurements made on a single machine. Cross-machine variance is ±10% typical.

---

## 8. Comparison Caveats

zstd-3 is a default-quality preset. Other zstd levels:
- zstd-1 is faster than zstd-3 to encode and slightly worse ratio. vv would lose more on encode-speed comparison vs zstd-1.
- zstd-19 (highest) has much better ratio than vv but much slower encode. vv would win decode-speed comparison vs zstd-19 by a larger margin.

zstd-3 is the right comparison point because it's the most common default-tuning real-world deployment.
