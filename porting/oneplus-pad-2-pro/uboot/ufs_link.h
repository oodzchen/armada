/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef OPD2413_UFS_LINK_H
#define OPD2413_UFS_LINK_H
#include "opd2413_ufs_nop.h"
/* DME_GET diagnostics and Hibern8 handshakes; no UTP queue or DMA changes. */
int opd2413_ufs_link_check(const struct opd_ufs_io *io, uint32_t states[3]);
int opd2413_ufs_hibern8(const struct opd_ufs_io *io, int enter);
#endif
