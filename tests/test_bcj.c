/* SPDX-License-Identifier: GPL-3.0-or-later
 * BCJ filters (x86 + AArch64): standalone reversibility, full
 * compress/decompress roundtrip, and corrupt-input safety. */
#include "vaptvupt.h"
#include "vv_bcj.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint64_t st=0x9E3779B97F4A7C15ULL;
static uint32_t xr(void){st^=st<<13;st^=st>>7;st^=st<<17;return(uint32_t)(st>>32);}
static int g_pass=0,g_fail=0;
static void ok(int c,const char*m){if(c)g_pass++;else{g_fail++;printf("  FAIL: %s\n",m);}}

static int x86_reversible(const uint8_t*orig,size_t n){
    uint8_t*a=malloc(n?n:1),*b=malloc(n?n:1);
    memcpy(a,orig,n); vv_bcj_x86(a,n,0,1);
    memcpy(b,a,n);     vv_bcj_x86(b,n,0,0);
    int r=(memcmp(b,orig,n)==0); free(a);free(b); return r;
}
static int arm64_reversible(const uint8_t*orig,size_t n){
    uint8_t*a=malloc(n?n:1),*b=malloc(n?n:1);
    memcpy(a,orig,n); vv_bcj_arm64(a,n,0,1);
    memcpy(b,a,n);     vv_bcj_arm64(b,n,0,0);
    int r=(memcmp(b,orig,n)==0); free(a);free(b); return r;
}
/* full API roundtrip; which=0 -> x86 filter, which=1 -> arm64 filter */
static int full_roundtrip(const uint8_t*orig,size_t n,int which){
    uint8_t*c=malloc(vv_compress_bound(n)+64),*o=malloc(n+64);
    vv_options_t opt; vv_default_options(&opt); opt.mode=VV_MODE_BALANCED;
    if(which) opt.filter_arm64=1; else opt.filter_x86=1;
    int64_t cl=vv_compress(orig,n,c,vv_compress_bound(n)+64,&opt);
    int r=0;
    if(cl>0){ int64_t dl=vv_decompress(c,(size_t)cl,o,n+64); r=(dl==(int64_t)n && memcmp(o,orig,n)==0); }
    free(c);free(o); return r;
}
/* corrupt-input safety: compress with a filter, bit-flip the stream, decode.
 * The decoder must never read/write out of bounds; it may return an error or
 * (since flips can yield a still-valid stream) succeed. We only require that
 * it returns and does not corrupt memory (caught by ASan/UBSan in the
 * sanitizer build). Returns 1 always unless it would crash. */
static int corrupt_decode(const uint8_t*orig,size_t n,int which){
    uint8_t*c=malloc(vv_compress_bound(n)+64),*o=malloc(n+256);
    vv_options_t opt; vv_default_options(&opt); opt.mode=VV_MODE_BALANCED;
    opt.checksum=(xr()&1); if(which) opt.filter_arm64=1; else opt.filter_x86=1;
    int64_t cl=vv_compress(orig,n,c,vv_compress_bound(n)+64,&opt);
    if(cl>20){
        int flips=1+(int)(xr()%5);
        for(int f=0;f<flips;f++){ size_t pos=16+(xr()%((size_t)cl-16)); c[pos]^=(uint8_t)(1u<<(xr()&7)); }
        volatile int64_t dl=vv_decompress(c,(size_t)cl,o,n+256); (void)dl;
    }
    free(c);free(o); return 1;
}

int main(void){
    printf("BCJ filter tests (x86 + AArch64):\n");

    /* ---- x86 ---- */
    for(int t=0;t<2000;t++){
        size_t n=xr()%8192; uint8_t*b=malloc(n?n:1);
        for(size_t i=0;i<n;i++){b[i]=(uint8_t)xr();}
        ok(x86_reversible(b,n),"x86 reversible random"); free(b);
    }
    for(size_t n=0;n<=20;n++){ uint8_t*b=malloc(n?n:1);
        memset(b,0xE8,n); ok(x86_reversible(b,n),"x86 reversible all-E8");
        memset(b,0xE9,n); ok(x86_reversible(b,n),"x86 reversible all-E9"); free(b); }
    for(int t=0;t<300;t++){
        size_t n=8+xr()%16384; uint8_t*b=malloc(n);
        for(size_t i=0;i<n;i++){b[i]=(uint8_t)((xr()%100<60)?0xE8:xr());}
        ok(full_roundtrip(b,n,0),"x86 full roundtrip"); free(b);
    }

    /* ---- AArch64 ---- */
    for(int t=0;t<2000;t++){
        size_t n=xr()%8192; uint8_t*b=malloc(n?n:1);
        for(size_t i=0;i<n;i++){b[i]=(uint8_t)xr();}
        ok(arm64_reversible(b,n),"arm64 reversible random"); free(b);
    }
    /* adversarial: all-BL and all-ADRP words with random immediates */
    for(size_t w=0;w<=64;w++){ size_t n=w*4; uint8_t*b=malloc(n?n:1);
        for(size_t i=0;i<w;i++){uint32_t r=xr();uint32_t insn=0x94000000u|(r&0x03FFFFFFu);b[i*4]=(uint8_t)insn;b[i*4+1]=(uint8_t)(insn>>8);b[i*4+2]=(uint8_t)(insn>>16);b[i*4+3]=(uint8_t)(insn>>24);}
        ok(arm64_reversible(b,n),"arm64 reversible all-BL");
        for(size_t i=0;i<w;i++){uint32_t r=xr();uint32_t insn=0x90000000u|((r&0x3u)<<29)|(((r>>2)&0x7FFFFu)<<5)|(r&0x1Fu);b[i*4]=(uint8_t)insn;b[i*4+1]=(uint8_t)(insn>>8);b[i*4+2]=(uint8_t)(insn>>16);b[i*4+3]=(uint8_t)(insn>>24);}
        ok(arm64_reversible(b,n),"arm64 reversible all-ADRP"); free(b); }
    for(int t=0;t<300;t++){
        size_t n=8+xr()%16384; uint8_t*b=malloc(n);
        for(size_t i=0;i<n;i++){b[i]=(uint8_t)xr();}
        /* sprinkle BL/ADRP opcodes to exercise the filter */
        for(size_t i=0;i+4<=n;i+=4){ if(xr()%100<40){uint32_t insn=(xr()&1)?0x94000000u:0x90000000u;insn|=(xr()&0x00FFFFFFu);b[i]=(uint8_t)insn;b[i+1]=(uint8_t)(insn>>8);b[i+2]=(uint8_t)(insn>>16);b[i+3]=(uint8_t)(insn>>24);} }
        ok(full_roundtrip(b,n,1),"arm64 full roundtrip"); free(b);
    }

    /* ---- empty + tiny, both filters ---- */
    { uint8_t z[1]={0};
      ok(full_roundtrip(z,0,0),"x86 empty"); ok(full_roundtrip(z,1,0),"x86 1-byte");
      ok(full_roundtrip(z,0,1),"arm64 empty"); ok(full_roundtrip(z,1,1),"arm64 1-byte"); }

    /* ---- corrupt-input safety (memory safety verified under ASan/UBSan) ---- */
    for(int t=0;t<400;t++){
        size_t n=32+xr()%8192; uint8_t*b=malloc(n);
        for(size_t i=0;i<n;i++){b[i]=(uint8_t)xr();}
        ok(corrupt_decode(b,n,0),"x86 corrupt-decode safe");
        ok(corrupt_decode(b,n,1),"arm64 corrupt-decode safe"); free(b);
    }

    printf("Results: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail?1:0;
}
