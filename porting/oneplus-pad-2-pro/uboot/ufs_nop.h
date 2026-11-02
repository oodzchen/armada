/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef OPD2413_UFS_NOP_H
#define OPD2413_UFS_NOP_H
#include "opd2413_ufs_snapshot.h"
#define OPD_DMA_BASE 0xb9800000ULL
#define OPD_DMA_SIZE 0x10000ULL

struct opd_ufs_io {
	opd_read32 read32;
	void (*write32)(uint64_t addr, uint32_t value, void *ctx);
	void (*sync_io)(void *ctx);
	void (*delay_us)(unsigned us, void *ctx);
	void *ctx;
};
/* 0=NOP succeeded, original queue and Hibern8 restored; 1=skipped without DMA;
 * -1=hardware was modified and cannot safely return directly to Android.
 */
int opd2413_ufs_nop(const struct opd_ufs_io *io, volatile void *dma);
/* Shared exact-state guard and DME_GET link check for DMA experiments.
 * 0=ready (saved[] = UTRLBA, UTRLBA_HI, UTRLRSR), 1=skipped with no writes
 * beyond link-check diagnostics, -1=uncertain.
 */
int opd2413_ufs_preflight(const struct opd_ufs_io *io, const char *tag, uint32_t saved[3]);
#endif
