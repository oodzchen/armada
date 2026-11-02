// SPDX-License-Identifier: GPL-2.0+
#include <stdio.h>
#include "opd2413_ufs_link.h"

static uint32_t rd(const struct opd_ufs_io *io, unsigned off)
{
	return io->read32(OPD_UFS_BASE + off, io->ctx);
}
static void wr(const struct opd_ufs_io *io, unsigned off, uint32_t val)
{
	io->write32(OPD_UFS_BASE + off, val, io->ctx);
	io->sync_io(io->ctx);
}

int opd2413_ufs_link_check(const struct opd_ufs_io *io, uint32_t states[3])
{
	static const uint32_t attr[] = {0x00410000, 0x00410001, 0x15710000};
	static const char *const names[] = {"TX_FSM_L0", "TX_FSM_L1", "PA_PWRMODE"};
	printf("OPD2413_UFS_LINK_BEGIN\n");
	uint32_t ahb = io->read32(OPD_GCC_UFS_BASE + 0x28, io->ctx);
	if (!(io->read32(OPD_GCC_UFS_BASE + 4, io->ctx) & 0x80000000U) ||
	    ((ahb & 0x80000000U) && (ahb & 3) != 3) ||
	    rd(io, 0x34) != 1 || !(rd(io, 0x30) & 8) || rd(io, 0x24) ||
	    rd(io, 0x58) || rd(io, 0x78) || (rd(io, 0x20) & 0xf0804)) {
		printf("UFS_LINK skipped: controller not idle/ready\n"); return 1;
	}
	uint32_t args[] = {rd(io, 0x94), rd(io, 0x98), rd(io, 0x9c)};
	uint32_t old_is = rd(io, 0x20);
	printf("UFS_LINK original UIC_CMD=%08x args=%08x,%08x,%08x IS=%08x\n",
	       rd(io, 0x90), args[0], args[1], args[2], old_is);
	for (unsigned n = 0; n < sizeof(attr)/sizeof(*attr); ++n) {
		wr(io, 0x20, 0x400); /* clear previous UIC completion */
		wr(io, 0x94, attr[n]); wr(io, 0x98, 0); wr(io, 0x9c, 0);
		wr(io, 0x90, 1); /* DME_GET only */
		unsigned i;
		for (i = 0; i < 10000; ++i) {
			if (rd(io, 0x20) & 0x400) break;
			io->delay_us(50, io->ctx);
		}
		if (i == 10000 || (rd(io, 0x20) & 0xf0804)) {
			printf("OPD2413_UFS_LINK_FAIL HCS=%08x IS=%08x CMD=%08x ARG2=%08x; reset required\n",
			       rd(io, 0x30), rd(io, 0x20), rd(io, 0x90), rd(io, 0x98));
			return -1;
		}
		uint32_t result = rd(io, 0x98) & 0xff, value = rd(io, 0x9c);
		if (states) states[n] = result ? 0xffffffffU : value;
		printf("UFS_LINK %s result=%u value=%08x\n", names[n], result, value);
		/* An unsupported lane/attribute still completed safely. */
	}
	for (unsigned i = 0; i < 3; ++i) wr(io, 0x94 + 4 * i, args[i]);
	if (!(old_is & 0x400)) wr(io, 0x20, 0x400);
	printf("OPD2413_UFS_LINK_END args_restored\n");
	return 0;
}

int opd2413_ufs_hibern8(const struct opd_ufs_io *io, int enter)
{
	uint32_t event = enter ? 0x40 : 0x20;
	uint32_t old_is = rd(io, 0x20);
	if ((enter != 0 && enter != 1) || rd(io, 0x34) != 1 ||
	    !(rd(io, 0x30) & 8) || rd(io, 0x24) || rd(io, 0x58) || rd(io, 0x78) ||
	    (old_is & (0xf0804 | 0x60))) return 1;
	uint32_t args[] = {rd(io, 0x94), rd(io, 0x98), rd(io, 0x9c)};
	wr(io, 0x20, 0x400); wr(io, 0x20, event);
	wr(io, 0x94, 0); wr(io, 0x98, 0); wr(io, 0x9c, 0);
	wr(io, 0x90, enter ? 0x17 : 0x18);
	unsigned i;
	for (i = 0; i < 10000; ++i) {
		uint32_t status = rd(io, 0x20);
		if (status & 0xf0804) break;
		if ((status & (event | 0x400)) == (event | 0x400)) break;
		io->delay_us(50, io->ctx);
	}
	uint32_t status = rd(io, 0x20), hcs = rd(io, 0x30), result = rd(io, 0x98) & 0xff;
	if (i == 10000 || (status & 0xf0804) || result ||
	    (status & (event | 0x400)) != (event | 0x400) || ((hcs >> 8) & 7) != 1) {
		printf("UFS_H8_%s_FAIL HCS=%08x IS=%08x result=%u; reset required\n",
		       enter ? "ENTER" : "EXIT", hcs, status, result);
		return -1;
	}
	for (unsigned n = 0; n < 3; ++n) wr(io, 0x94 + 4 * n, args[n]);
	wr(io, 0x20, event);
	if (!(old_is & 0x400)) wr(io, 0x20, 0x400);
	printf("UFS_H8_%s_PASS\n", enter ? "ENTER" : "EXIT");
	return 0;
}
