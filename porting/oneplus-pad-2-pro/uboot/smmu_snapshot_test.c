// SPDX-License-Identifier: GPL-2.0+
#include <assert.h>
#include <libfdt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "opd2413_smmu_snapshot.h"

struct state { uint32_t *regs; unsigned context_reads, context_base, gr1; };
static uint32_t read32(uint64_t addr, void *ctx)
{
	struct state *s = ctx;
	assert(addr >= OPD_SMMU_BASE && addr < OPD_SMMU_BASE + OPD_SMMU_SIZE);
	assert(!(addr & 3));
	unsigned off = addr - OPD_SMMU_BASE;
	if (off >= 0x1000) {
		assert(off == s->gr1 + 8 || off == s->gr1 + 0x808 ||
		       (off >= s->context_base && off <= s->context_base + 0x3c));
		++s->context_reads;
	}
	return s->regs[off / 4];
}
static void exercise(struct state *s, uint32_t smr, uint32_t s2cr, uint32_t id1,
		     unsigned expected)
{
	memset(s->regs, 0, OPD_SMMU_SIZE);
	s->regs[0x20/4] = 0x08000004; /* stream matching, 4 groups */
	s->regs[0x24/4] = id1;
	s->regs[0x804/4] = smr;
	s->regs[0xc04/4] = s2cr;
	s->context_reads = 0;
	opd2413_smmu_snapshot(read32, s, 0x60);
	assert(s->context_reads == expected);
}

int main(int argc, char **argv)
{
	assert(argc == 2);
	struct state s = {.regs = calloc(1, OPD_SMMU_SIZE), .context_base = 0x82000, .gr1 = 0x1000};
	assert(s.regs);
	/* 4 KiB pages, 128 global pages, 8 context banks, stream assigned CB2. */
	exercise(&s, 0x80000060, 2, 0x60000008, 10);
	exercise(&s, 0x80010061, 2, 0x60000008, 10); /* masked match */
	exercise(&s, 0x80000061, 2, 0x60000008, 0); /* different SID */
	exercise(&s, 0x00000060, 2, 0x60000008, 0); /* invalid SMR */
	exercise(&s, 0x80000060, 0x10002, 0x60000008, 0); /* bypass */
	exercise(&s, 0x80000060, 0x20002, 0x60000008, 0); /* fault */
	exercise(&s, 0x80000060, 8, 0x60000008, 0); /* CB >= NUMCB */
	exercise(&s, 0x80000060, 2, 0xf0000008, 0); /* CB outside MMIO */
	s.context_base = 0x40000; s.gr1 = 0x10000;
	exercise(&s, 0x80000060, 2, 0x80000008, 10); /* valid 64 KiB pages */
	memset(s.regs, 0, OPD_SMMU_SIZE); s.context_reads = 0;
	opd2413_smmu_snapshot(read32, &s, 0x60);
	assert(!s.context_reads);
	free(s.regs);

	FILE *fp = fopen(argv[1], "rb"); assert(fp);
	assert(!fseek(fp, 0, SEEK_END)); long len = ftell(fp); assert(len > 0);
	rewind(fp); char *fdt = malloc((size_t)len + 4096); assert(fdt);
	assert(fread(fdt, 1, len, fp) == (size_t)len); fclose(fp);
	assert(!fdt_open_into(fdt, fdt, len + 4096));
	uint32_t sid = 0xffff;
	assert(!opd2413_smmu_resources(fdt, &sid) && sid == 0x60);
	int host = fdt_path_offset(fdt, "/soc/ufshc@1d84000"); assert(host >= 0);
	fdt32_t bad[] = {cpu_to_fdt32(1), cpu_to_fdt32(0x60), 0};
	assert(!fdt_setprop(fdt, host, "iommus", bad, sizeof(bad)));
	assert(opd2413_smmu_resources(fdt, &sid));
	assert(!fdt_delprop(fdt, host, "iommus"));
	assert(opd2413_smmu_resources(fdt, &sid));
	free(fdt);
	puts("PASS: SMMU exact/masked/invalid stream, bypass/fault, bounds, FDT phandle");
	return 0;
}
