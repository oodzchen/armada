// SPDX-License-Identifier: GPL-2.0+
/* Read-only ARM SMMUv2 handoff inspection. Register definitions follow the
 * pinned U-Boot drivers/iommu/qcom-hyp-smmu.c. No context or TLB writes.
 */
#ifdef OPD2413_HOST_TEST
#include <libfdt.h>
#else
#include <linux/libfdt.h>
#endif
#include <stdio.h>
#include "opd2413_smmu_snapshot.h"

int opd2413_smmu_resources(const void *fdt, uint32_t *sid)
{
	int node = fdt_path_offset(fdt, "/soc/apps-smmu@15000000"), len;
	if (node < 0 || fdt_node_check_compatible(fdt, node, "qcom,qsmmu-v500")) return -1;
	int parent = fdt_parent_offset(fdt, node);
	if (fdt_address_cells(fdt, parent) != 1 || fdt_size_cells(fdt, parent) != 1) return -1;
	const fdt32_t *p = fdt_getprop(fdt, node, "reg", &len);
	if (!p || len != 8 || fdt32_to_cpu(p[0]) != OPD_SMMU_BASE ||
	    fdt32_to_cpu(p[1]) != OPD_SMMU_SIZE) return -1;
	p = fdt_getprop(fdt, node, "#iommu-cells", &len);
	if (!p || len != 4 || fdt32_to_cpu(*p) != 2) return -1;
	int host = fdt_path_offset(fdt, "/soc/ufshc@1d84000");
	if (host < 0) return -1;
	p = fdt_getprop(fdt, host, "iommus", &len);
	if (!p || len != 12 || fdt32_to_cpu(p[0]) != fdt_get_phandle(fdt, node) ||
	    !fdt32_to_cpu(p[0]) || fdt32_to_cpu(p[1]) > 0xffff || fdt32_to_cpu(p[2])) return -1;
	*sid = fdt32_to_cpu(p[1]);
	return 0;
}

static uint32_t reg32(opd_read32 read32, void *ctx, unsigned offset, const char *name)
{
	uint32_t val = read32(OPD_SMMU_BASE + offset, ctx);
	printf("SMMU_RO %08llx %-12s %08x\n", (unsigned long long)(OPD_SMMU_BASE + offset), name, val);
	return val;
}

void opd2413_smmu_snapshot(opd_read32 read32, void *ctx, uint32_t sid)
{
	printf("OPD2413_SMMU_RO_BEGIN sid=%04x\n", sid);
	uint32_t id0 = reg32(read32, ctx, 0x20, "ID0");
	uint32_t id1 = reg32(read32, ctx, 0x24, "ID1");
	unsigned count = id0 & 0xff, banks = id1 & 0xff;
	unsigned shift = id1 & 0x80000000U ? 16 : 12;
	unsigned cbbase = 1U << (shift + ((id1 >> 28) & 7) + 1);
	unsigned found = 0;
	if (!id0 || id0 == 0xffffffffU || id1 == 0xffffffffU || sid > 0xffff) {
		printf("SMMU_RO invalid or inaccessible identification registers\n");
		goto done;
	}
	for (unsigned i = 0; i < count; ++i) {
		uint32_t smr = read32(OPD_SMMU_BASE + 0x800 + 4 * i, ctx);
		unsigned mask = (smr >> 16) & 0x7fff;
		if (!(smr & 0x80000000U) || ((smr ^ sid) & 0xffff & ~mask)) continue;
		++found;
		printf("SMMU_RO match=%u SMR=%08x mask=%04x\n", i, smr, mask);
		uint32_t s2cr = reg32(read32, ctx, 0xc00 + 4 * i, "S2CR");
		unsigned type = (s2cr >> 16) & 3, cb = s2cr & 0xff;
		if (type != 0) {
			printf("SMMU_RO stream type=%u (1=bypass, 2=fault); no context read\n", type);
			continue;
		}
		unsigned base = cbbase + (cb << shift), gr1 = 1U << shift;
		if (cb >= banks || base > OPD_SMMU_SIZE - 0x40 ||
		    gr1 + 0x800 + 4 * cb > OPD_SMMU_SIZE - 4) {
			printf("SMMU_RO context outside validated register window; skipped\n");
			continue;
		}
		printf("SMMU_RO context=%u page_shift=%u\n", cb, shift);
		reg32(read32, ctx, gr1 + 4 * cb, "CBAR");
		reg32(read32, ctx, gr1 + 0x800 + 4 * cb, "CBA2R");
		reg32(read32, ctx, base, "SCTLR");
		reg32(read32, ctx, base + 0x20, "TTBR0_LO");
		reg32(read32, ctx, base + 0x24, "TTBR0_HI");
		reg32(read32, ctx, base + 0x28, "TTBR1_LO");
		reg32(read32, ctx, base + 0x2c, "TTBR1_HI");
		reg32(read32, ctx, base + 0x30, "TCR");
		reg32(read32, ctx, base + 0x38, "MAIR0");
		reg32(read32, ctx, base + 0x3c, "MAIR1");
	}
	printf("SMMU_RO matching_streams=%u\n", found);
done:
	printf("OPD2413_SMMU_RO_END\n");
}
