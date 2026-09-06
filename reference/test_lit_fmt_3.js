/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Round-trip regression test for the JavaScript reference decoder
 * against C-encoded frames covering the `lit_fmt = 3` (single-stream
 * Huffman) literal format.
 *
 * Sprint 117 (v2.47.9): added when `lit_fmt = 3` support was ported
 * from `src/vv_huffman.c` to `reference/vv_decoder.js`. Closes the
 * JavaScript half of `AUDIT.md` Section 8 item 6 (reference-decoder
 * coverage gap).
 *
 * Workflow:
 *   1. Build a corpus of inputs (text, binary, mixed)
 *   2. Encode each with `tests/encode_compat`, which sets
 *      `compat_v246_5_decoder = 1` to force `lit_fmt = 3` instead of
 *      the default `lit_fmt = 4`
 *   3. Decode each with the JavaScript reference decoder
 *   4. Assert byte-equality with the original input
 *
 * Run from the repo root:
 *   node reference/test_lit_fmt_3.js
 *
 * Requires `tests/encode_compat` (built from `tests/encode_compat.c`).
 */
'use strict';

const fs = require('fs');
const path = require('path');
const { execFileSync } = require('child_process');
const assert = require('assert');

const here = __dirname;
const decoder = require(path.join(here, 'vv_decoder.js'));

// A complete four-literal sequence frame lets these tests reach Huffman
// through the public API without requiring a C encoder or a checksum.
function literalFrame(litFmt, encoded) {
    const trailer = Buffer.alloc(22);
    trailer.writeUInt16LE(2, 8);  // LL table header size; ML/OF unused
    trailer[10] = 1;             // single-symbol ANS table
    trailer[11] = 4;             // literal-run length code = 4
    const literals = Buffer.alloc(10);
    literals[0] = 'S'.charCodeAt(0);
    literals.writeUInt32LE(4, 1);
    literals[5] = litFmt;
    literals.writeUInt32LE(encoded.length, 6);
    const payload = Buffer.concat([literals, encoded, trailer]);
    const header = Buffer.alloc(23);
    header.writeUInt32LE(decoder._constants.VV_MAGIC, 0);
    header[4] = decoder._constants.VV_VERSION;
    header[7] = 16;
    header.writeBigUInt64LE(4n, 8);
    header.writeUInt32LE((4 << 3) | 4 | decoder._constants.BLOCK_ENTROPY, 16);
    header.writeUIntLE(payload.length, 20, 3);
    return Buffer.concat([header, payload]);
}

for (const codeLength of [1, 15]) {
    const single = Buffer.concat([
        Buffer.from([0, codeLength << 4]),
        Buffer.alloc(Math.ceil(4 * codeLength / 8)),
    ]);
    assert.deepStrictEqual(Buffer.from(decoder.decompress(literalFrame(3, single))),
                           Buffer.alloc(4));
    assert.throws(() => decoder.decompress(literalFrame(3, single.subarray(0, -1))),
                  /Huffman: bitstream exhausted/);
    const laneSize = Math.ceil(codeLength / 8);
    const four = Buffer.concat([
        Buffer.from([0, codeLength << 4, laneSize, 0, 0,
                     laneSize, 0, 0, laneSize, 0, 0]),
        Buffer.alloc(4 * laneSize),
    ]);
    assert.deepStrictEqual(Buffer.from(decoder.decompress(literalFrame(4, four))),
                           Buffer.alloc(4));
    for (let lane = 0; lane < 4; lane++) {
        const truncated = Buffer.from(four.subarray(0, -1));
        if (lane) truncated[2 + (lane - 1) * 3]--;
        assert.throws(() => decoder.decompress(literalFrame(4, truncated)),
                      /Huffman4: (bitstream exhausted|zero-length stream)/);
    }
}
console.log('  PASS Huffman input exhaustion (short/long codes, all four lanes)');
if (process.argv.includes('--huffman-only')) process.exit(0);

// Locate the encode_compat tool
const candidates = [
    path.join(here, '..', 'tests', 'encode_compat'),
    path.join(here, '..', 'encode_compat'),
    '/tmp/encode_compat',
];
let encodeCompat = null;
for (const c of candidates) {
    try {
        fs.accessSync(c, fs.constants.X_OK);
        encodeCompat = c;
        break;
    } catch (e) { /* keep trying */ }
}

if (!encodeCompat) {
    console.error(
        'SKIP: encode_compat tool not found. Build with:\n' +
        '  cc -Iinclude tests/encode_compat.c src/*.c -mavx2 -O2 \\\n' +
        '    -o tests/encode_compat\n' +
        `Searched: ${candidates.join(', ')}`);
    process.exit(77);  // Autoconf "test skipped" convention
}

// Corpus
const cases = [
    ['tiny_repetitive_300B', Buffer.from('abc'.repeat(100))],
    ['medium_text_4kb',
        Buffer.from('The quick brown fox jumps over the lazy dog. '.repeat(90))],
    ['large_text_64kb', Buffer.from('Hello, world!\n'.repeat(4682))],
    ['256_byte_ramp', Buffer.from(Array.from({ length: 256 }, (_, i) => i))],
    ['100kb_repeating_phrase',
        Buffer.from(('the quick brown fox jumps over the lazy dog. '
            .repeat(2222)).slice(0, 100000))],
    ['structured_records', (() => {
        const parts = [];
        for (let i = 0; i < 5000; i++) {
            parts.push(`record_${i}: value=${i * 1.5}, tag=item_${i % 50}; `);
        }
        return Buffer.from(parts.join('').slice(0, 500000));
    })()],
];

// Add silesia slices if present
const silesia = '/tmp/silesia';
if (fs.existsSync(silesia)) {
    for (const f of ['dickens', 'xml', 'sao']) {
        const p = path.join(silesia, f);
        if (fs.existsSync(p)) {
            cases.push([`silesia_${f}_64kb`, fs.readFileSync(p).slice(0, 65536)]);
        }
    }
}

// And a real binary
if (fs.existsSync('/bin/bash')) {
    cases.push(['bash_binary', fs.readFileSync('/bin/bash')]);
}

console.log(`Running ${cases.length} round-trip tests for ` +
            `\`lit_fmt = 3\` (JavaScript reference)...\n`);

const tmpIn = '/tmp/_test_lit_fmt_3_in';
const tmpOut = '/tmp/_test_lit_fmt_3_out.vv';
let passed = 0;
let failed = 0;

for (const [name, data] of cases) {
    try {
        fs.writeFileSync(tmpIn, data);
        execFileSync(encodeCompat, [tmpIn, tmpOut],
                     { stdio: ['ignore', 'ignore', 'pipe'], timeout: 30000 });
        const encoded = fs.readFileSync(tmpOut);
        const decoded = decoder.decompress(encoded);
        if (Buffer.compare(Buffer.from(decoded), data) === 0) {
            const ratio = (encoded.length / Math.max(data.length, 1) * 100)
                          .toFixed(1);
            console.log(`  PASS ${name}: ${data.length} → ` +
                        `${encoded.length} bytes (${ratio}%)`);
            passed++;
        } else {
            console.log(`  FAIL ${name}: decoded ${decoded.length} bytes, ` +
                        `expected ${data.length}`);
            if (decoded.length === data.length) {
                for (let i = 0; i < data.length; i++) {
                    if (decoded[i] !== data[i]) {
                        console.log(`     first diff at byte ${i}: ` +
                                    `got 0x${decoded[i].toString(16)}, ` +
                                    `expected 0x${data[i].toString(16)}`);
                        break;
                    }
                }
            }
            failed++;
        }
    } catch (e) {
        console.log(`  FAIL ${name}: ${e.constructor.name}: ` +
                    `${e.message.substring(0, 120)}`);
        failed++;
    }
}

// Cleanup
try { fs.unlinkSync(tmpIn); } catch (e) { /* ignore */ }
try { fs.unlinkSync(tmpOut); } catch (e) { /* ignore */ }

console.log(`\nResults: ${passed} passed, ${failed} failed`);
process.exit(failed === 0 ? 0 : 1);
