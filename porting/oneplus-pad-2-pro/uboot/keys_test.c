// SPDX-License-Identifier: GPL-2.0+
/* Host test for keys.c against a model of the SPMI arbiter v7 observer channel. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "opd2413_keys.h"

struct arb {
	uint32_t version, count, map[1024];
	uint8_t pmic[2][0x10000];
	uint32_t status[1024], rdata[1024];
	unsigned writes, bad_writes, mode; /* 1 = never done, 2 = denied */
};

static uint32_t read32(uint64_t addr, void *ctx)
{
	struct arb *a = ctx;
	if (addr == OPD_SPMI_CORE) return a->version;
	if (addr == OPD_SPMI_CORE + 4) return a->count | 0xf800; /* upper bits are not the count */
	if (addr >= OPD_SPMI_CORE + 0x2000 && addr < OPD_SPMI_CORE + 0x3000) return a->map[(addr - OPD_SPMI_CORE - 0x2000) / 4];
	if (addr >= OPD_SPMI_OBS && addr < OPD_SPMI_OBS + OPD_SPMI_OBS_SIZE) {
		unsigned apid = (addr - OPD_SPMI_OBS) / 0x20, reg = (addr - OPD_SPMI_OBS) % 0x20;
		if (reg == 0x08) return a->status[apid];
		if (reg == 0x18) return a->rdata[apid];
	}
	assert(!"unexpected read");
	return 0;
}

static void write32(uint64_t addr, uint32_t value, void *ctx)
{
	struct arb *a = ctx;
	++a->writes;
	if (!opd2413_keys_write_allowed(addr, value)) { ++a->bad_writes; return; }
	unsigned apid = (addr - OPD_SPMI_OBS) / 0x20;
	uint16_t ppid = (a->map[apid] >> 8) & 0xfff;
	assert((value >> 27) == 1 && (value & 7) == 0); /* EXT_READL, one byte */
	uint16_t reg = (uint16_t)((ppid & 0xff) << 8 | ((value >> 4) & 0xff));
	a->status[apid] = a->mode == 1 ? 0 : a->mode == 2 ? 5 : 1;
	a->rdata[apid] = 0xffffff00U | a->pmic[ppid >> 8][reg];
}

static void sync_io(void *ctx) { (void)ctx; }
static void delay_us(unsigned us, void *ctx) { (void)us; (void)ctx; }

static void setup(struct arb *a)
{
	memset(a, 0, sizeof(*a));
	a->version = 0x70020000; a->count = 300;
	for (unsigned i = 0; i < a->count; ++i) a->map[i] = (0x0ffU << 8) | 0x01000000; /* unrelated */
	a->map[17] = 0x013 << 8;   /* PON, SID 0 */
	a->map[250] = 0x18d << 8;  /* GPIO6, SID 1 */
	a->map[400] = 0x013 << 8;  /* beyond the bus-0 count: must be ignored */
	a->pmic[1][0x8d10] = 1;    /* volume down released (active low) */
}

int main(void)
{
	static struct arb a;
	struct opd_keys k;
	struct opd_ufs_io io = {read32, write32, sync_io, delay_us, &a};

	/* Whitelist: only EXT_READL/one byte to an EE 0 observer command register. */
	assert(opd2413_keys_write_allowed(OPD_SPMI_OBS + 0x20 * 5, 1U << 27 | 0x10 << 4));
	assert(!opd2413_keys_write_allowed(OPD_SPMI_OBS + 0x20 * 5 + 8, 1U << 27));
	assert(!opd2413_keys_write_allowed(OPD_SPMI_OBS + 0x20 * 5, 0U << 27));      /* EXT_WRITEL */
	assert(!opd2413_keys_write_allowed(OPD_SPMI_OBS + 0x20 * 5, 1U << 27 | 1));  /* two bytes */
	assert(!opd2413_keys_write_allowed(OPD_SPMI_OBS + OPD_SPMI_OBS_SIZE, 1U << 27));
	assert(!opd2413_keys_write_allowed(OPD_SPMI_CORE, 1U << 27));

	setup(&a);
	assert(!opd2413_keys_init(&k, &io) && k.apid_pon == 17 && k.apid_gpio == 250);
	assert(opd2413_keys_poll(&k) == 0);
	/* Press volume up: reported once after two equal samples. */
	a.pmic[0][0x1310] = 0x40;
	assert(opd2413_keys_poll(&k) == 0 && opd2413_keys_poll(&k) == 1 << OPD_KEY_UP && opd2413_keys_poll(&k) == 0);
	/* A one-sample glitch is ignored. */
	a.pmic[1][0x8d10] = 0;
	assert(opd2413_keys_poll(&k) == 0);
	a.pmic[1][0x8d10] = 1;
	assert(opd2413_keys_poll(&k) == 0 && opd2413_keys_poll(&k) == 0);
	/* Volume down (active low) and power. */
	a.pmic[0][0x1310] = 0;
	assert(opd2413_keys_poll(&k) == 0 && opd2413_keys_poll(&k) == 0); /* release: no event */
	a.pmic[1][0x8d10] = 0; a.pmic[0][0x1310] = 0x80;
	assert(opd2413_keys_poll(&k) == 0 && opd2413_keys_poll(&k) == (1 << OPD_KEY_DOWN | 1 << OPD_KEY_POWER));
	assert(!a.bad_writes);

	/* A key held at init (the kShimC menu confirm) is not a new press. */
	setup(&a); a.pmic[0][0x1310] = 0x80;
	assert(!opd2413_keys_init(&k, &io) && k.stable[OPD_KEY_POWER]);
	assert(opd2413_keys_poll(&k) == 0 && opd2413_keys_poll(&k) == 0);
	a.pmic[0][0x1310] = 0;
	assert(opd2413_keys_poll(&k) == 0 && opd2413_keys_poll(&k) == 0);
	a.pmic[0][0x1310] = 0x80;
	assert(opd2413_keys_poll(&k) == 0 && opd2413_keys_poll(&k) == 1 << OPD_KEY_POWER);

	/* Unexpected arbiter or missing peripherals: no input, no writes. */
	setup(&a); a.version = 0x50000000;
	assert(opd2413_keys_init(&k, &io) == -1 && !a.writes && opd2413_keys_poll(&k) == -1);
	setup(&a); a.map[250] = 0;
	assert(opd2413_keys_init(&k, &io) == -1 && !a.writes);
	setup(&a); a.count = 200; /* GPIO6 APID outside bus 0 */
	assert(opd2413_keys_init(&k, &io) == -1);
	/* Transaction never completes or is denied: reported as an error. */
	setup(&a); a.mode = 1;
	assert(opd2413_keys_init(&k, &io) == -1);
	setup(&a); assert(!opd2413_keys_init(&k, &io)); a.mode = 2;
	assert(opd2413_keys_poll(&k) == -1);
	puts("PASS: SPMI v7 APID lookup, EXT_READL-only writes, debounce, held keys, faults");
	return 0;
}
