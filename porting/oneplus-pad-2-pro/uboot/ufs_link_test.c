// SPDX-License-Identifier: GPL-2.0+
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "opd2413_ufs_link.h"
struct mock { uint32_t r[1024]; unsigned writes, gets, timeout; };
static uint32_t read32(uint64_t addr, void *ctx)
{
	struct mock *s = ctx;
	if (addr == OPD_GCC_UFS_BASE + 4) return 0x80000000;
	if (addr == OPD_GCC_UFS_BASE + 0x28) return 0x88000003;
	assert(addr >= OPD_UFS_BASE && addr < OPD_UFS_BASE + 4096 && !(addr & 3));
	return s->r[(addr - OPD_UFS_BASE)/4];
}
static void write32(uint64_t addr, uint32_t val, void *ctx)
{
	struct mock *s = ctx;
	unsigned off = addr - OPD_UFS_BASE;
	assert(off == 0x20 || off == 0x90 || off == 0x94 || off == 0x98 || off == 0x9c);
	++s->writes;
	if (off == 0x20) { assert(val == 0x400); s->r[off/4] &= ~val; return; }
	s->r[off/4] = val;
	if (off == 0x90) {
		assert(val == 1 && !(s->r[0x20/4] & 0x400));
		unsigned attr = s->r[0x94/4];
		assert(attr == 0x00410000 || attr == 0x00410001 || attr == 0x15710000);
		++s->gets;
		if (s->timeout) return;
		s->r[0x98/4] = 0; s->r[0x9c/4] = attr == 0x15710000 ? 0x11 : 1;
		s->r[0x20/4] |= 0x400;
	}
}
static void sync_io(void *ctx) { (void)ctx; }
static void delay_us(unsigned us, void *ctx) { (void)ctx; assert(us == 50); }
static void init(struct mock *s, unsigned is)
{
	memset(s, 0, sizeof(*s));
	s->r[0x34/4] = 1; s->r[0x30/4] = 0x10f; s->r[0x20/4] = is;
	s->r[0x94/4] = 0x11223344; s->r[0x98/4] = 0x55667788; s->r[0x9c/4] = 0x99aabbcc;
}
int main(void)
{
	struct mock s;
	struct opd_ufs_io io = {read32, write32, sync_io, delay_us, &s};
	for (unsigned is = 0; is <= 0x400; is += 0x400) {
		uint32_t states[3];
		init(&s, is); assert(!opd2413_ufs_link_check(&io, states)); assert(s.gets == 3);
		assert(states[0] == 1 && states[1] == 1 && states[2] == 0x11);
		assert(s.r[0x20/4] == is && s.r[0x94/4] == 0x11223344 &&
		       s.r[0x98/4] == 0x55667788 && s.r[0x9c/4] == 0x99aabbcc);
	}
	init(&s, 0x400); s.r[0x58/4] = 1;
	assert(opd2413_ufs_link_check(&io, NULL) == 1 && !s.writes);
	init(&s, 0x400); s.timeout = 1;
	assert(opd2413_ufs_link_check(&io, NULL) == -1 && s.gets == 1);
	puts("PASS: DME_GET only, idle guard, argument/IS restoration, timeout");
	return 0;
}
