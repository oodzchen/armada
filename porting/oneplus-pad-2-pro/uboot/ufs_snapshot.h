/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef OPD2413_UFS_SNAPSHOT_H
#define OPD2413_UFS_SNAPSHOT_H
#include <stdint.h>

/* Only these pages are mapped, Device-nGnRnE, read-only and execute-never. */
#define OPD_GCC_UFS_BASE 0x177000ULL
#define OPD_GCC_UFS_SIZE 0x1000ULL
#define OPD_GCC_MEM_BASE 0x19e000ULL
#define OPD_GCC_MEM_SIZE 0x1000ULL
#define OPD_UFS_BASE 0x1d84000ULL
#define OPD_UFS_SIZE 0x3000ULL
#define OPD_PHY_BASE 0x1d80000ULL
#define OPD_PHY_SIZE 0x2000ULL

/* No write callback: this is a snapshot of ABL state, not UFS initialization. */
typedef uint32_t (*opd_read32)(uint64_t addr, void *ctx);
int opd2413_ufs_resources(const void *fdt);
void opd2413_ufs_snapshot(opd_read32 read32, void *ctx);
#endif
