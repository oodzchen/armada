// SPDX-License-Identifier: GPL-2.0+
/* Offsets: Linux v7.2 gcc-sm8750.c and phy-qcom-qmp-ufs.c, and the pinned
 * U-Boot's ufshci.h / ufs-qcom.h. Never enable clocks, reset or issue commands.
 */
#ifdef OPD2413_HOST_TEST
#include <libfdt.h>
#else
#include <linux/libfdt.h>
#endif
#include <stdio.h>
#include "opd2413_ufs_snapshot.h"

struct snapshot_reg { unsigned offset; const char *name; };
#define R(off, name) {off, #name}

static int resource(const void *fdt, const char *path, const char *compatible,
		    unsigned base, unsigned size, int tuples)
{
	int node = fdt_path_offset(fdt, path), len;
	if (node < 0 || fdt_node_check_compatible(fdt, node, compatible)) return -1;
	int parent = fdt_parent_offset(fdt, node);
	if (fdt_address_cells(fdt, parent) != 1 || fdt_size_cells(fdt, parent) != 1)
		return -1;
	const fdt32_t *p = fdt_getprop(fdt, node, "reg", &len);
	return p && len == tuples * 8 && fdt32_to_cpu(p[0]) == base &&
		fdt32_to_cpu(p[1]) == size ? 0 : -1;
}

int opd2413_ufs_resources(const void *fdt)
{
	return resource(fdt, "/soc/clock-controller@100000", "qcom,sun-gcc",
			0x100000, 0x1f4200, 1) ||
		resource(fdt, "/soc/ufsphy_mem@1d80000", "qcom,ufs-phy-qmp-v4-sun",
			 OPD_PHY_BASE, OPD_PHY_SIZE, 1) ||
		resource(fdt, "/soc/ufshc@1d84000", "qcom,ufshc",
			 OPD_UFS_BASE, OPD_UFS_SIZE, 4) ? -1 : 0;
}

static uint32_t read_reg(opd_read32 read32, void *ctx, uint64_t addr, const char *name)
{
	uint32_t val = read32(addr, ctx);
	printf("UFS_RO %08llx %-20s %08x\n", (unsigned long long)addr, name, val);
	return val;
}

static void read_regs(opd_read32 read32, void *ctx, uint64_t base,
		      const struct snapshot_reg *regs, unsigned count)
{
	for (unsigned i = 0; i < count; ++i)
		read_reg(read32, ctx, base + regs[i].offset, regs[i].name);
}

static int clock_available(uint32_t cbcr)
{
	/* Linux clk-branch.c skips CLK_OFF in hardware-gated mode. ABL leaves
	 * the AHB branch enabled with HWCG=1, so its idle CLK_OFF is normal.
	 * Require both the software vote and HWCG before accepting that case.
	 */
	return !(cbcr & 0x80000000U) || (cbcr & 3) == 3;
}

void opd2413_ufs_snapshot(opd_read32 read32, void *ctx)
{
	static const struct snapshot_reg gcc[] = {
		R(0x018, AXI_CBCR), R(0x0f0, AGGRE_AXI_CBCR),
		R(0x07c, ICE_CBCR), R(0x06c, UNIPRO_CBCR),
		R(0x030, RX0_CBCR), R(0x0d8, RX1_CBCR), R(0x02c, TX0_CBCR),
		R(0x034, AXI_CMD_RCGR), R(0x038, AXI_CFG_RCGR),
		R(0x08c, ICE_CMD_RCGR), R(0x090, ICE_CFG_RCGR),
		R(0x0a4, UNIPRO_CMD_RCGR), R(0x0a8, UNIPRO_CFG_RCGR),
		R(0x0c0, AUX_CMD_RCGR), R(0x0c4, AUX_CFG_RCGR),
		R(0x068, RX0_MUX), R(0x0ec, RX1_MUX), R(0x058, TX0_MUX),
	};
	static const struct snapshot_reg host[] = {
		R(0x000, CAP), R(0x004, MCQCAP), R(0x008, VER), R(0x00c, EXT_CAP),
		R(0x018, AHIT), R(0x020, IS), R(0x024, IE),
		R(0x030, HCS), R(0x034, HCE),
		R(0x050, UTRLBA_L), R(0x054, UTRLBA_H), R(0x058, UTRLDBR),
		R(0x060, UTRLRSR), R(0x070, UTMRLBA_L), R(0x074, UTMRLBA_H),
		R(0x078, UTMRLDBR), R(0x080, UTMRLRSR),
		R(0x0d0, PARAM0), R(0x0d8, CFG0), R(0x0dc, CFG1),
		R(0x0e0, CFG2), R(0x0e4, HW_VERSION), R(0x300, MEM_CFG),
		R(0x380, MCQ_CFG), R(0x271c, CFG3),
	};
	static const struct snapshot_reg phy[] = {
		R(0x400, PCS_START), R(0x404, PCS_POWER_DOWN),
		R(0x408, PCS_SW_RESET), R(0x5a8, PCS_READY),
	};

	printf("OPD2413_UFS_RO_BEGIN\n");
	uint32_t reset = read_reg(read32, ctx, OPD_GCC_UFS_BASE, "BCR");
	uint32_t power = read_reg(read32, ctx, OPD_GCC_UFS_BASE + 4, "PHY_GDSCR");
	uint32_t mem = read_reg(read32, ctx, OPD_GCC_MEM_BASE, "MEM_PHY_GDSCR");
	uint32_t ahb = read_reg(read32, ctx, OPD_GCC_UFS_BASE + 0x28, "AHB_CBCR");
	uint32_t aux = read_reg(read32, ctx, OPD_GCC_UFS_BASE + 0xbc, "AUX_CBCR");
	read_regs(read32, ctx, OPD_GCC_UFS_BASE, gcc, sizeof(gcc)/sizeof(*gcc));
	/* GDSC PWR_ON=bit31; branch CLK_OFF=bit31; BCR=bit0. Fail closed. */
	if (!(reset & 1) && (power & 0x80000000U) && clock_available(ahb))
		read_regs(read32, ctx, OPD_UFS_BASE, host, sizeof(host)/sizeof(*host));
	else printf("UFS_RO host skipped: reset, power or AHB clock is off\n");
	if (!(reset & 1) && (mem & 0x80000000U) && clock_available(aux))
		read_regs(read32, ctx, OPD_PHY_BASE, phy, sizeof(phy)/sizeof(*phy));
	else printf("UFS_RO PHY skipped: reset, power or AUX clock is off\n");
	printf("OPD2413_UFS_RO_END\n");
}
