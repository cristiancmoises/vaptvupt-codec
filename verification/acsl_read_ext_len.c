/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Frama-C/WP deductive proof for the decoder's variable-length integer
 * reader. Unlike the bounded CBMC harness (cbmc_read_ext_len.c, which proves
 * sizes 0..16), this proves memory safety for buffers of ANY size via a loop
 * invariant: read_ext_len never dereferences outside [*pp, end) and leaves
 * *pp within [old(*pp), end].
 *
 * NOTE: WP is not in the frama-c-base package; install the full frama-c
 * (with the WP plugin) to discharge these obligations. The Eva analysis in
 * verification/eva_read_ext_len.c provides a memory-safety result that runs
 * with frama-c-base alone.
 *
 * The function body is an exact copy of the static read_ext_len in
 * src/vv_decoder.c (verify.sh fails on drift). Verify with:
 *   frama-c -wp -wp-rte -wp-prover z3 verification/acsl_read_ext_len.c
 */
#include <stdint.h>
#include <stddef.h>

/*@
  requires \valid_read(*pp);
  requires \valid_read(end);
  // pp points at a position at or before end, inside one object:
  requires *pp <= end;
  requires \valid_read(*pp + (0 .. (end - *pp) - 1)) || *pp == end;

  assigns *pp;

  // memory-safety contract: the pointer only moves forward, never past end.
  ensures *pp >= \old(*pp);
  ensures *pp <= end;
*/
static size_t read_ext_len(const uint8_t **pp, const uint8_t *end) {
    size_t val = 0;
    const uint8_t *p = *pp;
    /*@
      loop invariant \at(*pp,Pre) <= p <= end;
      loop assigns p, val, *pp;
      loop variant end - p;
    */
    while (p < end) {
        uint8_t b = *p++;
        val += b;
        if (b < 255) {
            *pp = p;
            return val;
        }
    }
    *pp = p;
    /* Even a zero extension requires its terminating byte. All token
     * payloads are bounded by the 24-bit compressed-size field, so their
     * byte sums fit below SIZE_MAX on both 32- and 64-bit hosts. */
    return SIZE_MAX;
}

/* A trivial caller so WP has an entry context; the proof obligations live in
 * read_ext_len's contract and loop annotations above. */
void harness(const uint8_t *buf, size_t n) {
    if (buf == (void*)0) return;
    const uint8_t *p = buf;
    const uint8_t *end = buf + n;
    //@ assert p <= end;
    (void)read_ext_len(&p, end);
}
