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
	/* Read-only query results; *_ok is set only for a successful response. */
	uint8_t power_mode_ok, power_mode, boot_lun_ok, boot_lun, device_init_ok, device_init;
	uint8_t descriptor_ok, number_lu, init_power_mode;
	/* OPD_UFS_WAKE only: woke from power_mode, restored and re-read it. */
	uint8_t woke, restored_power_mode;
	uint16_t spec_version, manufacturer_id;
	unsigned luns;
	struct opd_ufs_read_lun lun[OPD_READ_MAX_LUNS];
};

/*
 * Read-only LUN, capacity and GPT survey on ABL's pre-initialized UFS link.
 * First the STANDARD READ queries bCurrentPowerMode, bBootLunEn, fDeviceInit
 * and the device descriptor; then only TEST UNIT READY, REPORT LUNS,
 * READ CAPACITY(10/16) and READ(10) of the primary and backup GPT blocks.
 * Every query and CDB is checked against these lists before it reaches the
 * controller. No query write, power-state change, media write, reset, clock,
 * PHY or SMMU change.
 * 0 = survey finished, original queue and Hibern8 restored;
 * 1 = skipped before any DMA; -1 = controller state uncertain, reset required.
 * `out` may be NULL.
 */
int opd2413_ufs_read_gpt(const struct opd_ufs_io *io, volatile void *dma,
			 struct opd_ufs_read_result *out);

/*
 * The same survey with OPD_UFS_WAKE: when bCurrentPowerMode reads UFS-Sleep
 * (0x22) or UFS-PowerDown (0x33), send START STOP UNIT to the UFS Device
 * W-LUN (0xd0) with POWER CONDITION Active first, then after the survey
 * restore that exact power condition and require bCurrentPowerMode to read
 * the original value again before the queue and Hibern8 are restored.
 * A power-state change, never a media write.
 */
#define OPD_UFS_WAKE 1U
int opd2413_ufs_survey(const struct opd_ufs_io *io, volatile void *dma,
		       struct opd_ufs_read_result *out, unsigned flags);
/* START STOP UNIT whitelist: Device W-LUN, POWER CONDITION 1/2/3, IMMED=0. */
int opd2413_ufs_ssu_allowed(uint8_t lun, const uint8_t *cdb, unsigned cdb_len, uint32_t xfer);

/* IEEE 802.3 CRC-32 as used by GPT and zlib. */
uint32_t opd2413_crc32(uint32_t crc, const volatile void *buf, size_t len);
/* Query whitelist: STANDARD READ of a descriptor, attribute or flag only. */
int opd2413_ufs_query_allowed(uint8_t function, uint8_t opcode, uint16_t length);
/* Whitelist used before submission: CDB, its length and the transfer size. */
int opd2413_ufs_read_allowed(const uint8_t *cdb, unsigned cdb_len, uint32_t xfer);
#endif
