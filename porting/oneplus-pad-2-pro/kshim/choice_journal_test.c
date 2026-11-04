/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdio.h>
#include "choice_journal.h"
int main(void)
{
    uint8_t pages[CHOICE_BYTES] = {0}, saved[CHOICE_PAGE];
    uint32_t entries[] = {123, 456, 789, 101112};
    struct choice_record r;
    assert(!choice_latest(pages, 42, entries, 4, &r));
    choice_encode(pages, 42, entries[2], 1, 2);
    assert(choice_latest(pages, 42, entries, 4, &r) == 1 && r.entry == 2 && r.slot == 0);
    choice_encode(pages + CHOICE_PAGE, 42, entries[3], 2, 3);
    assert(choice_latest(pages, 42, entries, 4, &r) == 1 && r.entry == 3 && r.slot == 1);
    memcpy(saved, pages + CHOICE_PAGE, CHOICE_PAGE);
    /* Every single-byte corruption must retain the previous generation. */
    for (unsigned i = 0; i < CHOICE_PAGE; ++i) {
        pages[CHOICE_PAGE + i] ^= 1;
        assert(choice_latest(pages, 42, entries, 4, &r) == 1 && r.entry == 2 && r.slot == 0);
        pages[CHOICE_PAGE + i] ^= 1;
    }
    /* Torn writes, including CRC partly written, retain the old page. */
    for (unsigned n = 0; n < CHOICE_PAGE; ++n) {
        memset(pages + CHOICE_PAGE, 0, CHOICE_PAGE);
        memcpy(pages + CHOICE_PAGE, saved, n);
        assert(choice_latest(pages, 42, entries, 4, &r) == 1 && r.entry == 2);
    }
    memcpy(pages + CHOICE_PAGE, saved, CHOICE_PAGE);
    assert(!choice_latest(pages, 43, entries, 4, &r)); /* changed menu/image */
    assert(!choice_latest(pages, 42, entries, 2, &r)); /* removed entries */
    entries[3] ^= 1;
    assert(choice_latest(pages, 42, entries, 4, &r) == 1 && r.entry == 2);
    entries[3] ^= 1;
    choice_encode(pages + CHOICE_PAGE, 42, entries[3], 1, 3);
    assert(choice_latest(pages, 42, entries, 4, &r) == -1);
    uint8_t marker[CHOICE_PAGE] = {0}, guid[16] = {1};
    memcpy(marker,"KSHPROV1",8);choice_put32(marker+8,1);choice_put32(marker+12,16384);
    memcpy(marker+16,guid,16);choice_put64(marker+32,123*4096);choice_put32(marker+40,4096);
    choice_put32(marker+44,2);marker[48]=1;
    choice_put32(marker+4092,choice_crc(marker,4092));
    assert(choice_provision_valid(marker,guid,123*4096));
    assert(!choice_provision_valid(marker,guid,124*4096));
    for(unsigned i=0;i<CHOICE_PAGE;++i){marker[i]^=1;assert(!choice_provision_valid(marker,guid,123*4096));marker[i]^=1;}
    puts("choice journal: generations, all byte corruptions, all torn prefixes and menu identity passed");
}
