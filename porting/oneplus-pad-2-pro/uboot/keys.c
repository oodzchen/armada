// SPDX-License-Identifier: GPL-2.0+
/* Read-only OPD2413 keys over the SPMI PMIC arbiter v7 observer channel. */
#include <stdio.h>
#include "opd2413_keys.h"

#define ARB_VERSION 0x0000
#define ARB_FEATURES 0x0004
#define ARB_APID_MAP(n) (0x2000 + 4 * (n))
#define ARB_MAX_APIDS 1024U
#define CHAN_CMD 0x00
#define CHAN_STATUS 0x08
#define CHAN_RDATA0 0x18
#define OP_EXT_READL 1U
#define PPID_PON 0x013   /* SID 0, 0x13xx */
#define PPID_GPIO6 0x18d /* SID 1, 0x8dxx */

static uint32_t rd(const struct opd_keys *k, uint64_t addr) { return k->io.read32(addr, k->io.ctx); }

int opd2413_keys_write_allowed(uint64_t addr, uint32_t value)
{
	return addr >= OPD_SPMI_OBS && addr < OPD_SPMI_OBS + OPD_SPMI_OBS_SIZE &&
	       (addr - OPD_SPMI_OBS) % 0x20 == CHAN_CMD && (value & ~0xff0U) == OP_EXT_READL << 27;
}

static int find_apid(const struct opd_keys *k, uint32_t count, uint16_t ppid, uint16_t *apid)
{
	for (uint32_t i = 0; i < count; ++i)
		if (((rd(k, OPD_SPMI_CORE + ARB_APID_MAP(i)) >> 8) & 0xfff) == ppid) {
			*apid = (uint16_t)i;
			return 0;
		}
	return -1;
}

static int read8(const struct opd_keys *k, uint16_t apid, uint16_t addr, uint8_t *value)
{
	uint64_t chan = OPD_SPMI_OBS + 0x20ULL * apid;
	uint32_t cmd = OP_EXT_READL << 27 | (uint32_t)(addr & 0xff) << 4; /* byte count 1 */
	if (!opd2413_keys_write_allowed(chan + CHAN_CMD, cmd)) return -1;
	k->io.write32(chan + CHAN_CMD, cmd, k->io.ctx);
	for (unsigned i = 0; i < 1000; ++i) {
		uint32_t status = rd(k, chan + CHAN_STATUS);
		if (status & 1) {
			if (status & 0xe) return -1; /* failed, denied or dropped */
			*value = (uint8_t)rd(k, chan + CHAN_RDATA0);
			return 0;
		}
		k->io.delay_us(1, k->io.ctx);
	}
	return -1;
}

int opd2413_keys_sample(struct opd_keys *k, uint8_t pressed[OPD_KEYS])
{
	uint8_t pon, gpio;
	if (!k->ready || read8(k, k->apid_pon, 0x1310, &pon) || read8(k, k->apid_gpio, 0x8d10, &gpio))
		return -1;
	pressed[OPD_KEY_UP] = (pon >> 6) & 1;
	pressed[OPD_KEY_POWER] = (pon >> 7) & 1;
	pressed[OPD_KEY_DOWN] = !(gpio & 1);
	return 0;
}

int opd2413_keys_init(struct opd_keys *k, const struct opd_ufs_io *io)
{
	*k = (struct opd_keys){.io = *io};
	uint32_t version = rd(k, OPD_SPMI_CORE + ARB_VERSION);
	uint32_t count = rd(k, OPD_SPMI_CORE + ARB_FEATURES) & 0x7ff;
	if (version >> 28 != 7 || !count || count > ARB_MAX_APIDS ||
	    find_apid(k, count, PPID_PON, &k->apid_pon) || find_apid(k, count, PPID_GPIO6, &k->apid_gpio)) {
		printf("OPD2413_KEYS unavailable: arbiter %08x apids %u\n", version, count);
		return -1;
	}
	k->ready = 1;
	if (opd2413_keys_sample(k, k->stable)) {
		k->ready = 0;
		printf("OPD2413_KEYS unavailable: SPMI read failed\n");
		return -1;
	}
	for (unsigned i = 0; i < OPD_KEYS; ++i) k->candidate[i] = k->stable[i];
	printf("OPD2413_KEYS ready arbiter %08x apid pon=%u gpio=%u held up=%u down=%u power=%u\n", version,
	       k->apid_pon, k->apid_gpio, k->stable[OPD_KEY_UP], k->stable[OPD_KEY_DOWN], k->stable[OPD_KEY_POWER]);
	return 0;
}

int opd2413_keys_poll(struct opd_keys *k)
{
	uint8_t raw[OPD_KEYS];
	int edges = 0;
	if (opd2413_keys_sample(k, raw)) return -1;
	for (unsigned i = 0; i < OPD_KEYS; ++i) {
		if (raw[i] == k->stable[i]) { k->candidate[i] = raw[i]; k->samples[i] = 0; continue; }
		if (raw[i] != k->candidate[i]) { k->candidate[i] = raw[i]; k->samples[i] = 1; continue; }
		if (++k->samples[i] < 2) continue;
		k->stable[i] = raw[i]; k->samples[i] = 0;
		if (raw[i]) edges |= 1 << i;
	}
	return edges;
}
