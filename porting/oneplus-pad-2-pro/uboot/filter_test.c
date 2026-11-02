/* SPDX-License-Identifier: GPL-2.0+ */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libfdt.h>
#include "layout.h"
int opd2413_filter_fdt(const void *, void *, unsigned);
static uint64_t cell64(const fdt32_t *p) { return ((uint64_t)fdt32_to_cpu(p[0])<<32)|fdt32_to_cpu(p[1]); }
static size_t load_ranges(void *f, struct opd_range *r)
{
    int len, n=fdt_path_offset(f,"/memory"); size_t count=0;
    const fdt32_t *p=fdt_getprop(f,n,"reg",&len); assert(p && !(len%16));
    for(int i=0;i<len/4;i+=4) { struct opd_range v={cell64(p+i),cell64(p+i+2)}; if(v.size) { assert(opd_range_valid(v)); r[count++]=v; } }
    n=fdt_path_offset(f,"/reserved-memory"); int c;
    fdt_for_each_subnode(c,f,n) {
        p=fdt_getprop(f,c,"reg",&len); assert(p && !(len%16));
        for(int i=0;i<len/4;i+=4) assert(!opd_carve(r,&count,(struct opd_range){cell64(p+i),cell64(p+i+2)}));
    }
    return count;
}
int main(int argc,char **argv)
{
    assert(argc==2); FILE *fp=fopen(argv[1],"rb"); assert(fp);
    fseek(fp,0,SEEK_END); long n=ftell(fp); rewind(fp);
    void *src=calloc(1,n+65536), *original=malloc(n), *out=malloc(65536); assert(src&&original&&out);
    assert(fread(src,1,n,fp)==(size_t)n); fclose(fp); memcpy(original,src,n);
    assert(!opd2413_filter_fdt(src,out,65536));
    assert(!memcmp(original,src,n)); assert(!fdt_check_full(out,fdt_totalsize(out)));
    int a=fdt_path_offset(src,"/memory"), b=fdt_path_offset(out,"/memory"), al,bl;
    const void *ar=fdt_getprop(src,a,"reg",&al), *br=fdt_getprop(out,b,"reg",&bl);
    assert(ar&&br&&al==bl&&!memcmp(ar,br,al));
    assert(fdt_path_offset(out,"/soc")<0);
    b=fdt_path_offset(out,"/chosen"); assert(b>=0);
    assert(!fdt_getprop(out,b,"bootargs",NULL));
    assert(!fdt_getprop(out,b,"linux,initrd-start",NULL));
    assert(fdt_path_offset(out,"/chosen/framebuffer@fc800000")>=0);
    struct opd_range ram[OPD_MAX_RANGES];size_t count=load_ranges(out,ram);
    assert(opd_contains(ram,count,0xb9000000,0x1000000));
    assert(opd_contains(ram,count,0x880000000ULL,0x40000));
    assert(!opd_contains(ram,count,0xf3800000,0x1000)); /* secure VM */
    assert(!opd_contains(ram,count,0xfc800000,0x1000)); /* separate framebuffer map */
    struct opd_range high[OPD_MAX_RANGES]={{0xa00000000ULL,0x10000}};size_t hn=1;
    assert(!opd_carve(high,&hn,(struct opd_range){0xa00004000ULL,0x2000}));
    assert(hn==2 && !opd_contains(high,hn,0xa00004000ULL,0x1000));
    assert(opd_contains(high,hn,0xa00000000ULL,0x4000));
    assert(opd_contains(high,hn,0xa00006000ULL,0xa000));
    assert(opd_carve(high,&hn,(struct opd_range){(uint64_t)-4096,8192}));
    assert(opd_carve(high,&hn,(struct opd_range){1,4096}));
    assert(!opd_carve(high,&hn,(struct opd_range){0,0}));
    assert(opd2413_filter_fdt(src,out,64));
    assert(!fdt_open_into(src,src,n+65536));
    assert(!fdt_setprop_string(src,0,"model","wrong-device"));
    assert(opd2413_filter_fdt(src,out,65536));
    assert(!fdt_open_into(original,src,n+65536));
    a=fdt_path_offset(src,"/memory");assert(!fdt_delprop(src,a,"reg"));
    assert(opd2413_filter_fdt(src,out,65536));
    printf("PASS: live-memory copy, source unchanged, no peripheral nodes, secure carveouts, high-address split, overflow and malformed input (%zu usable ranges)\n",count);
    free(src);free(original);free(out);return 0;
}
