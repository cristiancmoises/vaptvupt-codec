/* SPDX-License-Identifier: GPL-3.0-or-later
 * BCJ x86 filter: standalone reversibility + full compress/decompress roundtrip. */
#include "vaptvupt.h"
#include "vv_bcj.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint64_t st=0x9E3779B97F4A7C15ULL;
static uint32_t xr(void){st^=st<<13;st^=st>>7;st^=st<<17;return(uint32_t)(st>>32);}
static int g_pass=0,g_fail=0;
static void ok(int c,const char*m){if(c)g_pass++;else{g_fail++;printf("  FAIL: %s\n",m);}}

static int bcj_reversible(const uint8_t*orig,size_t n){
    uint8_t*a=malloc(n?n:1),*b=malloc(n?n:1);
    memcpy(a,orig,n); vv_bcj_x86(a,n,0,1);
    memcpy(b,a,n);     vv_bcj_x86(b,n,0,0);
    int r=(memcmp(b,orig,n)==0); free(a);free(b); return r;
}
static int full_roundtrip(const uint8_t*orig,size_t n){
    uint8_t*c=malloc(vv_compress_bound(n)+64),*o=malloc(n+64);
    vv_options_t opt; vv_default_options(&opt); opt.mode=VV_MODE_BALANCED; opt.filter_x86=1;
    int64_t cl=vv_compress(orig,n,c,vv_compress_bound(n)+64,&opt);
    int r=0;
    if(cl>0){ int64_t dl=vv_decompress(c,(size_t)cl,o,n+64); r=(dl==(int64_t)n && memcmp(o,orig,n)==0); }
    free(c);free(o); return r;
}
int main(void){
    printf("BCJ x86 filter tests:\n");
    /* reversibility on random */
    for(int t=0;t<2000;t++){
        size_t n=xr()%8192; uint8_t*b=malloc(n?n:1);
        for(size_t i=0;i<n;i++){b[i]=(uint8_t)xr();}
        ok(bcj_reversible(b,n),"reversible random"); free(b);
    }
    /* reversibility adversarial */
    for(size_t n=0;n<=20;n++){ uint8_t*b=malloc(n?n:1);
        memset(b,0xE8,n); ok(bcj_reversible(b,n),"reversible all-E8");
        memset(b,0xE9,n); ok(bcj_reversible(b,n),"reversible all-E9"); free(b); }
    /* full compress/decompress roundtrip with filter on, including non-x86 data */
    for(int t=0;t<300;t++){
        size_t n=8+xr()%16384; uint8_t*b=malloc(n);
        for(size_t i=0;i<n;i++){b[i]=(uint8_t)((xr()%100<60)?0xE8:xr());} /* E8-rich */
        ok(full_roundtrip(b,n),"full roundtrip filter=1"); free(b);
    }
    /* empty + tiny */
    { uint8_t z[1]={0}; ok(full_roundtrip(z,0),"empty filter=1"); ok(full_roundtrip(z,1),"1-byte filter=1"); }
    printf("Results: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail?1:0;
}
