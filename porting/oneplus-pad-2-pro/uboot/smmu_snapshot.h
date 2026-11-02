/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef OPD2413_SMMU_SNAPSHOT_H
#define OPD2413_SMMU_SNAPSHOT_H
#include "opd2413_ufs_snapshot.h"
#define OPD_SMMU_BASE 0x15000000ULL
#define OPD_SMMU_SIZE 0x100000ULL
int opd2413_smmu_resources(const void *fdt, uint32_t *sid);
void opd2413_smmu_snapshot(opd_read32 read32, void *ctx, uint32_t sid);
#endif
