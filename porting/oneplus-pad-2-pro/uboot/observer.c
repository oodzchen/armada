// SPDX-License-Identifier: GPL-2.0+
/* RAM-only OPD2413 bring-up. No storage or peripheral controller is enabled. */
#include <asm/armv8/mmu.h>
#include <asm/global_data.h>
#include <asm/io.h>
#include <asm/system.h>
#include <asm/u-boot-arm.h>
#include <command.h>
#include <init.h>
#include <dm.h>
#include <dm/uclass.h>
#include <console.h>
#include <linux/libfdt.h>
#include <linux/sizes.h>
#include <stdio.h>
#include <string.h>
#include <u-boot/crc.h>
#include "qcom-priv.h"
#include "opd2413_layout.h"
#include "opd2413_ufs_snapshot.h"
#include "opd2413_smmu_snapshot.h"

DECLARE_GLOBAL_DATA_PTR;
/* board_fdt_blob_setup runs before BSS is available. */
static unsigned char probe_fdt[65536] __aligned(8) __section(".data") = {1};
static struct mm_region probe_map[OPD_MAX_RANGES + 7];
static u32 original_fdt_crc __section(".data") = 1;
static u64 original_kernel_instructions __section(".data") = 1;
#ifdef CONFIG_OPD2413_SMMU_SNAPSHOT
static u32 original_ufs_sid __section(".data") = 0xffff;
#endif

#define FB_BASE 0xfc800000ULL
#define FB_SIZE (3392ULL * 2400 * 4)
#define LOG_BASE 0x880000000ULL
#define LOG_SIZE 0x40000ULL

static uint64_t cell64(const fdt32_t *p)
{
	return ((uint64_t)fdt32_to_cpu(p[0]) << 32) | fdt32_to_cpu(p[1]);
}

int opd2413_filter_fdt(const void *src, void *dest, unsigned capacity);
int opd2413_observer_fdt(void **out)
{
    const void *src = (const void *)get_prev_bl_fdt_addr();
    int ret = opd2413_filter_fdt(src, probe_fdt, sizeof(probe_fdt));
    if (ret) return ret;
#ifdef CONFIG_OPD2413_UFS_SNAPSHOT
    if (opd2413_ufs_resources(src)) return -EINVAL;
#endif
#ifdef CONFIG_OPD2413_SMMU_SNAPSHOT
    if (opd2413_smmu_resources(src, &original_ufs_sid)) return -EINVAL;
#endif
    original_fdt_crc = crc32(0, src, fdt_totalsize(src));
    original_kernel_instructions = *(const volatile u64 *)0xa8000000ULL;
    ret = qcom_parse_memory(src, false);
    if (ret) return ret;
    qcom_boot_source = QCOM_BOOT_SOURCE_ANDROID;
    *out = probe_fdt;
    return 0;
}

void opd2413_observer_map(void)
{
	struct opd_range ram[OPD_MAX_RANGES];
	size_t count = 0;
	for (unsigned i = 0; i < CONFIG_NR_DRAM_BANKS; ++i) {
		if (!gd->dram[i].size) continue;
		ram[count++] = (struct opd_range){gd->dram[i].start, gd->dram[i].size};
	}
	for (int i = 0; i < fdt_num_mem_rsv(gd->fdt_blob); ++i) {
		uint64_t base, size;
		if (fdt_get_mem_rsv(gd->fdt_blob, i, &base, &size) ||
		    opd_carve(ram, &count, (struct opd_range){base, size}))
			panic("observer: invalid reserve map\n");
	}
	int parent = fdt_path_offset(gd->fdt_blob, "/reserved-memory"), node;
	fdt_for_each_subnode(node, gd->fdt_blob, parent) {
		int len;
		const fdt32_t *p = fdt_getprop(gd->fdt_blob, node, "reg", &len);
		if (!p || len <= 0 || len % 16) panic("observer: invalid reservation\n");
		for (int i = 0; i < len / 4; i += 4)
			if (opd_carve(ram, &count, (struct opd_range){cell64(p+i), cell64(p+i+2)}))
				panic("observer: invalid memory carveout\n");
	}
	if (!opd_contains(ram, count, LOG_BASE, LOG_SIZE) ||
	    !opd_contains(ram, count, gd->relocaddr, gd->mon_len))
		panic("observer: log or relocated image outside usable RAM\n");
	for (size_t i = 0; i < count; ++i)
		probe_map[i] = (struct mm_region){.phys=ram[i].base, .virt=ram[i].base,
			.size=ram[i].size, .attrs=PTE_BLOCK_MEMTYPE(MT_NORMAL) | PTE_BLOCK_INNER_SHARE};
	probe_map[count] = (struct mm_region){.phys=FB_BASE, .virt=FB_BASE, .size=FB_SIZE,
		.attrs=PTE_BLOCK_MEMTYPE(MT_NORMAL_NC) | PTE_BLOCK_INNER_SHARE | PTE_BLOCK_PXN | PTE_BLOCK_UXN};
	++count;
#ifdef CONFIG_OPD2413_UFS_SNAPSHOT
	const struct opd_range mmio[] = {
		{OPD_GCC_UFS_BASE, OPD_GCC_UFS_SIZE}, {OPD_GCC_MEM_BASE, OPD_GCC_MEM_SIZE},
		{OPD_UFS_BASE, OPD_UFS_SIZE}, {OPD_PHY_BASE, OPD_PHY_SIZE},
#ifdef CONFIG_OPD2413_SMMU_SNAPSHOT
		{OPD_SMMU_BASE, OPD_SMMU_SIZE},
#endif
	};
	for (unsigned i = 0; i < ARRAY_SIZE(mmio); ++i)
		probe_map[count++] = (struct mm_region){.phys=mmio[i].base, .virt=mmio[i].base,
			.size=mmio[i].size, .attrs=PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			PTE_BLOCK_NON_SHARE | PTE_BLOCK_RO | PTE_BLOCK_PXN | PTE_BLOCK_UXN};
#endif
	probe_map[count] = (struct mm_region){0};
	mem_map = probe_map;
}

#ifdef CONFIG_OPD2413_UFS_SNAPSHOT
static uint32_t snapshot_read32(uint64_t addr, void *ctx)
{
	return readl((const volatile void *)(uintptr_t)addr);
}
#endif

int opd2413_observer_late(void)
{
	struct udevice *display;
	int ret = uclass_first_device_err(UCLASS_VIDEO, &display);
	printf("observer: video probe=%d\n", ret);
	if (!ret) {
		iomux_doenv(stdout, "serial,vidconsole");
		iomux_doenv(stderr, "serial,vidconsole");
	}
	printf("OPD2413_OBSERVER_READY EL%u original_fdt=%llx\n", current_el(),
	       (unsigned long long)get_prev_bl_fdt_addr());
	printf("No storage drivers or environment writes enabled.\n");
#ifdef CONFIG_OPD2413_UFS_SNAPSHOT
	opd2413_ufs_snapshot(snapshot_read32, NULL);
#endif
#ifdef CONFIG_OPD2413_SMMU_SNAPSHOT
	opd2413_smmu_snapshot(snapshot_read32, NULL, original_ufs_sid);
#endif
	return 0;
}

/* Observer-only return to the already loaded ABL kernel, never an Android
 * image loaded by U-Boot. Keep its original DT/ramdisk/TEE handoff intact. */
static int do_opd_android(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	const void *fdt = (const void *)get_prev_bl_fdt_addr();
	const volatile u32 *image = (const volatile u32 *)0xa8000000ULL;
	if (fdt_check_header(fdt) ||
	    crc32(0, fdt, fdt_totalsize(fdt)) != original_fdt_crc ||
	    *(const volatile u64 *)image != original_kernel_instructions ||
	    image[14] != 0x644d5241 || (image[1] & 0xfc000000U) != 0x14000000U) {
		printf("observer: original ABL handoff changed; refusing Android return\n");
		return CMD_RET_FAILURE;
	}
	printf("OPD2413_ANDROID_HANDOFF fdt_crc=%08x entry=a8000004\n", original_fdt_crc);
	cleanup_before_linux();
	((void (*)(u64, u64, u64, u64))0xa8000004ULL)((u64)fdt, 0, 0, 0);
	return CMD_RET_FAILURE;
}
U_BOOT_CMD(opd_android, 1, 0, do_opd_android,
	   "return to the original ABL-loaded Android kernel", "");
