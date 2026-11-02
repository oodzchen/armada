// SPDX-License-Identifier: GPL-2.0+
#include <assert.h>
#include <libfdt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "opd2413_ufs_snapshot.h"

struct state { unsigned flags, host, phy; };
static int expected(unsigned flags, unsigned off, unsigned sw, unsigned hwcg)
{
	return !(flags & off) || ((flags & sw) && (flags & hwcg));
}
static uint32_t read32(uint64_t addr, void *ctx)
{
	struct state *s = ctx;
	if (addr >= OPD_UFS_BASE && addr < OPD_UFS_BASE + OPD_UFS_SIZE) {
		assert(!(s->flags & 3) && expected(s->flags, 8, 32, 128));
		++s->host; return 0;
	}
	if (addr >= OPD_PHY_BASE && addr < OPD_PHY_BASE + OPD_PHY_SIZE) {
		assert(!(s->flags & 5) && expected(s->flags, 16, 64, 256));
		++s->phy; return 0;
	}
	assert((addr >= OPD_GCC_UFS_BASE && addr < OPD_GCC_UFS_BASE + OPD_GCC_UFS_SIZE) ||
	       addr == OPD_GCC_MEM_BASE);
	assert(!(addr & 3));
	if (addr == OPD_GCC_UFS_BASE) return s->flags & 1;
	if (addr == OPD_GCC_UFS_BASE + 4) return s->flags & 2 ? 0 : 0x80000000U;
	if (addr == OPD_GCC_MEM_BASE) return s->flags & 4 ? 0 : 0x80000000U;
	if (addr == OPD_GCC_UFS_BASE + 0x28)
		return (s->flags & 8 ? 0x80000000U : 0) | !!(s->flags & 32) | (!!(s->flags & 128) << 1);
	if (addr == OPD_GCC_UFS_BASE + 0xbc)
		return (s->flags & 16 ? 0x80000000U : 0) | !!(s->flags & 64) | (!!(s->flags & 256) << 1);
	return 0;
}

int main(int argc, char **argv)
{
	assert(argc == 2);
	for (unsigned flags = 0; flags < 512; ++flags) {
		struct state s = {.flags = flags};
		opd2413_ufs_snapshot(read32, &s);
		assert(!!s.host == (!(flags & 3) && expected(flags, 8, 32, 128)));
		assert(!!s.phy == (!(flags & 5) && expected(flags, 16, 64, 256)));
	}
	FILE *fp = fopen(argv[1], "rb"); assert(fp);
	assert(!fseek(fp, 0, SEEK_END)); long len = ftell(fp); assert(len > 0);
	rewind(fp);
	char *fdt = malloc((size_t)len + 4096); assert(fdt);
	assert(fread(fdt, 1, len, fp) == (size_t)len); fclose(fp);
	assert(!fdt_open_into(fdt, fdt, len + 4096));
	assert(!opd2413_ufs_resources(fdt));
	int node = fdt_path_offset(fdt, "/soc/ufshc@1d84000"); assert(node >= 0);
	fdt32_t bad[] = {cpu_to_fdt32(0x1d84000), cpu_to_fdt32(0x4000)};
	assert(!fdt_setprop(fdt, node, "reg", bad, sizeof(bad)));
	assert(opd2413_ufs_resources(fdt));
	assert(!fdt_del_node(fdt, node));
	assert(opd2413_ufs_resources(fdt));
	free(fdt);
	puts("PASS: all 512 clock/power/reset/HWCG states, resource rejection");
	return 0;
}
