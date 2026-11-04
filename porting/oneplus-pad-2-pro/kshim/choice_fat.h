/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef OPD_CHOICE_FAT_H
#define OPD_CHOICE_FAT_H
#include <stdint.h>
#include <stddef.h>
/* Read-only FAT16 locator for the board's existing ESP. No FAT writes.
 * Returns two aligned 4 KiB pages inside the contiguous 16 KiB KSHIM.CHO.
 */
typedef int (*choice_read_bytes)(void *, uint64_t, void *, size_t);
int choice_fat_locate(choice_read_bytes read, void *ctx, uint64_t bytes,
                      uint64_t *page_offset);
#endif
