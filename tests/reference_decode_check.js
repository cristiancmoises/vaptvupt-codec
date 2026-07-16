#!/usr/bin/env node
/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * reference_decode_check.js <compressed.vv> <expected_plain>
 *
 * Decodes <compressed.vv> with the JavaScript reference decoder
 * (reference/vv_decoder.js) and compares the result byte-for-byte with
 * <expected_plain>. Exit 0 on an exact match, 1 otherwise. Used by
 * tests/reference_roundtrip.py to fold the JS reference decoder into the
 * default-format regression guard.
 */
'use strict';
const fs = require('fs');
const path = require('path');

const vv = require(path.join(__dirname, '..', 'reference', 'vv_decoder.js'));
const decompress = vv.decompress || (vv.default && vv.default.decompress);
if (typeof decompress !== 'function') {
    console.error('reference/vv_decoder.js does not export decompress()');
    process.exit(2);
}

const [, , vvPath, plainPath] = process.argv;
if (!vvPath || !plainPath) {
    console.error('usage: reference_decode_check.js <compressed.vv> <expected_plain>');
    process.exit(2);
}

let out;
try {
    out = Uint8Array.from(decompress(new Uint8Array(fs.readFileSync(vvPath))));
} catch (e) {
    console.error('JS decode error: ' + String(e).slice(0, 200));
    process.exit(1);
}
const ref = new Uint8Array(fs.readFileSync(plainPath));
if (out.length !== ref.length) {
    console.error(`JS length mismatch: ${out.length} vs ${ref.length}`);
    process.exit(1);
}
for (let i = 0; i < out.length; i++) {
    if (out[i] !== ref[i]) {
        console.error(`JS byte mismatch at ${i}`);
        process.exit(1);
    }
}
process.exit(0);
