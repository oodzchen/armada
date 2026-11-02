/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef OPD2413_UFS_READ_H
#define OPD2413_UFS_READ_H
#include <stddef.h>
#include "opd2413_ufs_nop.h"

#define OPD_READ_MAX_LUNS 8
#define OPD_READ_BLOCK 4096U
#define OPD_READ_PRIMARY_BLOCKS 6U /* protective MBR, header, 128 x 128-byte entries */
#define OPD_READ_BACKUP_BLOCKS 5U  /* backup entries and header at the last LBA */

struct opd_ufs_read_lun {
	uint8_t lun;
	uint8_t capacity_ok, primary_ok, backup_ok;
	uint32_t block;
	uint64_t last_lba;
	uint32_t partitions;
	uint32_t primary_crc[OPD_READ_PRIMARY_BLOCKS];
	uint32_t backup_crc[OPD_READ_BACKUP_BLOCKS];
};

struct opd_ufs_read_result {
	unsigned luns;
	struct opd_ufs_read_lun lun[OPD_READ_MAX_LUNS];
};

/*
 * Read-only LUN, capacity and GPT survey on ABL's pre-initialized UFS link.
 * Only TEST UNIT READY, REPORT LUNS, READ CAPACITY(10/16) and READ(10) of the
 * primary and backup GPT blocks are ever submitted; every CDB is checked
 * against that list before it reaches the controller. No query, write,
 * reset, clock, PHY or SMMU change.
 * 0 = survey finished, original queue and Hibern8 restored;
 * 1 = skipped before any DMA; -1 = controller state uncertain, reset required.
 * `out` may be NULL.
 */
int opd2413_ufs_read_gpt(const struct opd_ufs_io *io, volatile void *dma,
			 struct opd_ufs_read_result *out);

/* IEEE 802.3 CRC-32 as used by GPT and zlib. */
uint32_t opd2413_crc32(uint32_t crc, const volatile void *buf, size_t len);
/* Whitelist used before submission: CDB, its length and the transfer size. */
int opd2413_ufs_read_allowed(const uint8_t *cdb, unsigned cdb_len, uint32_t xfer);
#endif
