// SPDX-License-Identifier: GPL-2.0+
/* Host model of the legacy UFS doorbell executing only SCSI reads against
 * synthetic GPT disks. The model checks every descriptor independently of
 * the code under test and aborts on any opcode outside the read-only set.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "opd2413_ufs_read.h"
#include "opd2413_smmu_snapshot.h"

#define BLK 4096U
#define DATA 0x2000U

struct disk { uint8_t lun; uint32_t block; uint64_t last; uint8_t *primary, *backup; };
struct mock {
	uint32_t host[1024], smmu[OPD_SMMU_SIZE / 4];
	unsigned char *dma;
	unsigned writes, commands, hibernated, pending, fail_at, mode, ua_pending, uic, target_failed;
	unsigned opcodes[256];
	struct disk disk[3]; unsigned disks;
};

static uint32_t rd_be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void wr_be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void wr_le32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void wr_le64(uint8_t *p, uint64_t v) { wr_le32(p, (uint32_t)v); wr_le32(p + 4, (uint32_t)(v >> 32)); }

static void header(uint8_t *h, uint64_t self, uint64_t alt, uint64_t entries, uint32_t entries_crc,
		   uint64_t disk_last)
{
	memset(h, 0, BLK);
	memcpy(h, "EFI PART", 8);
	wr_le32(h + 8, 0x10000); wr_le32(h + 12, 92);
	wr_le64(h + 24, self); wr_le64(h + 32, alt);
	wr_le64(h + 40, 6); wr_le64(h + 48, disk_last - 5);
	wr_le64(h + 72, entries); wr_le32(h + 80, 128); wr_le32(h + 84, 128);
	wr_le32(h + 88, entries_crc);
	wr_le32(h + 16, opd2413_crc32(0, h, 92));
}
static void make_disk(struct disk *d, uint8_t lun, uint32_t block, uint64_t last)
{
	*d = (struct disk){lun, block, last, calloc(6, BLK), calloc(5, BLK)};
	assert(d->primary && d->backup);
	d->primary[510] = 0x55; d->primary[511] = 0xaa;
	uint8_t *e = d->primary + 2 * BLK;
	const char *names[] = {"persist", "super", "userdata"};
	for (unsigned i = 0; i < 3; ++i) {
		uint8_t *x = e + i * 128;
		x[0] = 0x10 + i; x[16] = 0x20 + i;
		wr_le64(x + 32, 6 + i * 100); wr_le64(x + 40, 105 + i * 100);
		for (unsigned c = 0; names[i][c]; ++c) x[56 + 2 * c] = names[i][c];
	}
	uint32_t crc = opd2413_crc32(0, e, 128 * 128);
	header(d->primary + BLK, 1, last, 2, crc, last);
	memcpy(d->backup, e, 4 * BLK);
	header(d->backup + 4 * BLK, last, 1, last - 4, crc, last);
}

static struct disk *find(struct mock *s, uint8_t lun)
{
	for (unsigned i = 0; i < s->disks; ++i) if (s->disk[i].lun == lun) return &s->disk[i];
	return NULL;
}

/* Execute the submitted UTRD; returns via the response UPIU and data. */
static void execute(struct mock *s)
{
	uint32_t *utrd = (uint32_t *)s->dma;
	uint8_t *cmd = s->dma + 0x1000, *rsp = s->dma + 0x1200, *data = s->dma + DATA;
	uint32_t xfer = rd_be32(cmd + 12);
	uint8_t op = cmd[16], lun = cmd[2];
	/* Independent wire-format and read-only checks. */
	assert(utrd[0] == (xfer ? 0x15000000U : 0x11000000U) && utrd[2] == 0xf);
	assert(utrd[4] == OPD_DMA_BASE + 0x1000 && !utrd[5] && utrd[6] == 0x00800080);
	assert(utrd[7] == (xfer ? 0x01000001U : 0x01000000U));
	assert(cmd[0] == 0x01 && cmd[1] == (xfer ? 0x40 : 0) && !cmd[3] && !cmd[4]);
	assert(op == 0x00 || op == 0xa0 || op == 0x25 || op == 0x9e || op == 0x28);
	if (xfer) {
		uint32_t *prd = (uint32_t *)(s->dma + 0x1400);
		assert(prd[0] == OPD_DMA_BASE + DATA && !prd[1] && !prd[2] && prd[3] == ((xfer - 1) | 3));
		assert(DATA + xfer <= OPD_DMA_SIZE);
		for (unsigned i = 0; i < xfer; ++i) assert(data[i] == 0xa5);
	}
	for (unsigned i = 0; i < 512; ++i) assert(rsp[i] == 0xa5);
	++s->opcodes[op]; ++s->commands;
	memset(rsp, 0, 512);
	rsp[0] = s->mode == 3 && s->commands == s->fail_at ? 0x3f : 0x21;
	utrd[2] = s->mode == 2 && s->commands == s->fail_at ? 1 : 0;
	uint8_t status = 0, key = 0, asc = 0;
	uint32_t residual = 0;
	struct disk *d = find(s, lun);
	if (op == 0xa0) {
		assert(lun == 0x81);
		if (s->mode == 5) { status = 2; key = 5; asc = 0x20; }
		else {
			memset(data, 0, xfer);
			wr_be32(data, 8 * (s->disks + 2));
			uint8_t *e = data + 8;
			for (unsigned i = 0; i < s->disks; ++i, e += 8) e[1] = s->disk[i].lun;
			e[0] = 0xc1; e[1] = 0x50; e += 8; /* well-known UFS Device W-LUN */
			e[0] = 0; e[1] = 0x81;            /* reported as W-LUN form, skipped */
		}
	} else if (!d) {
		status = 2; key = 5; asc = 0x25;
	} else if (op == 0x00) {
		if (s->ua_pending && d->lun == 0) { --s->ua_pending; status = 2; key = 6; asc = 0x29; }
	} else if (op == 0x25) {
		memset(data, 0, 8);
		wr_be32(data, d->last > 0xffffffffU ? 0xffffffffU : (uint32_t)d->last);
		wr_be32(data + 4, d->block);
	} else if (op == 0x9e) {
		assert(cmd[17] == 0x10);
		memset(data, 0, 32);
		wr_be32(data, (uint32_t)(d->last >> 32)); wr_be32(data + 4, (uint32_t)d->last);
		wr_be32(data + 8, d->block);
	} else {
		uint32_t lba = rd_be32(cmd + 18), blocks = cmd[23] << 8 | cmd[24];
		assert(d->block == BLK && xfer == blocks * BLK);
		for (uint32_t b = 0; b < blocks; ++b) {
			uint8_t *src = NULL;
			if (lba + b < 6) src = d->primary + (lba + b) * BLK;
			else if (lba + b >= d->last - 4 && lba + b <= d->last) src = d->backup + (lba + b - (d->last - 4)) * BLK;
			assert(src); /* only GPT blocks are ever requested */
			memcpy(data + b * BLK, src, BLK);
		}
		if (s->mode == 4 && s->commands == s->fail_at) residual = xfer + 1;
	}
	rsp[6] = 0; rsp[7] = status;
	/* Target Failure: REPORT LUNS (mode 6) or LUN 0's first READ(10) (mode 7). */
	if ((s->mode == 6 && op == 0xa0) || (s->mode == 7 && op == 0x28 && lun == 0 && !s->target_failed)) {
		rsp[6] = 1; rsp[7] = 0; status = 0; s->target_failed = 1;
	}
	wr_be32(rsp + 12, residual);
	if (status) { rsp[33] = 18; rsp[34] = 0x70; rsp[36] = key; rsp[46] = asc; }
	s->host[0x58/4] = 0;
	s->host[0x20/4] |= 1;
}

static uint32_t read32(uint64_t addr, void *ctx)
{
	struct mock *s = ctx;
	if (addr == OPD_GCC_UFS_BASE) return 0;
	if (addr == OPD_GCC_UFS_BASE + 4 || addr == OPD_GCC_MEM_BASE) return 0x80000000U;
	if (addr == OPD_GCC_UFS_BASE + 0x28) return 0x88000003;
	if (addr >= OPD_SMMU_BASE && addr < OPD_SMMU_BASE + OPD_SMMU_SIZE)
		return s->smmu[(addr - OPD_SMMU_BASE)/4];
	assert(addr >= OPD_UFS_BASE && addr < OPD_UFS_BASE + 4096 && !(addr & 3));
	if (addr == OPD_UFS_BASE + 0x58 && s->pending && !(s->mode == 1 && s->commands + 1 == s->fail_at))
		if (!--s->pending) execute(s);
	return s->host[(addr - OPD_UFS_BASE)/4];
}
static void write32(uint64_t addr, uint32_t value, void *ctx)
{
	struct mock *s = ctx;
	assert(addr >= OPD_UFS_BASE && addr < OPD_UFS_BASE + 4096);
	unsigned off = addr - OPD_UFS_BASE;
	assert(off == 0x20 || off == 0x50 || off == 0x54 || off == 0x58 || off == 0x60 ||
	       off == 0x90 || off == 0x94 || off == 0x98 || off == 0x9c);
	++s->writes;
	if (off == 0x50 || off == 0x54) assert(!s->host[0x58/4] && !s->host[0x60/4]);
	if (off == 0x20) {
		assert(value == 1 || value == 0x400 || value == 0x20 || value == 0x40);
		s->host[off/4] &= ~value; return;
	}
	if (off == 0x90) {
		assert(value == 1 || value == 0x17 || value == 0x18);
		assert(!(s->host[0x20/4] & 0x400));
		++s->uic; s->host[0x98/4] = 0;
		if (value == 1) {
			unsigned attr = s->host[0x94/4];
			s->host[0x9c/4] = attr == 0x15710000 ? 0x11 : (s->hibernated ? 1 : 3);
		} else {
			assert(!s->host[0x58/4]);
			s->hibernated = value == 0x17;
			s->host[0x20/4] |= value == 0x17 ? 0x40 : 0x20;
		}
		s->host[0x20/4] |= 0x400;
	}
	if (off == 0x58) {
		assert(value == 1 && !s->pending && s->host[0x60/4] == 1 && !s->hibernated);
		assert(s->host[0x50/4] == OPD_DMA_BASE && !s->host[0x54/4]);
		assert(!(s->host[0x20/4] & 1)); /* previous completion was cleared */
		s->pending = 2;
	}
	s->host[off/4] = value;
}
static void barrier(void *ctx) { (void)ctx; }
static void delay_us(unsigned us, void *ctx) { (void)ctx; assert(us == 50); }
static void init(struct mock *s, unsigned char *dma)
{
	memset(s->host, 0, sizeof(s->host)); memset(s->smmu, 0, sizeof(s->smmu));
	memset(s->opcodes, 0, sizeof(s->opcodes));
	s->dma = dma; s->hibernated = 1; s->writes = s->commands = s->pending = s->uic = 0;
	s->fail_at = s->mode = 0; s->ua_pending = 1; s->target_failed = 0;
	s->host[0] = 0xd5c7031f; s->host[0x08/4] = 0x400;
	s->host[0xe4/4] = 0x60010000; s->host[0x34/4] = 1;
	s->host[0x30/4] = 0x10f; s->host[0x20/4] = 0x400;
	s->host[0x50/4] = 0xd6524000; s->host[0x60/4] = 1;
	s->host[0x90/4] = 0x17; s->host[0x9c/4] = 2;
	s->smmu[0x20/4] = 0x4c017e84; s->smmu[0x24/4] = 0x60000053;
	s->smmu[0x800/4] = 0x80000060; s->smmu[0x1000/4] = 0x0001f000;
	s->smmu[0x1800/4] = 1; s->smmu[0x80000/4] = 0x0a9f00e0;
}
static void restored(struct mock *s)
{
	assert(s->host[0x50/4] == 0xd6524000 && !s->host[0x54/4] && s->host[0x60/4] == 1);
	assert(s->host[0x20/4] == 0x400 && s->hibernated && s->host[0x90/4] == 0x17);
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	/* CRC-32 check value of the standard test vector. */
	assert(opd2413_crc32(0, "123456789", 9) == 0xcbf43926U);
	/* The whitelist itself, independent of submission. */
	uint8_t cdb[16] = {0};
	assert(opd2413_ufs_read_allowed(cdb, 6, 0) && !opd2413_ufs_read_allowed(cdb, 6, 8));
	cdb[0] = 0x2a; assert(!opd2413_ufs_read_allowed(cdb, 10, BLK)); /* WRITE(10) */
	cdb[0] = 0x35; assert(!opd2413_ufs_read_allowed(cdb, 10, 0));   /* SYNCHRONIZE CACHE */
	cdb[0] = 0x42; assert(!opd2413_ufs_read_allowed(cdb, 10, 0));   /* UNMAP */
	cdb[0] = 0x3b; assert(!opd2413_ufs_read_allowed(cdb, 10, BLK)); /* WRITE BUFFER */
	cdb[0] = 0x28; cdb[8] = 1; assert(opd2413_ufs_read_allowed(cdb, 10, BLK));
	cdb[1] = 0x08; assert(!opd2413_ufs_read_allowed(cdb, 10, BLK)); /* FUA */
	cdb[1] = 0; cdb[8] = 15; assert(!opd2413_ufs_read_allowed(cdb, 10, 15 * BLK)); /* > window */
	cdb[0] = 0x9e; cdb[1] = 0x11; assert(!opd2413_ufs_read_allowed(cdb, 16, 32));

	struct mock *s = calloc(1, sizeof(*s)); assert(s);
	make_disk(&s->disk[0], 0, BLK, 1000);
	make_disk(&s->disk[1], 1, BLK, 0x100000000ULL); /* needs READ CAPACITY(16) */
	make_disk(&s->disk[2], 2, 512, 4000);           /* unsupported block size */
	s->disks = 3;
	unsigned char *buffer = malloc(OPD_DMA_SIZE + 32); assert(buffer);
	memset(buffer, 0x5a, OPD_DMA_SIZE + 32);
	unsigned char *dma = buffer + 16;
	struct opd_ufs_io io = {read32, write32, barrier, delay_us, s};
	struct opd_ufs_read_result r;

	init(s, dma);
	assert(!opd2413_ufs_read_gpt(&io, dma, &r));
	restored(s);
	assert(r.luns == 3 && s->opcodes[0xa0] == 1 && s->opcodes[0x9e] == 1);
	assert(s->opcodes[0x00] == 4); /* UNIT ATTENTION on LUN 0 retried once */
	assert(r.lun[0].capacity_ok && r.lun[0].primary_ok && r.lun[0].backup_ok);
	assert(r.lun[0].last_lba == 1000 && r.lun[0].partitions == 3);
	for (unsigned b = 0; b < 6; ++b)
		assert(r.lun[0].primary_crc[b] == opd2413_crc32(0, s->disk[0].primary + b * BLK, BLK));
	for (unsigned b = 0; b < 5; ++b)
		assert(r.lun[0].backup_crc[b] == opd2413_crc32(0, s->disk[0].backup + b * BLK, BLK));
	assert(r.lun[1].capacity_ok && r.lun[1].last_lba == 0x100000000ULL && !r.lun[1].primary_ok);
	assert(r.lun[2].capacity_ok && r.lun[2].block == 512 && !r.lun[2].primary_ok);
	for (unsigned i = 0; i < 16; ++i) assert(buffer[i] == 0x5a && buffer[16 + OPD_DMA_SIZE + i] == 0x5a);

	/* REPORT LUNS rejected: still a clean survey, probing LUN 0-7. */
	init(s, dma); s->mode = 5;
	assert(!opd2413_ufs_read_gpt(&io, dma, &r));
	restored(s); assert(r.luns == 8 && r.lun[0].primary_ok && r.lun[0].backup_ok);
	assert(r.lun[1].capacity_ok && r.lun[2].capacity_ok && !r.lun[3].capacity_ok && !r.lun[7].capacity_ok);
	/* REPORT LUNS Target Failure (as seen on OPD2413): completed, so the
	 * state is known; fall back to probing and finish cleanly. */
	init(s, dma); s->mode = 6;
	assert(!opd2413_ufs_read_gpt(&io, dma, &r));
	restored(s); assert(r.luns == 8 && r.lun[0].primary_ok && r.lun[0].partitions == 3);
	/* A Target Failure on one READ only marks that LUN. */
	init(s, dma); s->mode = 7;
	assert(!opd2413_ufs_read_gpt(&io, dma, &r));
	restored(s); assert(r.lun[0].capacity_ok && !r.lun[0].primary_ok && r.luns == 3);

	/* A corrupt primary header is reported, not trusted, and not fatal. */
	init(s, dma); s->disk[0].primary[BLK + 16] ^= 1;
	assert(!opd2413_ufs_read_gpt(&io, dma, &r));
	restored(s); assert(!r.lun[0].primary_ok && !r.lun[0].partitions);
	s->disk[0].primary[BLK + 16] ^= 1;
	/* Corrupt partition entries fail the entries CRC. */
	init(s, dma); s->disk[0].primary[2 * BLK + 60] ^= 1;
	assert(!opd2413_ufs_read_gpt(&io, dma, &r));
	restored(s); assert(!r.lun[0].primary_ok);
	s->disk[0].primary[2 * BLK + 60] ^= 1;

	/* Transport faults at every command position of a clean run: never a
	 * clean return. READ residual faults apply to READ(10) commands only. */
	init(s, dma);
	assert(!opd2413_ufs_read_gpt(&io, dma, &r));
	const unsigned total = s->commands;
	assert(total == 11);
	unsigned residual_failures = 0;
	for (unsigned mode = 1; mode <= 4; ++mode) {
		for (unsigned at = 1; at <= total; ++at) {
			init(s, dma); s->mode = mode; s->fail_at = at;
			int ret = opd2413_ufs_read_gpt(&io, dma, &r);
			if (mode == 4) {
				assert(ret == 0 || ret == -1);
				if (ret == -1) { assert(!s->host[0x60/4]); ++residual_failures; }
			} else {
				assert(ret == -1 && !s->host[0x60/4]);
			}
		}
	}
	assert(residual_failures == 2); /* LUN 0 primary and backup READ(10) */
	/* Unexpected handoff state: skipped with no writes at all. */
	init(s, dma); s->host[0x50/4] = 0;
	assert(opd2413_ufs_read_gpt(&io, dma, &r) == 1 && !s->writes);
	init(s, dma); s->smmu[0x80000/4] ^= 1;
	assert(opd2413_ufs_read_gpt(&io, dma, &r) == 1 && !s->writes);

	for (unsigned i = 0; i < 3; ++i) { free(s->disk[i].primary); free(s->disk[i].backup); }
	free(buffer); free(s);
	puts("PASS: read-only CDB whitelist, CRC-32, REPORT LUNS, capacity 10/16, GPT primary/backup, UA retry, faults, restoration");
	return 0;
}
