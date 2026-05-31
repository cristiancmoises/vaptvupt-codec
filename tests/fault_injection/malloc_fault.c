/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Malloc-fault injection harness for VaptVupt audit.
 *
 * Usage: VV_FAULT_RATE=N VV_FAULT_SEED=S LD_PRELOAD=/tmp/malloc_fault.so prog
 *   VV_FAULT_RATE: 1-1000 (failures per 1000 allocations, default 0)
 *   VV_FAULT_SEED: PRNG seed (default 1)
 *   VV_FAULT_SKIP: skip first N allocations (default 100, lets startup
 *                  complete before failure injection begins)
 *
 * Wraps malloc, calloc, realloc. After SKIP allocations, fails RATE/1000
 * of them at random. Records statistics, prints summary at exit.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static void *(*real_malloc)(size_t) = NULL;
static void *(*real_calloc)(size_t, size_t) = NULL;
static void *(*real_realloc)(void *, size_t) = NULL;

static unsigned int rate = 0;
static unsigned int seed = 1;
static unsigned int skip = 100;
static unsigned int n_calls = 0;
static unsigned int n_failures = 0;
static unsigned int rng_state = 0;
static int initialized = 0;

static unsigned int xorshift(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static void init_once(void) {
    if (initialized) return;
    initialized = 1;
    real_malloc  = dlsym(RTLD_NEXT, "malloc");
    real_calloc  = dlsym(RTLD_NEXT, "calloc");
    real_realloc = dlsym(RTLD_NEXT, "realloc");
    const char *r = getenv("VV_FAULT_RATE");
    const char *s = getenv("VV_FAULT_SEED");
    const char *k = getenv("VV_FAULT_SKIP");
    if (r) rate = (unsigned int)atoi(r);
    if (s) seed = (unsigned int)atoi(s);
    if (k) skip = (unsigned int)atoi(k);
    rng_state = seed ? seed : 1;
}

static int should_fail(void) {
    if (rate == 0) return 0;
    if (n_calls < skip) return 0;
    return (xorshift() % 1000) < rate;
}

void *malloc(size_t sz) {
    init_once();
    n_calls++;
    if (should_fail()) { n_failures++; return NULL; }
    return real_malloc(sz);
}

void *calloc(size_t n, size_t sz) {
    init_once();
    /* Glibc calls calloc during dlsym, so be careful */
    if (!real_calloc) {
        /* Fallback: use static buffer for the dlsym calloc */
        static char buf[4096]; static size_t off = 0;
        size_t total = n * sz;
        if (off + total > sizeof(buf)) return NULL;
        void *p = buf + off; off += total;
        memset(p, 0, total);
        return p;
    }
    n_calls++;
    if (should_fail()) { n_failures++; return NULL; }
    return real_calloc(n, sz);
}

void *realloc(void *p, size_t sz) {
    init_once();
    n_calls++;
    if (should_fail()) { n_failures++; return NULL; }
    return real_realloc(p, sz);
}

__attribute__((destructor))
static void summary(void) {
    if (rate > 0) {
        fprintf(stderr, "[fault-inject] %u allocs, %u failed (rate=%u/1000, seed=%u)\n",
                n_calls, n_failures, rate, seed);
    }
}
