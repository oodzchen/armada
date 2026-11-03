// SPDX-License-Identifier: GPL-2.0+
/* OPD2413 observer with optional explicit UFS diagnostics; no storage driver. */
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
#include <linux/delay.h>
#include <stdio.h>
#include <string.h>
#include <u-boot/crc.h>
#include "qcom-priv.h"
#include "opd2413_layout.h"
#include "opd2413_ufs_snapshot.h"
#include "opd2413_smmu_snapshot.h"
#include "opd2413_ufs_nop.h"
#include "opd2413_ufs_link.h"
#include "opd2413_ufs_read.h"
#ifdef CONFIG_OPD2413_UFS_BLK
#include "opd2413_ufs_blk.h"
#endif
#ifdef CONFIG_OPD2413_EFI
#include <env.h>
#include <lmb.h>
#endif
#ifdef CONFIG_OPD2413_KEYS
#include <env.h>
#include <cyclic.h>
#include <stdio_dev.h>
#include <time.h>
#include <vsprintf.h>
#include "opd2413_keys.h"
#endif

DECLARE_GLOBAL_DATA_PTR;
/* board_fdt_blob_setup runs before BSS is available. */
static unsigned char probe_fdt[65536] __aligned(8) __section(".data") = {1};
static struct mm_region probe_map[OPD_MAX_RANGES + 11];
static u32 original_fdt_crc __section(".data") = 1;
static u64 original_kernel_instructions __section(".data") = 1;
#ifdef CONFIG_OPD2413_SMMU_SNAPSHOT
static u32 original_ufs_sid __section(".data") = 0xffff;
#endif
#if defined(CONFIG_OPD2413_UFS_NOP) || defined(CONFIG_OPD2413_UFS_LINK_CHECK) || defined(CONFIG_OPD2413_UFS_READ)
static int android_handoff_safe = 1;
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
#if defined(CONFIG_OPD2413_UFS_NOP) || defined(CONFIG_OPD2413_UFS_READ)
	if (!opd_contains(ram, count, OPD_DMA_BASE, OPD_DMA_SIZE) ||
	    opd_carve(ram, &count, (struct opd_range){OPD_DMA_BASE, OPD_DMA_SIZE}))
		panic("observer: DMA buffer outside usable RAM\n");
#endif
	for (size_t i = 0; i < count; ++i)
		probe_map[i] = (struct mm_region){.phys=ram[i].base, .virt=ram[i].base,
			.size=ram[i].size, .attrs=PTE_BLOCK_MEMTYPE(MT_NORMAL) | PTE_BLOCK_INNER_SHARE};
	probe_map[count] = (struct mm_region){.phys=FB_BASE, .virt=FB_BASE, .size=FB_SIZE,
		.attrs=PTE_BLOCK_MEMTYPE(MT_NORMAL_NC) | PTE_BLOCK_INNER_SHARE | PTE_BLOCK_PXN | PTE_BLOCK_UXN};
	++count;
#ifdef CONFIG_OPD2413_UFS_SNAPSHOT
	const struct opd_range mmio[] = {
		{OPD_GCC_UFS_BASE, OPD_GCC_UFS_SIZE}, {OPD_GCC_MEM_BASE, OPD_GCC_MEM_SIZE},
		{OPD_UFS_BASE, 0x1000}, {OPD_UFS_BASE + 0x1000, OPD_UFS_SIZE - 0x1000},
		{OPD_PHY_BASE, OPD_PHY_SIZE},
#ifdef CONFIG_OPD2413_SMMU_SNAPSHOT
		{OPD_SMMU_BASE, OPD_SMMU_SIZE},
#endif
	};
	for (unsigned i = 0; i < ARRAY_SIZE(mmio); ++i) {
		u64 ro = PTE_BLOCK_RO;
#if defined(CONFIG_OPD2413_UFS_NOP) || defined(CONFIG_OPD2413_UFS_LINK_CHECK) || defined(CONFIG_OPD2413_UFS_READ)
		if (mmio[i].base == OPD_UFS_BASE) ro = 0;
#endif
		probe_map[count++] = (struct mm_region){.phys=mmio[i].base, .virt=mmio[i].base,
			.size=mmio[i].size, .attrs=PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			PTE_BLOCK_NON_SHARE | ro | PTE_BLOCK_PXN | PTE_BLOCK_UXN};
	}
#endif
#ifdef CONFIG_OPD2413_KEYS
	/* SPMI arbiter: core read-only; EE 0 observer channels for read commands. */
	probe_map[count++] = (struct mm_region){.phys=OPD_SPMI_CORE, .virt=OPD_SPMI_CORE, .size=OPD_SPMI_CORE_SIZE,
		.attrs=PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) | PTE_BLOCK_NON_SHARE | PTE_BLOCK_RO | PTE_BLOCK_PXN | PTE_BLOCK_UXN};
	probe_map[count++] = (struct mm_region){.phys=OPD_SPMI_OBS, .virt=OPD_SPMI_OBS, .size=OPD_SPMI_OBS_SIZE,
		.attrs=PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) | PTE_BLOCK_NON_SHARE | PTE_BLOCK_PXN | PTE_BLOCK_UXN};
#endif
#if defined(CONFIG_OPD2413_UFS_NOP) || defined(CONFIG_OPD2413_UFS_READ)
	/* Fixed private window inside the discarded 16 MiB U-Boot load slot.
	 * This single-NOP observer invokes no subsequent low-memory allocator.
	 */
	probe_map[count++] = (struct mm_region){.phys=OPD_DMA_BASE, .virt=OPD_DMA_BASE,
		.size=OPD_DMA_SIZE, .attrs=PTE_BLOCK_MEMTYPE(MT_NORMAL_NC) |
		PTE_BLOCK_INNER_SHARE | PTE_BLOCK_PXN | PTE_BLOCK_UXN};
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

#if defined(CONFIG_OPD2413_UFS_NOP) || defined(CONFIG_OPD2413_UFS_LINK_CHECK) || defined(CONFIG_OPD2413_UFS_READ)
static void probe_write32(uint64_t addr, uint32_t value, void *ctx)
{
	switch (addr - OPD_UFS_BASE) {
#if defined(CONFIG_OPD2413_UFS_NOP) || defined(CONFIG_OPD2413_UFS_READ)
	case 0x20:
		if (value != 1 && value != 0x400 && value != 0x20 && value != 0x40)
			panic("NOP: invalid W1C\n");
		break;
	case 0x50: case 0x54: case 0x58: case 0x60: break;
	case 0x90:
		if (value != 1 && value != 0x17 && value != 0x18)
			panic("NOP: invalid UIC command\n");
		break;
	case 0x94: case 0x98: case 0x9c: break;
#endif
#ifdef CONFIG_OPD2413_UFS_LINK_CHECK
	case 0x20: if (value != 0x400) panic("GET: invalid W1C\n"); break;
	case 0x90: if (value != 1) panic("GET: invalid command\n"); break;
	case 0x94: case 0x98: case 0x9c: break;
#endif
	default: panic("NOP: invalid register write\n");
	}
	writel(value, (volatile void *)(uintptr_t)addr);
}
static void probe_sync(void *ctx) { mb(); }
static void probe_delay(unsigned us, void *ctx) { udelay(us); }
#endif

static u64 read_cntpct(void)
{
	u64 v;
	asm volatile("isb; mrs %0, cntpct_el0" : "=r"(v));
	return v;
}
static u64 read_sp(void) { u64 v; asm volatile("mov %0, sp" : "=r"(v)); return v; }
static u64 read_daif(void) { u64 v; asm volatile("mrs %0, daif" : "=r"(v)); return v; }

/* Called first thing in every main_loop() entry (common/main.c patch). */
void opd2413_main_loop_hook(void)
{
	static unsigned entries;
	const u64 *frame = __builtin_frame_address(0);
	const u64 *caller = (const u64 *)frame[0]; /* main_loop's frame record */
	printf("OPD2413_MAIN_LOOP entry=%u sp=%llx fp=%llx lr=%llx daif=%llx cntpct=%llx\n", ++entries,
	       read_sp(), (unsigned long long)(uintptr_t)caller,
	       caller ? (unsigned long long)caller[1] - gd->reloc_off : 0ULL, read_daif(), read_cntpct());
}

#ifdef CONFIG_OPD2413_UFS_TRACE
static int trace_budget; /* commands left to trace; set after board init */
/*
 * Frame-record chain and stack contents above the caller of submit()/query()
 * must not change while the doorbell is outstanding: neither the CPU nor the
 * UFS DMA should touch them. Compare a CRC of [caller frame, initial SP).
 */
static u32 stack_crc(const u64 *frame, unsigned *depth, u64 *base)
{
	const u64 *fp = (const u64 *)frame[0]; /* submit()/query() frame */
	fp = fp ? (const u64 *)fp[0] : NULL;    /* its caller */
	u64 top = gd->start_addr_sp;
	unsigned n = 0;
	*base = (u64)(uintptr_t)fp;
	if (!fp || (u64)(uintptr_t)fp >= top) { *depth = 0; return 0; }
	for (const u64 *f = fp; f && (u64)(uintptr_t)f < top && !((uintptr_t)f & 7) && n < 64; f = (const u64 *)f[0]) ++n;
	*depth = n;
	return crc32(0, (const void *)fp, top - (u64)(uintptr_t)fp);
}

void opd2413_ufs_trace(int phase, uint8_t op, uint8_t lun)
{
	static u32 before_crc;
	static unsigned before_depth;
	unsigned depth;
	u64 base;
	if (trace_budget <= 0) return;
	u32 crc = stack_crc(__builtin_frame_address(0), &depth, &base);
	if (!phase) {
		before_crc = crc; before_depth = depth;
		printf("OPD2413_TRACE ring %02x lun %02x sp=%llx base=%llx top=%llx frames=%u\n", op, lun,
		       read_sp(), base, (unsigned long long)gd->start_addr_sp, depth);
		return;
	}
	printf("OPD2413_TRACE done %02x frames=%u %s cntpct=%llx\n", op, depth,
	       crc == before_crc && depth == before_depth ? "stack ok" : "STACK CHANGED", read_cntpct());
	if (--trace_budget == 0) printf("OPD2413_TRACE budget used; tracing off\n");
}
#endif

#ifdef CONFIG_OPD2413_EFI
/*
 * EFI allocates through LMB. Before any EFI code runs, reserve everything the
 * Android return depends on: ABL's kernel, FDT and initrd (one RAM bank from
 * 0xa8000000), the U-Boot load slot with the UFS DMA window, the framebuffer
 * and the ramoops zone. A CRC of the ABL range taken here is checked again by
 * opd_android, so the return is refused if anything wrote into it.
 */
#define OPD_ABL_BASE 0xa8000000ULL
#define OPD_SLOT_BASE 0xb9000000ULL
#define OPD_SLOT_END 0xba000000ULL
#define OPD_RAMOOPS_SIZE 0x240000ULL
static u64 abl_end __section(".data");
static u32 abl_crc __section(".data");

#ifdef CONFIG_OPD2413_EFI_ESP
/* FAT ESP image built by build.py --efi-esp, exposed through blkmap. */
asm(".section .rodata.opd2413_esp, \"a\"\n.balign 4096\n.global opd2413_esp_start\nopd2413_esp_start:\n"
    ".incbin \"" CONFIG_OPD2413_EFI_ESP_IMAGE "\"\n.balign 512\n.global opd2413_esp_end\nopd2413_esp_end:\n.previous\n");
extern const char opd2413_esp_start[], opd2413_esp_end[];
#endif

static int chosen_u64(const void *fdt, int node, const char *name, u64 *out)
{
	int len;
	const fdt32_t *p = fdt_getprop(fdt, node, name, &len);
	if (!p || (len != 4 && len != 8)) return -1;
	*out = len == 8 ? cell64(p) : fdt32_to_cpu(p[0]);
	return 0;
}

/* EFI ResetSystem: return to Android through the checked path; on refusal
 * the caller continues with the ordinary reset. */
void opd2413_efi_reset_hook(int type)
{
	printf("OPD2413_EFI_RESET type=%d: returning to Android instead\n", type);
	run_command("opd_android", 0);
	printf("OPD2413_EFI_RESET Android return refused; firmware reset follows\n");
}

/* Reserve every still-free byte of [base, end) that LMB knows as memory. */
static int protect(u64 base, u64 end, unsigned *pieces)
{
	struct lmb *l = lmb_get();
	for (unsigned m = 0; m < l->available_mem.count; ++m) {
		const struct lmb_region *mem = (const struct lmb_region *)l->available_mem.data + m;
		u64 cur = max(base, (u64)mem->base), stop = min(end, (u64)mem->base + mem->size);
		while (cur < stop) {
			const struct lmb_region *used = l->used_mem.data;
			u64 next = stop;
			int inside = 0;
			for (unsigned u = 0; u < l->used_mem.count; ++u) {
				u64 ub = used[u].base, ue = ub + used[u].size;
				if (ub <= cur && cur < ue) { cur = ue; inside = 1; break; }
				if (ub > cur && ub < next) next = ub;
			}
			if (inside) continue;
			phys_addr_t a = cur;
			if (lmb_alloc_mem(LMB_MEM_ALLOC_ADDR, 0, &a, next - cur, LMB_NOOVERWRITE)) {
				printf("OPD2413_EFI_PROTECT reserve %llx-%llx failed\n", cur, next);
				return -1;
			}
			++*pieces; cur = next;
		}
	}
	return 0;
}

/* Allocation anywhere in [base, end) must now be impossible. */
static int unallocatable(u64 base, u64 end)
{
	for (u64 a = base & ~0xfffULL; a < end; a += SZ_1M) {
		phys_addr_t p = a;
		if (!lmb_alloc_mem(LMB_MEM_ALLOC_ADDR, 0, &p, 0x1000, LMB_NONE)) {
			lmb_free(p, 0x1000, LMB_NONE);
			printf("OPD2413_EFI_PROTECT %llx still allocatable\n", a);
			return -1;
		}
	}
	return 0;
}

static void efi_protect(void)
{
	const void *fdt = (const void *)get_prev_bl_fdt_addr();
	int chosen = fdt_path_offset(fdt, "/chosen");
	u64 rd_start, rd_end, fdt_end = (u64)(uintptr_t)fdt + fdt_totalsize(fdt);
	struct lmb *l = lmb_get();
	unsigned pieces = 0, bank = 0;
	if (chosen < 0 || chosen_u64(fdt, chosen, "linux,initrd-start", &rd_start) ||
	    chosen_u64(fdt, chosen, "linux,initrd-end", &rd_end) || rd_end < rd_start) {
		printf("OPD2413_EFI_PROTECT no initrd range in the ABL FDT; EFI disabled\n");
		return;
	}
	abl_end = max(fdt_end, rd_end);
	for (unsigned m = 0; m < l->available_mem.count; ++m) {
		const struct lmb_region *mem = (const struct lmb_region *)l->available_mem.data + m;
		if (mem->base <= OPD_ABL_BASE && abl_end <= mem->base + mem->size) bank = 1;
	}
	if ((u64)(uintptr_t)fdt < OPD_ABL_BASE || rd_start < OPD_ABL_BASE || abl_end > OPD_SLOT_BASE || !bank) {
		printf("OPD2413_EFI_PROTECT unexpected layout fdt=%llx-%llx initrd=%llx-%llx; EFI disabled\n",
		       (u64)(uintptr_t)fdt, fdt_end, rd_start, rd_end);
		return;
	}
	/* Already-used regions are skipped by protect(); list those in the ABL
	 * range (the handoff CRC still covers them). */
	for (unsigned u = 0; u < l->used_mem.count; ++u) {
		const struct lmb_region *r = (const struct lmb_region *)l->used_mem.data + u;
		if (r->base < abl_end && r->base + r->size > OPD_ABL_BASE)
			printf("OPD2413_EFI_PROTECT pre-used %llx-%llx flags=%x\n", (u64)r->base,
			       (u64)(r->base + r->size), r->flags);
	}
	const u64 ranges[][2] = {{OPD_ABL_BASE, abl_end}, {OPD_SLOT_BASE, OPD_SLOT_END},
				 {FB_BASE, FB_BASE + FB_SIZE}, {LOG_BASE, LOG_BASE + OPD_RAMOOPS_SIZE}};
	for (unsigned i = 0; i < ARRAY_SIZE(ranges); ++i)
		if (protect(ranges[i][0], ranges[i][1], &pieces) || unallocatable(ranges[i][0], ranges[i][1])) {
			printf("OPD2413_EFI_PROTECT failed; EFI disabled\n");
			return;
		}
	abl_crc = crc32(0, (const void *)OPD_ABL_BASE, abl_end - OPD_ABL_BASE);
	printf("OPD2413_EFI_PROTECT abl=%llx-%llx initrd=%llx-%llx crc=%08x pieces=%u\n",
	       OPD_ABL_BASE, abl_end, rd_start, rd_end, abl_crc, pieces);
	env_set("opd_efi_safe", "1");
#ifdef CONFIG_OPD2413_EFI_ESP
	size_t esp = opd2413_esp_end - opd2413_esp_start;
	env_set_hex("opd_esp_addr", (ulong)opd2413_esp_start);
	env_set_hex("opd_esp_blocks", esp / 512);
	printf("OPD2413_EFI_ESP addr=%p size=%zu crc=%08x\n", opd2413_esp_start, esp,
	       crc32(0, (const void *)opd2413_esp_start, esp));
#endif
}
#endif

#ifdef CONFIG_OPD2413_KEYS
/*
 * Volume up / volume down / power as an input device ("opdkeys"): up and
 * down arrows and Enter, which U-Boot's EFI console turns into EFI keys.
 * Only press edges after initialization count, so a key still held from the
 * kShimC menu is ignored.
 */
static struct opd_keys keys;
static int keys_ok;
static char keyq[32];
static unsigned keyq_head, keyq_tail;
static ulong keys_last_us;

static uint32_t keys_read32(uint64_t addr, void *ctx) { return readl((const volatile void *)(uintptr_t)addr); }
static void keys_write32(uint64_t addr, uint32_t value, void *ctx)
{
	if (!opd2413_keys_write_allowed(addr, value)) panic("KEYS: invalid register write\n");
	writel(value, (volatile void *)(uintptr_t)addr);
}
static void keys_sync(void *ctx) { mb(); }
static void keys_delay(unsigned us, void *ctx) { udelay(us); }

static void keyq_put(const char *s)
{
	for (; *s; ++s) {
		unsigned next = (keyq_tail + 1) % sizeof(keyq);
		if (next == keyq_head) return; /* full: drop */
		keyq[keyq_tail] = *s; keyq_tail = next;
	}
}

static int keys_poll_events(void)
{
	if (!keys_ok || timer_get_us() - keys_last_us < 5000) return 0;
	keys_last_us = timer_get_us();
	int edges = opd2413_keys_poll(&keys);
	if (edges < 0) return -1;
	if (edges & 1 << OPD_KEY_UP) keyq_put("\x1b[A");
	if (edges & 1 << OPD_KEY_DOWN) keyq_put("\x1b[B");
	if (edges & 1 << OPD_KEY_POWER) keyq_put("\r");
	return edges;
}

static int keys_tstc(struct stdio_dev *dev)
{
	keys_poll_events();
	return keyq_head != keyq_tail;
}

static int keys_getc(struct stdio_dev *dev)
{
	while (!keys_tstc(dev)) schedule();
	char c = keyq[keyq_head];
	keyq_head = (keyq_head + 1) % sizeof(keyq);
	return (unsigned char)c;
}

static void keys_start(void)
{
	static const struct opd_ufs_io io = {keys_read32, keys_write32, keys_sync, keys_delay, NULL};
	static struct stdio_dev dev = {.name = "opdkeys", .flags = DEV_FLAGS_INPUT, .getc = keys_getc, .tstc = keys_tstc};
	/* The stream ID comes from the FDT, which QEMU shares; the SPMI MMIO
	 * exists only on the tablet's Qualcomm (implementer 0x51) cores. */
	u64 midr;
	asm volatile("mrs %0, midr_el1" : "=r"(midr));
	if (current_el() != 1 || original_ufs_sid != 0x60 || (midr >> 24 & 0xff) != 0x51) {
		printf("OPD2413_KEYS skipped: unexpected EL, stream ID or CPU (midr %llx)\n", midr);
		return;
	}
	if (opd2413_keys_init(&keys, &io)) return;
	keys_ok = !stdio_register(&dev);
	if (!keys_ok) printf("OPD2413_KEYS stdio registration failed\n");
}

/* opd_keys <seconds>: report presses until all three keys were seen. */
static int do_opd_keys(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	static const char *const names[OPD_KEYS] = {"volume_up", "volume_down", "power"};
	ulong seconds = argc > 1 ? dectoul(argv[1], NULL) : 20, start = get_timer(0);
	unsigned seen = 0;
	env_set("opd_keys_ok", "0");
	if (!keys_ok) { printf("OPD2413_KEYS not available\n"); return CMD_RET_FAILURE; }
	printf("OPD2413_KEYS press Volume Up, Volume Down, then Power (%lu s)\n", seconds);
	while (seen != 7 && get_timer(start) < seconds * 1000) {
		int edges = opd2413_keys_poll(&keys);
		if (edges < 0) { printf("OPD2413_KEYS SPMI read failed\n"); return CMD_RET_FAILURE; }
		for (unsigned i = 0; i < OPD_KEYS; ++i)
			if (edges & 1 << i) { seen |= 1 << i; printf("OPD2413_KEY %s\n", names[i]); }
		mdelay(10);
	}
	keyq_head = keyq_tail = 0;
	printf("OPD2413_KEYS %s (seen mask %u)\n", seen == 7 ? "all keys work" : "timeout", seen);
	if (seen != 7) return CMD_RET_FAILURE;
	env_set("opd_keys_ok", "1");
	return CMD_RET_SUCCESS;
}
U_BOOT_CMD(opd_keys, 2, 0, do_opd_keys, "read-only volume/power key test", "[seconds]");
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
	/* The counter runs from power-on: a re-entered observer prints a larger value. */
	printf("OPD2413_OBSERVER_READY EL%u original_fdt=%llx cntpct=%llx\n", current_el(),
	       (unsigned long long)get_prev_bl_fdt_addr(), read_cntpct());
	printf("No storage drivers or environment writes enabled.\n");
#ifdef CONFIG_OPD2413_EFI
	efi_protect(); /* before the block devices register EFI disks */
#endif
#ifdef CONFIG_OPD2413_KEYS
	keys_start();
#endif
#ifdef CONFIG_OPD2413_UFS_SNAPSHOT
	opd2413_ufs_snapshot(snapshot_read32, NULL);
#endif
#ifdef CONFIG_OPD2413_SMMU_SNAPSHOT
	opd2413_smmu_snapshot(snapshot_read32, NULL, original_ufs_sid);
#endif
#ifdef CONFIG_OPD2413_UFS_NOP
	if (current_el() == 1 && original_ufs_sid == 0x60) {
		const struct opd_ufs_io io = {snapshot_read32, probe_write32, probe_sync, probe_delay, NULL};
		android_handoff_safe = opd2413_ufs_nop(&io, (volatile void *)OPD_DMA_BASE) >= 0;
	} else printf("UFS_NOP skipped: unexpected EL or stream ID\n");
#endif
#ifdef CONFIG_OPD2413_UFS_BLK_RAMDISK
	android_handoff_safe = opd2413_ufs_blk_start_ramdisk() == 0;
#elif defined(CONFIG_OPD2413_UFS_BLK)
	if (current_el() == 1 && original_ufs_sid == 0x60) {
		/* Static: the block devices are read long after this frame is gone. */
		static const struct opd_ufs_io io = {snapshot_read32, probe_write32, probe_sync, probe_delay, NULL};
		android_handoff_safe = opd2413_ufs_blk_start(&io) >= 0;
	} else printf("UFS_READ skipped: unexpected EL or stream ID (block devices)\n");
#elif defined(CONFIG_OPD2413_UFS_READ)
	if (current_el() == 1 && original_ufs_sid == 0x60) {
		const struct opd_ufs_io io = {snapshot_read32, probe_write32, probe_sync, probe_delay, NULL};
		android_handoff_safe = opd2413_ufs_survey(&io, (volatile void *)OPD_DMA_BASE, NULL,
			IS_ENABLED(CONFIG_OPD2413_UFS_WAKE) ? OPD_UFS_WAKE : 0) >= 0;
	} else printf("UFS_READ skipped: unexpected EL or stream ID\n");
#endif
#ifdef CONFIG_OPD2413_UFS_TRACE
	printf("OPD2413_LATE_END sp=%llx daif=%llx top=%llx reloc=%llx cntpct=%llx\n", read_sp(), read_daif(),
	       (unsigned long long)gd->start_addr_sp, (unsigned long long)gd->relocaddr, read_cntpct());
	trace_budget = 12;
#endif
#ifdef CONFIG_OPD2413_UFS_LINK_CHECK
	if (current_el() == 1) {
		const struct opd_ufs_io io = {snapshot_read32, probe_write32, probe_sync, probe_delay, NULL};
		android_handoff_safe = opd2413_ufs_link_check(&io, NULL) >= 0;
	} else printf("UFS_LINK skipped: unexpected EL\n");
#endif
	return 0;
}

/* Observer-only return to the already loaded ABL kernel, never an Android
 * image loaded by U-Boot. Keep its original DT/ramdisk/TEE handoff intact. */
static int do_opd_android(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
#if defined(CONFIG_OPD2413_UFS_NOP) || defined(CONFIG_OPD2413_UFS_LINK_CHECK) || defined(CONFIG_OPD2413_UFS_READ)
	if (!android_handoff_safe) {
		printf("observer: controller state uncertain; firmware reset required\n");
		return CMD_RET_FAILURE;
	}
#endif
#ifdef CONFIG_OPD2413_UFS_BLK
	/* Restore the device power mode, ABL's queue and Hibern8 first. */
	if (android_handoff_safe && opd2413_ufs_blk_stop()) android_handoff_safe = 0;
	if (!android_handoff_safe) {
		printf("observer: UFS block session not restored; firmware reset required\n");
		return CMD_RET_FAILURE;
	}
#endif
	const void *fdt = (const void *)get_prev_bl_fdt_addr();
	const volatile u32 *image = (const volatile u32 *)0xa8000000ULL;
	if (fdt_check_header(fdt) ||
	    crc32(0, fdt, fdt_totalsize(fdt)) != original_fdt_crc ||
	    *(const volatile u64 *)image != original_kernel_instructions ||
	    image[14] != 0x644d5241 || (image[1] & 0xfc000000U) != 0x14000000U) {
		printf("observer: original ABL handoff changed; refusing Android return\n");
		return CMD_RET_FAILURE;
	}
#ifdef CONFIG_OPD2413_EFI
	if (abl_end && crc32(0, (const void *)OPD_ABL_BASE, abl_end - OPD_ABL_BASE) != abl_crc) {
		printf("observer: ABL kernel/FDT/initrd range changed; refusing Android return\n");
		return CMD_RET_FAILURE;
	}
#endif
	printf("OPD2413_ANDROID_HANDOFF fdt_crc=%08x entry=a8000004\n", original_fdt_crc);
	cleanup_before_linux();
	((void (*)(u64, u64, u64, u64))0xa8000004ULL)((u64)fdt, 0, 0, 0);
	return CMD_RET_FAILURE;
}
U_BOOT_CMD(opd_android, 1, 0, do_opd_android,
	   "return to the original ABL-loaded Android kernel", "");
