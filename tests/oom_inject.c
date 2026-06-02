/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Allocation-failure injector for OOM-robustness testing. Build as a shared
 * library and LD_PRELOAD it; it counts malloc/calloc/realloc calls and forces
 * the call whose index equals $VV_OOM_FAIL to return NULL, so each allocation
 * site in the program under test can be failed in turn. real_* are resolved
 * with dlsym(RTLD_NEXT, ...) so that, under AddressSanitizer, allocations
 * still flow through ASan and leaks/use-after-free on the error paths are
 * detected. Set $VV_OOM_REPORT to print the total allocation count on exit.
 *
 * This file is test scaffolding; it is never linked into the codec.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <stdio.h>

static void *(*real_malloc)(size_t) = NULL;
static void *(*real_calloc)(size_t, size_t) = NULL;
static void *(*real_realloc)(void *, size_t) = NULL;
static long g_count = 0;
static long g_fail_at = -1;
static int  g_armed = 0;

__attribute__((constructor)) static void vv_oom_init(void) {
    real_malloc  = dlsym(RTLD_NEXT, "malloc");
    real_calloc  = dlsym(RTLD_NEXT, "calloc");
    real_realloc = dlsym(RTLD_NEXT, "realloc");
    const char *e = getenv("VV_OOM_FAIL");
    if (e) { g_fail_at = atol(e); g_armed = 1; }
}
__attribute__((destructor)) static void vv_oom_report(void) {
    if (getenv("VV_OOM_REPORT")) fprintf(stderr, "OOM_ALLOC_COUNT=%ld\n", g_count);
}

void *malloc(size_t n) {
    if (!real_malloc) vv_oom_init();
    if (g_armed && g_count++ == g_fail_at) return NULL;
    return real_malloc(n);
}
void *calloc(size_t a, size_t b) {
    if (!real_calloc) vv_oom_init();
    if (g_armed && g_count++ == g_fail_at) return NULL;
    return real_calloc(a, b);
}
void *realloc(void *p, size_t n) {
    if (!real_realloc) vv_oom_init();
    if (g_armed && g_count++ == g_fail_at) return NULL;
    return real_realloc(p, n);
}
