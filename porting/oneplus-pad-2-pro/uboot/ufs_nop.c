// SPDX-License-Identifier: GPL-2.0+
/* One NOP only, using ABL's legacy UFS link after DME_GET / Hibern8 exit,
 * then restoring its queue and Hibern8 state. No SCSI, query writes, resets,
 * clock configuration, PHY configuration or SMMU changes.
 * UTRD/UPIU layout follows the pinned U-Boot drivers/ufs/{ufshci.h,ufs.h}.
 */
#include <stdio.h>
#include "opd2413_ufs_nop.h"
#include "opd2413_smmu_snapshot.h"
#include "opd2413_ufs_link.h"
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "OPD2413 UTRD layout requires a little-endian build"
#endif

#define IS 0x20
#define IE 0x24
#define HCS 0x30
#define HCE 0x34
#define UTRLBA 0x50
#define UTRLBA_HI 0x54
#define DBR 0x58
#define RSR 0x60
#define TASK_DBR 0x78
#define UFS_ERRORS 0x000f0804U

static uint32_t host(const struct opd_ufs_io *io, unsigned off)
{
	return io->read32(OPD_UFS_BASE + off, io->ctx);
}
static uint32_t smmu(const struct opd_ufs_io *io, unsigned off)
{
	return io->read32(OPD_SMMU_BASE + off, io->ctx);
}
static void write_host(const struct opd_ufs_io *io, unsigned off, uint32_t value)
{
	/* Hardware adapter also enforces this whitelist. */
	io->write32(OPD_UFS_BASE + off, value, io->ctx);
	io->sync_io(io->ctx);
}
static int wait_host(const struct opd_ufs_io *io, unsigned off, uint32_t value)
{
	for (unsigned i = 0; i < 10000; ++i) {
		if (host(io, off) == value) return 0;
		io->delay_us(50, io->ctx);
	}
	return -1;
}

int opd2413_ufs_preflight(const struct opd_ufs_io *io, const char *tag, uint32_t saved[3])
{
	uint32_t ahb = io->read32(OPD_GCC_UFS_BASE + 0x28, io->ctx);
	/* Intentionally restricted to the exact handoff observed on this unit.
	 * An unexpected state causes no writes, even if it might also be usable.
	 */
	if (io->read32(OPD_GCC_UFS_BASE, io->ctx) & 1 ||
	    !(io->read32(OPD_GCC_UFS_BASE + 4, io->ctx) & 0x80000000U) ||
	    !(io->read32(OPD_GCC_MEM_BASE, io->ctx) & 0x80000000U) ||
	    ((ahb & 0x80000000U) && (ahb & 3) != 3) ||
	    host(io, 0x08) != 0x400 || host(io, 0xe4) != 0x60010000 ||
	    !(host(io, 0) & 0x01000000U) || host(io, HCE) != 1 ||
	    (host(io, HCS) & 0xf) != 0xf || host(io, IE) || host(io, 0x18) ||
	    host(io, DBR) || host(io, TASK_DBR) || host(io, 0x300) ||
	    host(io, IS) & (UFS_ERRORS | 1) ||
	    smmu(io, 0x20) != 0x4c017e84 || smmu(io, 0x24) != 0x60000053 ||
	    smmu(io, 0x800) != 0x80000060 || smmu(io, 0xc00) ||
	    smmu(io, 0x1000) != 0x0001f000 || smmu(io, 0x1800) != 1 ||
	    smmu(io, 0x80000) != 0x0a9f00e0 ||
	    smmu(io, 0x80020) || smmu(io, 0x80024) ||
	    smmu(io, 0x80028) || smmu(io, 0x8002c) || smmu(io, 0x80030)) {
		printf("%s skipped: unexpected active-link or identity-DMA state\n", tag);
		return 1;
	}
	saved[0] = host(io, UTRLBA); saved[1] = host(io, UTRLBA_HI); saved[2] = host(io, RSR);
	if (saved[2] != 1 || !saved[0] || (saved[0] & 0x3ff)) {
		printf("%s skipped: unexpected original transfer queue\n", tag);
		return 1;
	}
	uint32_t states[3] = {0xffffffffU, 0xffffffffU, 0xffffffffU};
	int ret = opd2413_ufs_link_check(io, states);
	if (ret) return ret;
	if (states[0] != 1 || states[1] != 1 || states[2] != 0x11) {
		printf("%s skipped: unexpected initial link state\n", tag); return 1;
	}
	return 0;
}

int opd2413_ufs_nop(const struct opd_ufs_io *io, volatile void *dma)
{
	volatile uint32_t *utrd = dma;
	volatile unsigned char *ucd = (volatile unsigned char *)dma + 0x1000;
	uint32_t saved[3], old_base, old_high, old_run;
	printf("OPD2413_UFS_NOP_BEGIN\n");
	int ret = opd2413_ufs_preflight(io, "UFS_NOP", saved);
	if (ret) return ret;
	old_base = saved[0]; old_high = saved[1]; old_run = saved[2];
	ret = opd2413_ufs_hibern8(io, 0);
	if (ret) return ret;
	/* The dedicated low RAM window is mapped Normal-NC, with no WB alias.
	 * Full barriers order RAM descriptors, MMIO and DMA completion.
	 */
	volatile unsigned char *bytes = dma;
	for (unsigned i = 0; i < OPD_DMA_SIZE; ++i) bytes[i] = 0;
	utrd[0] = 0x11000000; /* command type 1, interrupt request, no data */
	utrd[2] = 0xf; /* invalid OCS until hardware completes */
	utrd[4] = (uint32_t)(OPD_DMA_BASE + 0x1000);
	utrd[5] = (uint32_t)((OPD_DMA_BASE + 0x1000) >> 32);
	utrd[6] = 0x00800080; /* response at +512, length 512, units DWORDs */
	utrd[7] = 0x01000000; /* PRDT at +1024, zero entries */
	/* NOP_OUT header is all zero (transaction=0, LUN=0, task tag=0). */
	for (unsigned i = 0x200; i < 0x400; ++i) ucd[i] = 0xa5;
	io->sync_io(io->ctx);
	printf("UFS_NOP DMA=%llx original_queue=%08x%08x\n",
	       (unsigned long long)OPD_DMA_BASE, old_high, old_base);
	write_host(io, RSR, 0);
	if (wait_host(io, RSR, 0) || host(io, DBR)) goto failed;
	write_host(io, UTRLBA_HI, 0);
	write_host(io, UTRLBA, (uint32_t)OPD_DMA_BASE);
	if (host(io, UTRLBA) != OPD_DMA_BASE || host(io, UTRLBA_HI)) goto failed;
	write_host(io, RSR, 1);
	if (wait_host(io, RSR, 1)) goto failed;
	write_host(io, DBR, 1);
	if (wait_host(io, DBR, 0)) goto failed;
	io->sync_io(io->ctx);
	printf("UFS_NOP OCS=%08x response=%02x tag=%02x IS=%08x\n",
	       utrd[2], ucd[0x200], ucd[0x203], host(io, IS));
	if ((utrd[2] & 0xf) || ucd[0x200] != 0x20 || ucd[0x203] ||
	    host(io, IS) & UFS_ERRORS) goto failed;
	/* Completion proves this NOP's DMA finished. Stop the empty list before
	 * restoring its base. Keep ABL's original UIC completion bit untouched.
	 */
	write_host(io, RSR, 0);
	if (wait_host(io, RSR, 0) || host(io, DBR)) goto failed;
	write_host(io, UTRLBA_HI, old_high);
	write_host(io, UTRLBA, old_base);
	write_host(io, IS, 1);
	write_host(io, RSR, old_run);
	if (host(io, UTRLBA) != old_base || host(io, UTRLBA_HI) != old_high ||
	    host(io, RSR) != old_run || host(io, DBR) || host(io, IE) ||
	    host(io, HCE) != 1 || host(io, IS) & (UFS_ERRORS | 1)) goto failed;
	if (opd2413_ufs_hibern8(io, 1)) goto failed;
	printf("OPD2413_UFS_NOP_PASS original_queue_and_hibern8_restored\n");
	return 0;
failed:
	/* Never return to Linux after an uncertain transfer. No base changes
	 * while DMA might still be live; leave its private buffer untouched.
	 */
	printf("OPD2413_UFS_NOP_FAIL OCS=%08x DBR=%08x IS=%08x; reset required\n",
	       utrd[2], host(io, DBR), host(io, IS));
	printf("UFS_NOP SMMU GFSR=%08x GFSYNR1=%08x CB0_FSR=%08x FAR=%08x%08x FSYNR0=%08x\n",
	       smmu(io, 0x48), smmu(io, 0x54), smmu(io, 0x80058),
	       smmu(io, 0x80064), smmu(io, 0x80060), smmu(io, 0x80068));
	write_host(io, RSR, 0);
	return -1;
}
