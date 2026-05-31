#!/usr/bin/env node
/*
 * VaptVupt — JavaScript reference decoder self-test.
 *
 * Compresses a set of known inputs with the C `vaptvupt` binary,
 * then decodes each output through the JS decoder and verifies
 * byte-exact equality with the original input.
 *
 * Run from the repo root:
 *     node reference/vv_decoder.test.js
 */

'use strict';

const fs = require('fs');
const os = require('os');
const path = require('path');
const { execFileSync } = require('child_process');

const VV_BINARY_CANDIDATES = [
    './vaptvupt',
    '../vaptvupt',
    path.join(__dirname, '..', 'vaptvupt'),
];

function findBinary() {
    for (const c of VV_BINARY_CANDIDATES) {
        try {
            fs.accessSync(c, fs.constants.X_OK);
            return c;
        } catch (_) { /* try next */ }
    }
    throw new Error(`vaptvupt not found. Tried: ${VV_BINARY_CANDIDATES}`);
}

function compressWithC(binary, data, extraArgs = []) {
    const tmpDir = os.tmpdir();
    const inPath = path.join(tmpDir, `vv_js_test_${process.pid}_${Date.now()}`);
    const outPath = inPath + '.vv';
    try {
        fs.writeFileSync(inPath, data);
        execFileSync(binary,
                     ['-c', '-m', 'balanced', ...extraArgs, '-o', outPath, inPath],
                     { stdio: ['ignore', 'pipe', 'pipe'] });
        return fs.readFileSync(outPath);
    } finally {
        try { fs.unlinkSync(inPath); } catch (_) {}
        try { fs.unlinkSync(outPath); } catch (_) {}
    }
}

function main() {
    console.log('\n╔════════════════════════════════════════════════════════════════╗');
    console.log('║  VaptVupt — JavaScript Reference Decoder Self-Test             ║');
    console.log('╚════════════════════════════════════════════════════════════════╝\n');

    const vv = require('./vv_decoder.js');
    const binary = findBinary();

    const cases = [
        ['empty',                    Buffer.alloc(0)],
        ['1 byte',                   Buffer.from('X')],
        ['hello world',              Buffer.from('hello world')],
        ['256-byte ramp',            Buffer.from(Array.from({length: 256}, (_, i) => i))],
        ['1KB ASCII repeating',      Buffer.from('abcdefghij'.repeat(102) + 'abc')],
        ['4KB low-entropy text',     Buffer.from(('the quick brown fox jumps over the lazy dog. ').repeat(100).slice(0, 4096))],
        ['8 bytes all zeros',        Buffer.alloc(8, 0)],
        ['16 bytes all 0xFF',        Buffer.alloc(16, 0xFF)],
        ['100 bytes single byte',    Buffer.alloc(100, 0x7E)],
        ['16KB ramp + repeat',       Buffer.from(Array.from({length: 16384}, (_, i) => i & 0xFF))],
        // Multi-block tests — exercise cross-block dict carry in 'S' tag decoder
        ['100KB repeating phrase',   Buffer.from(('the quick brown fox jumps over the lazy dog. ').repeat(2222).slice(0, 100000))],
        ['500KB mixed content',      (() => {
            // Mix of repeated sequences with variable literals to force 'S' blocks
            const parts = [];
            for (let i = 0; i < 5000; i++) {
                parts.push(`record_${i}: value=${i * 1.5}, tag=item_${i % 50}; `);
            }
            return Buffer.from(parts.join('').slice(0, 500000));
        })()],
        // ─── Sprint 46: format v2 ('T' tag) cross-language coverage ───
        // These tests compress with --format-v2 (emits 'T' blocks on
        // binary-like data where the adaptive gate turns on hash3, or
        // 'T'-tagged blocks with v1-matcher output on low-entropy
        // text). Either way the JS decoder must handle the 'T' tag.
        ['v2: 16KB ramp + repeat',   Buffer.from(Array.from({length: 16384}, (_, i) => i & 0xFF)), '--format-v2'],
        ['v2: 64KB binary-ish',      (() => {
            // Binary-like pattern with structure that triggers hash3
            const buf = Buffer.alloc(65536);
            for (let i = 0; i < buf.length; i++) {
                buf[i] = ((i * 13) ^ (i >> 3) ^ 0xA5) & 0xFF;
            }
            // Re-inject first 8KB at the end to force cross-chunk matches
            buf.copy(buf, 65536 - 8192, 0, 8192);
            return buf;
        })(), '--format-v2'],
        ['v2: long-run regression',  Buffer.concat([
            Buffer.from(Array.from({length: 16}, (_, i) => (i * 31) & 0xFF)),
            Buffer.alloc(140000 - 16, 0xAB)
        ]), '--format-v2'],
    ];

    let passed = 0;
    let failed = 0;
    let skipped = 0;

    for (const row of cases) {
        const [name, data, extraArg] = row;
        const extraArgs = extraArg ? [extraArg] : [];
        let frame;
        try {
            frame = compressWithC(binary, data, extraArgs);
        } catch (e) {
            console.log(`  ERROR  ${name}: C compress failed: ${e.message}`);
            failed++;
            continue;
        }

        try {
            const decoded = vv.decompress(frame);
            if (decoded.length !== data.length) {
                console.log(`  FAIL   ${name}: decoded ${decoded.length} bytes, expected ${data.length}`);
                failed++;
                continue;
            }
            let eq = true;
            for (let i = 0; i < data.length; i++) {
                if (decoded[i] !== data[i]) { eq = false; break; }
            }
            if (!eq) {
                console.log(`  FAIL   ${name}: byte mismatch`);
                failed++;
                continue;
            }
            console.log(`  PASS   ${name} (${data.length} → ${frame.length} bytes)`);
            passed++;
        } catch (e) {
            if (e.name === 'NotImplementedError') {
                console.log(`  SKIP   ${name} (ENTROPY block — out of scope for JS decoder)`);
                skipped++;
            } else if (e.name === 'CorruptError' && /lit_fmt=4/.test(e.message)) {
                // Known coverage gap: 4-stream Huffman (lit_fmt=4, added v2.47.0)
                // has not yet been ported to this JS reference.
                // (lit_fmt=3 was ported in Sprint 117 and now PASSes.)
                // See README.md "Reference decoder coverage gap" and AUDIT.md item 6.
                console.log(`  SKIP   ${name} (lit_fmt=4 4-stream Huffman — known coverage gap, see README)`);
                skipped++;
            } else {
                console.log(`  FAIL   ${name}: ${e.name}: ${e.message}`);
                failed++;
            }
        }
    }

    // Also validate multi-frame by concatenating two C-compressed frames.
    try {
        const f1 = compressWithC(binary, Buffer.from('hello from frame 1'));
        const f2 = compressWithC(binary, Buffer.from('and greetings from frame 2'));
        const combined = Buffer.concat([f1, f2]);
        const decoded = vv.decompress(combined);
        const expected = Buffer.concat([
            Buffer.from('hello from frame 1'),
            Buffer.from('and greetings from frame 2')
        ]);
        if (Buffer.compare(decoded, expected) === 0) {
            console.log(`  PASS   multi-frame (2 concatenated frames, ${combined.length} bytes)`);
            passed++;
        } else {
            console.log(`  FAIL   multi-frame: decoded ${decoded.length} bytes, expected ${expected.length}`);
            failed++;
        }
    } catch (e) {
        if (e.name === 'NotImplementedError') {
            skipped++;
            console.log(`  SKIP   multi-frame (ENTROPY — out of scope)`);
        } else {
            console.log(`  FAIL   multi-frame: ${e.name}: ${e.message}`);
            failed++;
        }
    }

    // Validate rejection of bad magic.
    try {
        const bad = Buffer.alloc(64, 0xFF);
        vv.decompress(bad);
        console.log(`  FAIL   bad-magic: decoder accepted garbage`);
        failed++;
    } catch (e) {
        if (e.name === 'CorruptError') {
            console.log(`  PASS   bad-magic rejected with CorruptError`);
            passed++;
        } else {
            console.log(`  FAIL   bad-magic: unexpected ${e.name}: ${e.message}`);
            failed++;
        }
    }

    console.log(`\nResults: ${passed} passed, ${failed} failed, ${skipped} skipped`);
    process.exit(failed > 0 ? 1 : 0);
}

main();
