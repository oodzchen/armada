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

/*
 * Session API used by the read-only U-Boot block device. open: preflight,
 * Hibern8 exit, private queue, queries and (OPD_UFS_WAKE) wake. read: whole
 * 4 KiB blocks through READ(10), at most 14 per command. close: restore the
 * power mode (verified), ABL's queue and Hibern8. Any transport fault marks
 * the session broken: later calls fail and close() returns -1, so the
 * caller must refuse the Android return.
 * open: 0 = open, 1 = skipped with no DMA, -1 = uncertain.
 * read/capacity: 0 = done, 1 = the unit refused, -1 = uncertain.
 */
struct opd_ufs_session {
	/* open() copies the caller's io: the observer's lives on the board-init
	 * stack, which main_loop reuses before the block device reads. */
	struct opd_ufs_io io_copy;
	const struct opd_ufs_io *io; /* = &io_copy */
	volatile uint8_t *dma;
	struct opd_ufs_read_result *res;
	uint32_t saved[3];
	uint8_t condition, open, broken;
#ifdef OPD2413_BOOT_CHOICE_WRITER
	/* Set only by the boot-choice adapter after GPT/image/Blob validation. */
	uint8_t choice_ready, choice_lun;
	uint32_t choice_lba[2];
#endif
};
#ifdef OPD2413_BOOT_CHOICE_WRITER
/* One FUA block, restricted to the two authorized journal pages. */
int opd2413_ufs_write_choice(struct opd_ufs_session *s, uint8_t lun,
                            uint32_t lba, const void *page);
#endif
int opd2413_ufs_open(struct opd_ufs_session *s, const struct opd_ufs_io *io, volatile void *dma,
		     unsigned flags, struct opd_ufs_read_result *res);
int opd2413_ufs_luns(struct opd_ufs_session *s, uint8_t luns[OPD_READ_MAX_LUNS], unsigned *count);
int opd2413_ufs_capacity(struct opd_ufs_session *s, struct opd_ufs_read_lun *l);
int opd2413_ufs_read(struct opd_ufs_session *s, uint8_t lun, uint64_t lba, uint32_t blocks, void *buffer);
int opd2413_ufs_close(struct opd_ufs_session *s);

/* IEEE 802.3 CRC-32 as used by GPT and zlib. */
uint32_t opd2413_crc32(uint32_t crc, const volatile void *buf, size_t len);
/* Query whitelist: STANDARD READ of a descriptor, attribute or flag only. */
int opd2413_ufs_query_allowed(uint8_t function, uint8_t opcode, uint16_t length);
/* Whitelist used before submission: CDB, its length and the transfer size. */
int opd2413_ufs_read_allowed(const uint8_t *cdb, unsigned cdb_len, uint32_t xfer);
#endif
