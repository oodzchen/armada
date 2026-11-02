/* SPDX-License-Identifier: GPL-2.0+ */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ramconsole_format.h"
int main(int argc,char **argv)
{
    assert(argc==2); uint32_t *mem=calloc(1,0x40000+64); assert(mem);
    mem[0]=OPD_LOG_MAGIC;mem[1]=mem[2]=4;memcpy(mem+3,"shim",4);
    opd_log_init(mem); assert(mem[2]==OPD_LOG_LAST+12);
    assert(opd_log_putc(mem,'\n'));assert(opd_log_putc(mem,'U'));
    opd_log_init(mem); /* driver re-probe must preserve all content */
    assert(((uint32_t *)(mem+3))[0]==6);
    for(unsigned n=0;n<OPD_LOG_COPIES;++n) {
        unsigned char *b=(unsigned char *)(mem+3)+n*OPD_LOG_STRIDE;
        assert(!memcmp(b+8,"shim\nU",6));assert(((uint32_t *)b)[0]==6);
    }
    FILE *f=fopen(argv[1],"wb");assert(f);assert(fwrite(mem,1,0x40000,f)==0x40000);fclose(f);
    for(unsigned i=6;i<OPD_LOG_CAPACITY;++i)assert(opd_log_putc(mem,'x'));
    assert(!opd_log_putc(mem,'!'));assert(mem[2]<0x40000-12);
    for(unsigned i=0x40000/4;i<(0x40000+64)/4;++i)assert(!mem[i]);
    free(mem);puts("PASS: preserve shim log, five replicas, re-probe, truncation and bounds");
}
