/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "choice_fat.h"
#include "choice_journal.h"
#define BYTES (16U * 1024 * 1024)
static uint8_t *disk;
static int rd(void *ctx, uint64_t off, void *out, size_t n)
{
    (void)ctx;
    if (off > BYTES || n > BYTES - off) return -1;
    memcpy(out, disk + off, n); return 0;
}
static void put16(uint8_t *p, unsigned n) { p[0] = n; p[1] = n >> 8; }
int main(void)
{
    disk = calloc(1, BYTES); assert(disk);
    put16(disk + 11, 512); disk[13] = 4; put16(disk + 14, 1);
    disk[16] = 2; put16(disk + 17, 512); put16(disk + 19, 32768);
    put16(disk + 22, 32); disk[510] = 0x55; disk[511] = 0xaa;
    unsigned root = (1 + 64) * 512, data = root + 512 * 32;
    memcpy(disk + root, "KSHIM   CHO", 11); disk[root + 11] = 0x20;
    put16(disk + root + 26, 2); choice_put32(disk + root + 28, 16384);
    for (unsigned i = 2; i <= 9; ++i) {
        put16(disk + 512 + 2 * i, i == 9 ? 0xffff : i + 1);
        put16(disk + 33 * 512 + 2 * i, i == 9 ? 0xffff : i + 1);
    }
    uint64_t off;
    assert(!choice_fat_locate(rd, NULL, BYTES, &off));
    assert(!(off & 4095) && off >= data && off + CHOICE_BYTES <= data + 16384);
    disk[root] = 0xe5; assert(choice_fat_locate(rd, NULL, BYTES, &off)); disk[root] = 'K';
    choice_put32(disk + root + 28, 8192); assert(choice_fat_locate(rd, NULL, BYTES, &off));
    choice_put32(disk + root + 28, 16384);
    disk[512 + 4] ^= 1; assert(choice_fat_locate(rd, NULL, BYTES, &off)); disk[512 + 4] ^= 1;
    put16(disk + 512 + 4, 12); put16(disk + 33 * 512 + 4, 12);
    assert(choice_fat_locate(rd, NULL, BYTES, &off)); /* fragmented chain */
    put16(disk + 512 + 4, 3); put16(disk + 33 * 512 + 4, 3);
    memcpy(disk + root + 32, disk + root, 32);
    assert(choice_fat_locate(rd, NULL, BYTES, &off)); /* ambiguous duplicate */
    memset(disk + root + 32, 0, 32);
    disk[512 + 100] ^= 1; /* mirror mismatch outside the journal chain */
    assert(choice_fat_locate(rd, NULL, BYTES, &off)); disk[512 + 100] ^= 1;
    put16(disk+512+100,2);put16(disk+33*512+100,2); /* outside FAT edge */
    assert(choice_fat_locate(rd,NULL,BYTES,&off));
    put16(disk+512+100,0);put16(disk+33*512+100,0);
    memcpy(disk+root+32,"EFI        ",11);disk[root+32+11]=0x10;put16(disk+root+32+26,20);
    put16(disk+512+40,0xffff);put16(disk+33*512+40,0xffff);
    assert(!choice_fat_locate(rd,NULL,BYTES,&off));
    uint8_t *child=disk+data+18*2048;
    memcpy(child,"BAD     EFI",11);child[11]=0x20;put16(child+26,2);choice_put32(child+28,4096);
    assert(choice_fat_locate(rd,NULL,BYTES,&off)); /* nested direct cross-link */
    memset(child,0,32);memset(disk+root+32,0,32);
    assert(choice_fat_locate(rd, NULL, BYTES / 2, &off));
    puts("choice FAT16: aligned bounded extent, absent/short/duplicate/fragmented file and FAT mismatch passed");
    free(disk);
}
