/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef OPD2413_UFS_BLK_H
#define OPD2413_UFS_BLK_H
#include "opd2413_ufs_nop.h"
/* 0 = devices bound; 1 = skipped (state unchanged or restored); -1 = uncertain. */
int opd2413_ufs_blk_start(const struct opd_ufs_io *io);
/* 0 = power mode, queue and Hibern8 restored; -1 = refuse the Android return. */
int opd2413_ufs_blk_stop(void);
/* QEMU-only RAM-backed LUN (CONFIG_OPD2413_UFS_BLK_RAMDISK); never in device builds. */
int opd2413_ufs_blk_start_ramdisk(void);
#endif
