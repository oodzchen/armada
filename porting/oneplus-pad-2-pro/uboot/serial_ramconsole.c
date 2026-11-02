// SPDX-License-Identifier: GPL-2.0+
/* Mirror output into the same ecc=0 ramoops console used by kShimC T14. */
#include <cpu_func.h>
#include <dm.h>
#include <serial.h>
#include <asm/io.h>

#define LOG_BASE 0x880000000ULL
#include "opd2413_ramconsole_format.h"
static int ramconsole_putc(struct udevice *dev, const char ch)
{
    volatile u32 *h = (volatile u32 *)LOG_BASE;
    if (!opd_log_putc(h, (unsigned char)ch)) return 0;
    __asm__ volatile("dsb sy" ::: "memory");
    if (dcache_status()) {
        u32 length = h[2] - OPD_LOG_LAST - 8;
        flush_dcache_range(LOG_BASE, LOG_BASE + 64);
        for (unsigned copy = 0; copy < OPD_LOG_COPIES; ++copy) {
            ulong base = LOG_BASE + 12 + copy * OPD_LOG_STRIDE;
            flush_dcache_range(base & ~63UL, (base & ~63UL) + 64);
            ulong line = (base + 8 + length - 1) & ~63UL;
            flush_dcache_range(line, line + 64);
        }
    }
    return 0;
}
static int ramconsole_getc(struct udevice *dev) { return -EAGAIN; }
static int ramconsole_pending(struct udevice *dev, bool input) { return 0; }
static int ramconsole_setbrg(struct udevice *dev, int baudrate) { return 0; }
static int ramconsole_probe(struct udevice *dev)
{
    /* Redundant copies survive sparse bit corruption across firmware reset. */
    opd_log_init((volatile u32 *)LOG_BASE);
	return 0;
}
static const struct dm_serial_ops ramconsole_ops = {
	.putc=ramconsole_putc, .getc=ramconsole_getc,
	.pending=ramconsole_pending, .setbrg=ramconsole_setbrg,
};
static const struct udevice_id ramconsole_ids[] = {
	{ .compatible="armada,opd2413-ramconsole" }, { }
};
U_BOOT_DRIVER(opd2413_ramconsole) = {
	.name="opd2413_ramconsole", .id=UCLASS_SERIAL,
	.of_match=ramconsole_ids, .probe=ramconsole_probe, .ops=&ramconsole_ops,
	.flags=DM_FLAG_PRE_RELOC,
};
