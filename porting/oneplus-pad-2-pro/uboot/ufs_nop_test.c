// SPDX-License-Identifier: GPL-2.0+
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "opd2413_ufs_nop.h"
#include "opd2413_smmu_snapshot.h"

struct mock {
	uint32_t host[1024], smmu[OPD_SMMU_SIZE / 4];
	unsigned char *dma;
	unsigned writes, submitted, pending, mode, barriers, hibernated, uic_commands;
};
static void finish(struct mock *s)
{
	uint32_t *utrd = (uint32_t *)s->dma;
	utrd[2] = s->mode == 2 ? 1 : 0;
	memset(s->dma + 0x1200, 0, 512);
	s->dma[0x1200] = s->mode == 3 ? 0x3f : 0x20;
	if (s->mode == 5) s->dma[0x1203] = 1;
	s->host[0x58/4] = 0;
	s->host[0x20/4] |= 1;
}
static uint32_t read32(uint64_t addr, void *ctx)
{
	struct mock *s = ctx;
	if (addr == OPD_GCC_UFS_BASE) return 0;
	if (addr == OPD_GCC_UFS_BASE + 4 || addr == OPD_GCC_MEM_BASE) return 0x80000000U;
	if (addr == OPD_GCC_UFS_BASE + 0x28) return 0x88000003;
	if (addr >= OPD_SMMU_BASE && addr < OPD_SMMU_BASE + OPD_SMMU_SIZE)
		return s->smmu[(addr - OPD_SMMU_BASE)/4];
	assert(addr >= OPD_UFS_BASE && addr < OPD_UFS_BASE + 4096 && !(addr & 3));
	if (addr == OPD_UFS_BASE + 0x58 && s->pending && s->mode != 1)
		if (!--s->pending) finish(s);
	return s->host[(addr - OPD_UFS_BASE)/4];
}
static void write32(uint64_t addr, uint32_t value, void *ctx)
{
	struct mock *s = ctx;
	assert(addr >= OPD_UFS_BASE && addr < OPD_UFS_BASE + 4096);
	unsigned off = addr - OPD_UFS_BASE;
	assert(off == 0x20 || off == 0x50 || off == 0x54 || off == 0x58 || off == 0x60 ||
	       off == 0x90 || off == 0x94 || off == 0x98 || off == 0x9c);
	++s->writes;
	if (off == 0x50 || off == 0x54) {
		assert(!s->host[0x58/4] && !s->host[0x60/4]);
		if (s->mode == 4 && off == 0x50 && value == 0xd6521000) return;
	}
	if (off == 0x20) {
		assert(value == 1 || value == 0x400 || value == 0x20 || value == 0x40);
		s->host[off/4] &= ~value; return;
	}
	if (off == 0x90) {
		assert(value == 1 || value == 0x17 || value == 0x18);
		assert(!(s->host[0x20/4] & 0x400));
		++s->uic_commands;
		s->host[0x98/4] = 0;
		if (value == 1) {
			unsigned attr = s->host[0x94/4];
			assert(attr == 0x00410000 || attr == 0x00410001 || attr == 0x15710000);
			s->host[0x9c/4] = attr == 0x15710000 ? 0x11 : (s->hibernated ? 1 : 3);
			if (s->mode == 8) s->host[0x98/4] = 1;
		} else {
			assert(!s->host[0x58/4]);
			if ((s->mode == 6 && value == 0x18) || (s->mode == 7 && value == 0x17))
				s->host[0x98/4] = 1;
			else s->hibernated = value == 0x17;
			if (s->mode == 9 && value == 0x18) return; /* no completion */
			s->host[0x20/4] |= value == 0x17 ? 0x40 : 0x20;
		}
		s->host[0x20/4] |= 0x400;
	}
	if (off == 0x58) {
		assert(value == 1 && !s->submitted && s->host[0x60/4] == 1);
		assert(!s->hibernated);
		assert(s->host[0x50/4] == OPD_DMA_BASE && !s->host[0x54/4]);
		uint32_t *utrd = (uint32_t *)s->dma;
		assert(utrd[0] == 0x11000000 && utrd[2] == 15);
		assert(utrd[4] == OPD_DMA_BASE + 4096 && !utrd[5]);
		assert(utrd[6] == 0x00800080 && utrd[7] == 0x01000000);
		for (unsigned i = 0; i < 512; ++i) assert(s->dma[0x1000+i] == 0);
		for (unsigned i = 0; i < 512; ++i) assert(s->dma[0x1200+i] == 0xa5);
		assert(s->barriers);
		++s->submitted; s->pending = 2;
	}
	s->host[off/4] = value;
}
static void barrier(void *ctx) { ++((struct mock *)ctx)->barriers; }
static void delay_us(unsigned us, void *ctx) { (void)ctx; assert(us == 50); }
static void init(struct mock *s, unsigned char *dma)
{
	memset(s, 0, sizeof(*s)); s->dma = dma; s->hibernated = 1;
	s->host[0] = 0xd5c7031f; s->host[0x08/4] = 0x400;
	s->host[0xe4/4] = 0x60010000; s->host[0x34/4] = 1;
	s->host[0x30/4] = 0x10f; s->host[0x20/4] = 0x400;
	s->host[0x50/4] = 0xd6521000; s->host[0x60/4] = 1;
	s->host[0x90/4] = 0x17; s->host[0x9c/4] = 2;
	s->smmu[0x20/4] = 0x4c017e84; s->smmu[0x24/4] = 0x60000053;
	s->smmu[0x800/4] = 0x80000060; s->smmu[0x1000/4] = 0x0001f000;
	s->smmu[0x1800/4] = 1; s->smmu[0x80000/4] = 0x0a9f00e0;
}

int main(void)
{
	struct mock *s = malloc(sizeof(*s)); assert(s);
	unsigned char *buffer = malloc(OPD_DMA_SIZE + 32); assert(buffer);
	memset(buffer, 0x5a, OPD_DMA_SIZE + 32);
	unsigned char *dma = buffer + 16;
	struct opd_ufs_io io = {read32, write32, barrier, delay_us, s};
	init(s, dma);
	assert(!opd2413_ufs_nop(&io, dma));
	assert(s->submitted == 1 && s->host[0x50/4] == 0xd6521000 && s->host[0x60/4] == 1);
	assert(s->host[0x20/4] == 0x400); /* ABL's original IS survived */
	assert(s->hibernated && s->uic_commands == 5 && s->host[0x90/4] == 0x17);
	assert(!s->host[0x94/4] && !s->host[0x98/4] && s->host[0x9c/4] == 2);
	for (unsigned i = 0; i < 16; ++i) assert(buffer[i] == 0x5a && buffer[16+OPD_DMA_SIZE+i] == 0x5a);
	/* Any unexpected state must prevent every MMIO write. */
	const unsigned host_off[] = {0, 8, 0xe4, 0x34, 0x30, 0x24, 0x18, 0x58, 0x78, 0x300, 0x20, 0x50, 0x60};
	for (unsigned i = 0; i < sizeof(host_off)/sizeof(*host_off); ++i) {
		init(s, dma); s->host[host_off[i]/4] ^= host_off[i] == 0 ? 0x01000000 : 1;
		assert(opd2413_ufs_nop(&io, dma) == 1 && !s->writes);
	}
	const unsigned smmu_off[] = {0x20, 0x24, 0x800, 0xc00, 0x1000, 0x1800, 0x80000, 0x80020, 0x80024, 0x80028, 0x8002c, 0x80030};
	for (unsigned i = 0; i < sizeof(smmu_off)/sizeof(*smmu_off); ++i) {
		init(s, dma); s->smmu[smmu_off[i]/4] ^= 1;
		assert(opd2413_ufs_nop(&io, dma) == 1 && !s->writes);
	}
	for (unsigned mode = 1; mode <= 5; ++mode) {
		init(s, dma); s->mode = mode;
		assert(opd2413_ufs_nop(&io, dma) == -1);
		assert(s->submitted == 1 && !s->host[0x60/4]);
		if (mode == 1) assert(s->host[0x50/4] == OPD_DMA_BASE); /* no live-base restoration */
	}
	init(s, dma); s->mode = 6;
	assert(opd2413_ufs_nop(&io, dma) == -1 && !s->submitted && s->hibernated);
	init(s, dma); s->mode = 7;
	assert(opd2413_ufs_nop(&io, dma) == -1 && s->submitted == 1);
	init(s, dma); s->mode = 8;
	assert(opd2413_ufs_nop(&io, dma) == 1 && !s->submitted && s->hibernated);
	init(s, dma); s->mode = 9;
	assert(opd2413_ufs_nop(&io, dma) == -1 && !s->submitted);
	free(buffer); free(s);
	puts("PASS: NOP layout, DMA canaries, Hibern8 wake/restore, guards, timeout/OCS/UPIU/UIC failure paths");
	return 0;
}
