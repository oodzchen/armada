/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef OPD2413_KEYS_H
#define OPD2413_KEYS_H
#include <stdint.h>
#include "opd2413_ufs_nop.h"

/*
 * Read-only OPD2413 volume/power keys through the SPMI PMIC arbiter v7
 * (ABL FDT spmi@c42d000: core 0xc400000, observer 0xc440000, EE 0, bus 0),
 * following Linux spmi-pmic-arb. Per the ABL FDT:
 *   Power       PMK8550 (SID 0) PON 0x1300, RT_STS 0x1310 bit 7 (pmk8350-pwrkey)
 *   Volume up   PMK8550 (SID 0) PON 0x1300, RT_STS 0x1310 bit 6 (pmk8350-resin, code 115)
 *   Volume down PM8550 (SID 1) GPIO 6, RT_STS 0x8d10 bit 0, active low (gpio-keys, code 114)
 * The only register write is an EXT_READL command to an EE 0 observer
 * channel; opd2413_keys_write_allowed() is the whitelist for it.
 */
#define OPD_SPMI_CORE 0x0c400000ULL
#define OPD_SPMI_CORE_SIZE 0x3000ULL
#define OPD_SPMI_OBS 0x0c440000ULL /* EE 0: 0x20 bytes per APID */
#define OPD_SPMI_OBS_SIZE 0x8000ULL

enum { OPD_KEY_UP, OPD_KEY_DOWN, OPD_KEY_POWER, OPD_KEYS };

struct opd_keys {
	struct opd_ufs_io io; /* own copy, as with the UFS session */
	uint16_t apid_pon, apid_gpio;
	uint8_t stable[OPD_KEYS], candidate[OPD_KEYS], samples[OPD_KEYS];
	uint8_t ready;
};

/* 0 = ready (initial key state recorded, no events for keys already held);
 * -1 = arbiter or peripherals not as expected (no key input). */
int opd2413_keys_init(struct opd_keys *k, const struct opd_ufs_io *io);
/* Current raw state; 0 = sampled, -1 = SPMI transaction failed. */
int opd2413_keys_sample(struct opd_keys *k, uint8_t pressed[OPD_KEYS]);
/* One debounced poll: bit n set = new press of key n; -1 on a read error. */
int opd2413_keys_poll(struct opd_keys *k);
/* Write whitelist: EXT_READL, one byte, to an EE 0 observer command register. */
int opd2413_keys_write_allowed(uint64_t addr, uint32_t value);
#endif
