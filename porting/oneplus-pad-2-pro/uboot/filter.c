// SPDX-License-Identifier: GPL-2.0+
#ifdef OPD2413_HOST_TEST
#include <libfdt.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#define CONFIG_NR_DRAM_BANKS 24
#define SZ_2M (2U << 20)
#else
#include <linux/libfdt.h>
#include <linux/errno.h>
#include <linux/sizes.h>
#include <string.h>
#endif
#include "opd2413_layout.h"
#define FB_BASE 0xfc800000ULL
#define FB_SIZE (3392ULL * 2400 * 4)
#define LOG_BASE 0x880000000ULL
#define LOG_SIZE 0x40000ULL
static uint64_t cell64(const fdt32_t *p)
{
    return ((uint64_t)fdt32_to_cpu(p[0]) << 32) | fdt32_to_cpu(p[1]);
}
/* The filtered DT contains only memory, reservations, PSCI, RAM log and fb. */
int opd2413_filter_fdt(const void *src, void *dest, unsigned capacity)
{
	const fdt32_t *memory;
	int len, parent, ret, node;
	const char *model;
	if (!src || fdt_check_header(src) || fdt_totalsize(src) > SZ_2M ||
	    fdt_check_full(src, fdt_totalsize(src))) return -EINVAL;
	if (fdt_address_cells(src, 0) != 2 || fdt_size_cells(src, 0) != 2) return -EINVAL;
	model = fdt_getprop(src, 0, "model", &len);
	if (!model || len != sizeof("Qualcomm Technologies, Inc. Sun MTP,erhai-O DVT") ||
	    memcmp(model, "Qualcomm Technologies, Inc. Sun MTP,erhai-O DVT", len)) return -EINVAL;
	node = fdt_path_offset(src, "/memory");
	memory = fdt_getprop(src, node, "reg", &len);
	if (!memory || len <= 0 || len % 16 || len / 16 > CONFIG_NR_DRAM_BANKS) return -EINVAL;
	for (int i = 0; i < len / 4; i += 4) {
		struct opd_range r = {cell64(memory + i), cell64(memory + i + 2)};
		if (r.size && !opd_range_valid(r)) return -EINVAL;
	}
#define DO(expr) do { ret = (expr); if (ret) return ret; } while (0)
	DO(fdt_create(dest, capacity));
	for (int i = 0; i < fdt_num_mem_rsv(src); ++i) {
		uint64_t base, size;
		DO(fdt_get_mem_rsv(src, i, &base, &size));
		if (size && !opd_range_valid((struct opd_range){base, size})) return -EINVAL;
		DO(fdt_add_reservemap_entry(dest, base, size));
	}
	DO(fdt_finish_reservemap(dest));
	DO(fdt_begin_node(dest, ""));
	DO(fdt_property_string(dest, "model", "OnePlus Pad 2 Pro U-Boot observer"));
	DO(fdt_property_string(dest, "compatible", "armada,opd2413-observer"));
	DO(fdt_property_u32(dest, "#address-cells", 2));
	DO(fdt_property_u32(dest, "#size-cells", 2));
	DO(fdt_begin_node(dest, "memory"));
	DO(fdt_property_string(dest, "device_type", "memory"));
	DO(fdt_property(dest, "reg", memory, len));
	DO(fdt_end_node(dest));
	DO(fdt_begin_node(dest, "reserved-memory"));
	DO(fdt_property_u32(dest, "#address-cells", 2));
	DO(fdt_property_u32(dest, "#size-cells", 2));
	DO(fdt_property(dest, "ranges", "", 0));
	parent = fdt_path_offset(src, "/reserved-memory");
	if (parent < 0 || fdt_address_cells(src, parent) != 2 || fdt_size_cells(src, parent) != 2) return -EINVAL;
	fdt_for_each_subnode(node, src, parent) {
		const void *reg = fdt_getprop(src, node, "reg", &len);
		/* Dynamic kernel reservations have no allocated address yet. */
		if (!reg) continue;
		if (len <= 0 || len % 16) return -EINVAL;
		for (int i = 0; i < len / 4; i += 4) {
			const fdt32_t *p = (const fdt32_t *)reg + i;
			struct opd_range r = {cell64(p), cell64(p + 2)};
			if (r.size && !opd_range_valid(r)) return -EINVAL;
		}
		DO(fdt_begin_node(dest, fdt_get_name(src, node, NULL)));
		DO(fdt_property(dest, "reg", reg, len));
		DO(fdt_property(dest, "no-map", "", 0));
		DO(fdt_end_node(dest));
	}
	DO(fdt_end_node(dest));
	DO(fdt_begin_node(dest, "psci"));
	DO(fdt_property_string(dest, "compatible", "arm,psci-1.0"));
	DO(fdt_property_string(dest, "method", "smc"));
	DO(fdt_end_node(dest));
	DO(fdt_begin_node(dest, "ramconsole"));
	DO(fdt_property_string(dest, "compatible", "armada,opd2413-ramconsole"));
	fdt64_t log_reg[] = {cpu_to_fdt64(LOG_BASE), cpu_to_fdt64(LOG_SIZE)};
	DO(fdt_property(dest, "reg", log_reg, sizeof(log_reg)));
	DO(fdt_property(dest, "bootph-all", "", 0));
	DO(fdt_end_node(dest));
	DO(fdt_begin_node(dest, "chosen"));
	DO(fdt_property_u32(dest, "#address-cells", 2));
	DO(fdt_property_u32(dest, "#size-cells", 2));
	DO(fdt_property(dest, "ranges", "", 0));
	DO(fdt_property_string(dest, "stdout-path", "/ramconsole"));
	DO(fdt_begin_node(dest, "framebuffer@fc800000"));
	DO(fdt_property_string(dest, "compatible", "simple-framebuffer"));
	fdt64_t fb_reg[] = {cpu_to_fdt64(FB_BASE), cpu_to_fdt64(FB_SIZE)};
	DO(fdt_property(dest, "reg", fb_reg, sizeof(fb_reg)));
	DO(fdt_property_u32(dest, "width", 3392));
	DO(fdt_property_u32(dest, "height", 2400));
	DO(fdt_property_u32(dest, "stride", 13568));
	DO(fdt_property_string(dest, "format", "a8r8g8b8"));
	DO(fdt_property(dest, "bootph-all", "", 0));
	DO(fdt_end_node(dest));
	DO(fdt_end_node(dest));
	DO(fdt_end_node(dest));
	DO(fdt_finish(dest));
	return 0;
#undef DO
}

