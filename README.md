# VaptVupt

**Fast LZ77 + tANS entropy codec**  
Pure C11 · Zero dependencies 

**Compress everything. Trust nothing. Encrypt always.** (codec only)

![License](https://img.shields.io/badge/license-Apache%202.0-blue)
![Language](https://img.shields.io/badge/language-C11-green)
![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS%20%7C%20Windows-lightgrey)
![AVX2](https://img.shields.io/badge/AVX2-SIMD-orange)

---

## Why VaptVupt

- **tANS entropy**: asymptotically optimal, single-instruction decode per symbol
- **4-way interleaved ANS**: 4 symbols per bitstream refill (4× less overhead)
- **AVX2 SIMD decoder**: inline 32-byte copies, tiered offsets, safe-zone path
- **Rep-match + lazy-2**: 3 recent offsets checked O(1), ~30% hit rate
- **Order-1 context model**: captures byte-pair correlations in JSON/CSV/logs
- **3 modes**: Ultra-Fast (greedy), Balanced (default), Extreme (order-1)

Decompresses **2–3× faster** than Zupt-LZHP while matching gzip ratios.

---

## Three Modes

| Mode       | CLI      | Chain Depth | Entropy          | Use Case                  |
|------------|----------|-------------|------------------|---------------------------|
| Ultra-Fast | `-l 1-3` | 4           | None             | Speed / streaming         |
| Balanced   | `-l 4-7` (default) | 48     | 4-way ANS        | General backups           |
| Extreme    | `-l 8-9` | 256         | Order-1 context ANS | Max compression       |

---

## Benchmark (1.9 MB mixed corpus)

| Codec              | Compress | Decompress | Ratio  |
|--------------------|----------|------------|--------|
| **VaptVupt UF**    | 63 MB/s  | **298 MB/s** | 2.7:1 |
| **VaptVupt BAL** (default) | 18 MB/s | **268 MB/s** | 3.5:1 |
| **VaptVupt EXT**   | 12 MB/s  | **311 MB/s** | 3.5:1 |
| Zupt-LZHP (v1)     | 8 MB/s   | 137 MB/s   | 4.0:1 |
| Zupt-LZ            | 28 MB/s  | 348 MB/s   | 3.3:1 |
| gzip -6            | 26 MB/s  | 99 MB/s    | 4.0:1 |

**VaptVupt BAL**: 2× faster decode than previous Zupt default, **2.7× faster** than gzip at similar ratio.

---

## Comparison vs Popular Codecs

| Codec     | Decompress Speed | Ratio (typical) | Dependencies | SIMD     | Notes                          |
|-----------|------------------|-----------------|--------------|----------|--------------------------------|
| **VaptVupt** | **268–311 MB/s** | 3.5:1          | None         | AVX2     | Pure C11, tANS + rep-match     |
| lz4       | ~500+ MB/s      | ~2.0:1         | None         | SSE/AVX  | Faster but weaker ratio        |
| zstd      | ~300–800 MB/s   | 3–5:1          | None         | AVX2     | Larger codebase, more features |
| gzip      | 99 MB/s         | 4.0:1          | None         | None     | Slow decode                    |
| brotli    | ~150–400 MB/s   | 4–6:1          | None         | None     | Heavier, better ratio          |

VaptVupt delivers **best speed/ratio balance** for backup/archival use with minimal code.

---

## Architecture

```
Encoder: 5-byte multiply-shift hash → rep-match (3 offsets) → lazy-2 parsing → AVX2 match extension
Entropy:  tANS (4-way interleaved) + order-1 context
Decoder:  AVX2 inline SIMD copies (32/16/8-byte tiers) + safe-zone fast path
```

---
# Quick Start (as library)

```c
#include "vaptvupt.h"

// Compress
vaptvupt_compress(src, src_len, dst, dst_cap, level);  // level 1-9

// Decompress
vaptvupt_decompress(src, src_len, dst, dst_cap);
```

Full API in `vaptvupt.h`. Zero allocations in hot paths.

Build:
```bash
git clone https://github.com/cristiancmoises/vaptvupt.git
cd vaptvupt && make
```

---

## License

**Apache 2.0** | [LICENSE](LICENSE).

Originally added on [Zupt v2.0-RC](https://github.com/cristiancmoises/zupt/releases) MIT + Apache 2.0.
I'm working alone on this project.

© 2026 Cristian Cezar Moisés
```
