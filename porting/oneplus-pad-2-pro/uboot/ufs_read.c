// SPDX-License-Identifier: GPL-2.0+
/* Read-only LUN/capacity/GPT survey on ABL's legacy single-doorbell UFS link.
 * Same guards and restoration as the NOP experiment; only whitelisted SCSI
 * read commands, one at a time with task tag 0 in slot 0. UTRD/UPIU/PRDT
 * layout follows the pinned U-Boot drivers/ufs/{ufshci.h,ufs.h,ufs-uclass.c}.
 */
#include <stdio.h>
#include <string.h>
#include "opd2413_ufs_read.h"
#include "opd2413_smmu_snapshot.h"
#include "opd2413_ufs_link.h"
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "OPD2413 UTRD layout requires a little-endian build"
#endif

#define IS 0x20
#define IE 0x24
#define HCE 0x34
#define UTRLBA 0x50
#define UTRLBA_HI 0x54
#define DBR 0x58
#define RSR 0x60
#define UFS_ERRORS 0x000f0804U

#define UCD 0x1000U
#define RSP (UCD + 0x200U)
#define PRDT (UCD + 0x400U)
#define DATA 0x2000U
#define DATA_MAX (OPD_DMA_SIZE - DATA)
#define REPORT_LUNS_WLUN 0x81
#define REPORT_LUNS_BYTES 4096U

enum { CMD_OK = 0, CMD_STATUS = 1, CMD_FAILED = -1, CMD_REFUSED = 2 };

/* Last submitted command, for the failure summary. */
static uint8_t last_cdb = 0xff, last_lun = 0xff;
/* START STOP UNIT is accepted only during an OPD_UFS_WAKE survey. */
static int wake_enabled;
#ifdef OPD2413_BOOT_CHOICE_WRITER
/* Live only for the duration of the single authorized WRITE(10). */
static const uint8_t *choice_data;
static uint8_t choice_lun;
static uint32_t choice_lba;
#endif
/* Debug hook around each doorbell (phase 0 before, 1 after); the observer
 * overrides it in CONFIG_OPD2413_UFS_TRACE builds. */
__attribute__((weak)) void opd2413_ufs_trace(int phase, uint8_t op, uint8_t lun) { (void)phase; (void)op; (void)lun; }
#define DEVICE_WLUN 0xd0

struct cmd_result {
	uint8_t status, sense_key, asc, ascq;
	uint32_t residual;
};

uint32_t opd2413_crc32(uint32_t crc, const volatile void *buf, size_t len)
{
	const volatile uint8_t *p = buf;
	crc = ~crc;
	while (len--) {
		crc ^= *p++;
		for (unsigned k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xedb88320U & -(crc & 1));
	}
	return ~crc;
}

static uint32_t be32(const volatile uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static uint64_t be64(const volatile uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }
static uint32_t le32(const volatile uint8_t *p)
{
	return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t le64(const volatile uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }
static void put_be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

int opd2413_ufs_read_allowed(const uint8_t *cdb, unsigned cdb_len, uint32_t xfer)
{
	if (!cdb) return 0;
	switch (cdb[0]) {
	case 0x00: /* TEST UNIT READY */
		return cdb_len == 6 && !xfer;
	case 0xa0: /* REPORT LUNS */
		return cdb_len == 12 && xfer == REPORT_LUNS_BYTES && be32(cdb + 6) == xfer;
	case 0x25: /* READ CAPACITY(10) */
		return cdb_len == 10 && xfer == 8;
	case 0x9e: /* SERVICE ACTION IN: READ CAPACITY(16) only */
		return cdb_len == 16 && cdb[1] == 0x10 && xfer == 32 && be32(cdb + 10) == xfer;
	case 0x28: { /* READ(10), no FUA/DPO/protection bits */
		uint32_t blocks = (uint32_t)cdb[7] << 8 | cdb[8];
		return cdb_len == 10 && !cdb[1] && blocks && xfer == blocks * OPD_READ_BLOCK &&
		       xfer <= DATA_MAX;
	}
	default:
		return 0;
	}
}

int opd2413_ufs_ssu_allowed(uint8_t lun, const uint8_t *cdb, unsigned cdb_len, uint32_t xfer)
{
	return cdb && lun == DEVICE_WLUN && cdb_len == 6 && !xfer && cdb[0] == 0x1b &&
	       !cdb[1] && !cdb[2] && !cdb[3] && !cdb[5] &&
	       (cdb[4] == 0x10 || cdb[4] == 0x20 || cdb[4] == 0x30);
}

int opd2413_ufs_query_allowed(uint8_t function, uint8_t opcode, uint16_t length)
{
	if (function != 0x01) return 0; /* STANDARD READ REQUEST */
	switch (opcode) {
	case 0x01: return length && length <= 0xff; /* READ DESCRIPTOR */
	case 0x03: case 0x05: return !length;      /* READ ATTRIBUTE / FLAG */
	default: return 0;
	}
}

static uint32_t host(const struct opd_ufs_io *io, unsigned off)
{
	return io->read32(OPD_UFS_BASE + off, io->ctx);
}
static uint32_t smmu(const struct opd_ufs_io *io, unsigned off)
{
	return io->read32(OPD_SMMU_BASE + off, io->ctx);
}
static void write_host(const struct opd_ufs_io *io, unsigned off, uint32_t value)
{
	io->write32(OPD_UFS_BASE + off, value, io->ctx);
	io->sync_io(io->ctx);
}
static int wait_host_n(const struct opd_ufs_io *io, unsigned off, uint32_t value, unsigned polls)
{
	for (unsigned i = 0; i < polls; ++i) {
		if (host(io, off) == value) return 0;
		io->delay_us(50, io->ctx);
	}
	return -1;
}
static int wait_host(const struct opd_ufs_io *io, unsigned off, uint32_t value)
{
	return wait_host_n(io, off, value, 10000);
}

/*
 * The DMA window is Normal-NC: every access is its own bus transaction, so
 * fill and copy it in 64-bit words (all offsets used here are 8-aligned).
 * Byte loops made a 56 KiB read cost about 8 ms, ~7 MB/s.
 */
static void dma_fill(volatile uint8_t *p, uint8_t value, unsigned n)
{
	const uint64_t v = 0x0101010101010101ULL * value;
	unsigned i = 0;
	for (; i + 8 <= n; i += 8) *(volatile uint64_t *)(p + i) = v;
	for (; i < n; ++i) p[i] = value;
}

static void dma_copy(uint8_t *out, const volatile uint8_t *p, unsigned n)
{
	unsigned i = 0;
	for (; i + 8 <= n; i += 8) {
		uint64_t v = *(const volatile uint64_t *)(p + i);
		memcpy(out + i, &v, 8); /* the caller's buffer may be unaligned */
	}
	for (; i < n; ++i) out[i] = p[i];
}

/* One command in slot 0. Data, if any, lands at dma + DATA. */
static int submit(const struct opd_ufs_io *io, volatile uint8_t *dma, uint8_t lun,
		  const uint8_t *cdb, unsigned cdb_len, uint32_t xfer, struct cmd_result *r)
{
	volatile uint32_t *utrd = (volatile uint32_t *)dma;
	int writing = 0;
#ifdef OPD2413_BOOT_CHOICE_WRITER
	writing = choice_data && lun == choice_lun && cdb_len == 10 &&
		cdb[0] == 0x2a && cdb[1] == 8 && be32(cdb + 2) == choice_lba &&
		!cdb[6] && !cdb[7] && cdb[8] == 1 && !cdb[9] && xfer == OPD_READ_BLOCK;
#endif
	if (cdb_len > 16 || !(opd2413_ufs_read_allowed(cdb, cdb_len, xfer) ||
			      (wake_enabled && opd2413_ufs_ssu_allowed(lun, cdb, cdb_len, xfer)) || writing)) {
		printf("UFS_READ refused CDB %02x len %u xfer %u\n", cdb[0], cdb_len, xfer);
		return CMD_REFUSED;
	}
	last_cdb = cdb[0]; last_lun = lun;
	dma_fill(dma, 0, DATA);
	dma_fill(dma + DATA, 0xa5, xfer);
#ifdef OPD2413_BOOT_CHOICE_WRITER
	if (writing) {
		for (unsigned i = 0; i < OPD_READ_BLOCK; i += 8) {
			uint64_t word;
			memcpy(&word, choice_data + i, 8);
			*(volatile uint64_t *)(dma + DATA + i) = word;
		}
	}
#endif
	dma_fill(dma + RSP, 0xa5, PRDT - RSP);
	utrd[0] = writing ? 0x13000000 : xfer ? 0x15000000 : 0x11000000;
	utrd[2] = 0xf;
	utrd[4] = (uint32_t)(OPD_DMA_BASE + UCD);
	utrd[5] = (uint32_t)((OPD_DMA_BASE + UCD) >> 32);
	utrd[6] = 0x00800080; /* response at +512, 512 bytes, DWORD units */
	utrd[7] = xfer ? 0x01000001 : 0x01000000; /* PRDT at +1024 */
	uint8_t upiu[32] = {0x01, writing ? 0x20 : xfer ? 0x40 : 0, lun, 0};
	put_be32(upiu + 12, xfer);
	for (unsigned i = 0; i < cdb_len; ++i) upiu[16 + i] = cdb[i];
	for (unsigned i = 0; i < sizeof(upiu); ++i) dma[UCD + i] = upiu[i];
	if (xfer) {
		volatile uint32_t *prd = (volatile uint32_t *)(dma + PRDT);
		prd[0] = (uint32_t)(OPD_DMA_BASE + DATA);
		prd[1] = (uint32_t)((OPD_DMA_BASE + DATA) >> 32);
		prd[2] = 0;
		prd[3] = (xfer - 1) | 3;
	}
	io->sync_io(io->ctx);
	opd2413_ufs_trace(0, cdb[0], lun);
	write_host(io, DBR, 1);
	/* A power-mode change may take far longer than a read: allow 5 s. */
	int waited = wait_host_n(io, DBR, 0, cdb[0] == 0x1b ? 100000 : 10000);
	opd2413_ufs_trace(1, cdb[0], lun);
	if (waited) {
		printf("UFS_READ CDB %02x LUN %02x timeout DBR=%08x IS=%08x\n", cdb[0], lun,
		       host(io, DBR), host(io, IS));
		return CMD_FAILED;
	}
	io->sync_io(io->ctx);
	const volatile uint8_t *rsp = dma + RSP;
	uint32_t ocs = utrd[2] & 0xff, is = host(io, IS);
	if (ocs || rsp[0] != 0x21 || rsp[3] || (is & UFS_ERRORS) || !(is & 1)) {
		printf("UFS_READ CDB %02x LUN %02x transport OCS=%02x UPIU=%02x tag=%02x resp=%02x IS=%08x\n",
		       cdb[0], lun, ocs, rsp[0], rsp[3], rsp[6], is);
		return CMD_FAILED;
	}
	write_host(io, IS, 1); /* this command's completion only */
	if (rsp[6]) {
		/* Target Failure: the device refused the command, but the transfer
		 * completed normally (OCS, doorbell and completion as expected), so
		 * the controller state stays known. Record the raw UPIU. */
		r->status = rsp[7] ? rsp[7] : 0xff; r->residual = 0; r->sense_key = r->asc = r->ascq = 0;
		uint32_t len = (uint32_t)rsp[32] << 8 | rsp[33];
		if (rsp[7] == 2 && len >= 14) { r->sense_key = rsp[36] & 0xf; r->asc = rsp[46]; r->ascq = rsp[47]; }
		printf("UFS_READ CDB %02x LUN %02x target failure resp=%02x status=%02x sense %x/%02x/%02x UPIU:",
		       cdb[0], lun, rsp[6], rsp[7], r->sense_key, r->asc, r->ascq);
		for (unsigned i = 0; i < 48; ++i) printf("%s%02x", i % 16 ? "" : " ", rsp[i]);
		printf("\n");
		return CMD_STATUS;
	}
	r->status = rsp[7];
	r->residual = be32(rsp + 12);
	r->sense_key = r->asc = r->ascq = 0;
	if (r->residual > xfer) {
		printf("UFS_READ CDB %02x LUN %02x residual %u > %u\n", cdb[0], lun, r->residual, xfer);
		return CMD_FAILED;
	}
	if (r->status) {
		uint32_t len = (uint32_t)rsp[32] << 8 | rsp[33];
		if (len >= 14) { r->sense_key = rsp[34 + 2] & 0xf; r->asc = rsp[34 + 12]; r->ascq = rsp[34 + 13]; }
		printf("UFS_READ CDB %02x LUN %02x status %02x sense %x/%02x/%02x\n",
		       cdb[0], lun, r->status, r->sense_key, r->asc, r->ascq);
		return CMD_STATUS;
	}
	return CMD_OK;
}

/* One QUERY REQUEST (no PRDT; any descriptor returns in the response
 * UPIU data segment). CMD_OK with *value set, CMD_STATUS for a query
 * failure response, CMD_FAILED for a transport fault. */
static int query(const struct opd_ufs_io *io, volatile uint8_t *dma, uint8_t opcode, uint8_t idn,
		 uint16_t length, uint32_t *value, uint16_t *data_len)
{
	volatile uint32_t *utrd = (volatile uint32_t *)dma;
	if (!opd2413_ufs_query_allowed(0x01, opcode, length)) {
		printf("UFS_QUERY refused opcode %02x length %u\n", opcode, length);
		return CMD_REFUSED;
	}
	last_cdb = 0x16; last_lun = idn;
	for (unsigned i = 0; i < DATA; ++i) dma[i] = 0;
	for (unsigned i = RSP; i < PRDT; ++i) dma[i] = 0xa5;
	utrd[0] = 0x11000000;
	utrd[2] = 0xf;
	utrd[4] = (uint32_t)(OPD_DMA_BASE + UCD);
	utrd[5] = (uint32_t)((OPD_DMA_BASE + UCD) >> 32);
	utrd[6] = 0x00800080;
	utrd[7] = 0x01000000;
	uint8_t upiu[32] = {0x16, 0, 0, 0, 0, 0x01};
	upiu[12] = opcode; upiu[13] = idn; upiu[18] = length >> 8; upiu[19] = length;
	for (unsigned i = 0; i < sizeof(upiu); ++i) dma[UCD + i] = upiu[i];
	io->sync_io(io->ctx);
	opd2413_ufs_trace(0, 0x16, idn);
	write_host(io, DBR, 1);
	int waited = wait_host(io, DBR, 0);
	opd2413_ufs_trace(1, 0x16, idn);
	if (waited) {
		printf("UFS_QUERY %02x/%02x timeout DBR=%08x IS=%08x\n", opcode, idn, host(io, DBR), host(io, IS));
		return CMD_FAILED;
	}
	io->sync_io(io->ctx);
	const volatile uint8_t *rsp = dma + RSP;
	uint32_t ocs = utrd[2] & 0xff, is = host(io, IS);
	uint16_t seg = (uint16_t)(rsp[10] << 8 | rsp[11]);
	if (ocs || rsp[0] != 0x36 || rsp[3] || rsp[12] != opcode || rsp[13] != idn ||
	    seg > 0x1c0 || (is & UFS_ERRORS) || !(is & 1)) {
		printf("UFS_QUERY %02x/%02x transport OCS=%02x UPIU=%02x tag=%02x op=%02x idn=%02x seg=%u IS=%08x\n",
		       opcode, idn, ocs, rsp[0], rsp[3], rsp[12], rsp[13], seg, is);
		return CMD_FAILED;
	}
	write_host(io, IS, 1);
	if (rsp[6]) {
		printf("UFS_QUERY %02x/%02x response %02x\n", opcode, idn, rsp[6]);
		return CMD_STATUS;
	}
	if (value) *value = be32(rsp + 20);
	if (data_len) *data_len = seg;
	return CMD_OK;
}

static int read_queries(const struct opd_ufs_io *io, volatile uint8_t *dma, struct opd_ufs_read_result *res)
{
	uint32_t v = 0;
	int ret = query(io, dma, 0x03, 0x02, 0, &v, NULL); /* bCurrentPowerMode */
	if (ret < 0) return -1;
	if (!ret) { res->power_mode_ok = 1; res->power_mode = v; }
	ret = query(io, dma, 0x03, 0x00, 0, &v, NULL); /* bBootLunEn */
	if (ret < 0) return -1;
	if (!ret) { res->boot_lun_ok = 1; res->boot_lun = v; }
	ret = query(io, dma, 0x05, 0x01, 0, &v, NULL); /* fDeviceInit */
	if (ret < 0) return -1;
	if (!ret) { res->device_init_ok = 1; res->device_init = v & 1; }
	uint16_t len = 0;
	ret = query(io, dma, 0x01, 0x00, 0x40, NULL, &len); /* device descriptor */
	if (ret < 0) return -1;
	const volatile uint8_t *d = dma + RSP + 32;
	if (!ret && len >= 0x1a && d[1] == 0x00) {
		res->descriptor_ok = 1;
		res->number_lu = d[6]; res->init_power_mode = d[10];
		res->spec_version = (uint16_t)(d[16] << 8 | d[17]);
		res->manufacturer_id = (uint16_t)(d[24] << 8 | d[25]);
		printf("UFS_QUERY device descriptor %u bytes:", len);
		for (unsigned i = 0; i < len && i < 0x40; ++i) printf("%s%02x", i % 16 ? "" : " ", d[i]);
		printf("\n");
	}
	printf("UFS_QUERY bCurrentPowerMode=%s%02x bBootLunEn=%s%02x fDeviceInit=%s%u bNumberLU=%u bInitPowerMode=%02x spec=%04x manufacturer=%04x\n",
	       res->power_mode_ok ? "" : "?", res->power_mode, res->boot_lun_ok ? "" : "?", res->boot_lun,
	       res->device_init_ok ? "" : "?", res->device_init, res->number_lu, res->init_power_mode,
	       res->spec_version, res->manufacturer_id);
	return 0;
}

/* GOOD, retrying once after UNIT ATTENTION. */
static int command(const struct opd_ufs_io *io, volatile uint8_t *dma, uint8_t lun,
		   const uint8_t *cdb, unsigned cdb_len, uint32_t xfer)
{
	struct cmd_result r;
	int ret = submit(io, dma, lun, cdb, cdb_len, xfer, &r);
	if (ret == CMD_STATUS && r.status == 2 && r.sense_key == 6)
		ret = submit(io, dma, lun, cdb, cdb_len, xfer, &r);
	if (ret == CMD_OK && r.residual) {
		printf("UFS_READ CDB %02x LUN %02x short by %u bytes\n", cdb[0], lun, r.residual);
		return CMD_STATUS;
	}
	return ret;
}

static int read10(const struct opd_ufs_io *io, volatile uint8_t *dma, uint8_t lun,
		  uint32_t lba, unsigned blocks)
{
	uint8_t cdb[10] = {0x28, 0};
	put_be32(cdb + 2, lba);
	cdb[7] = blocks >> 8; cdb[8] = blocks;
	return command(io, dma, lun, cdb, sizeof(cdb), blocks * OPD_READ_BLOCK);
}

/* Validate one GPT header in a 4 KiB block; copy of the header for its CRC. */
static int gpt_header(const volatile uint8_t *h, uint64_t lba, uint64_t last)
{
	static const char sig[8] = {'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T'};
	uint8_t copy[512];
	uint32_t size = le32(h + 12);
	for (unsigned i = 0; i < 8; ++i) if (h[i] != (uint8_t)sig[i]) return 0;
	if (size < 92 || size > sizeof(copy) || le64(h + 24) != lba) return 0;
	for (unsigned i = 0; i < size; ++i) copy[i] = h[i];
	copy[16] = copy[17] = copy[18] = copy[19] = 0;
	if (opd2413_crc32(0, copy, size) != le32(h + 16)) return 0;
	uint64_t first = le64(h + 40), last_usable = le64(h + 48);
	return first <= last_usable && last_usable <= last;
}

static void entry_name(const volatile uint8_t *e, char *name)
{
	unsigned n = 0;
	for (unsigned i = 0; i < 36; ++i) {
		uint16_t c = e[56 + 2 * i] | (uint16_t)e[57 + 2 * i] << 8;
		if (!c) break;
		name[n++] = c >= 0x20 && c < 0x7f ? (char)c : '?';
	}
	name[n] = 0;
}

/* -1 transport failure; 0 done (outcome recorded in `l`). */
/* TEST UNIT READY and READ CAPACITY: -1 transport failure, 0 done
 * (l->capacity_ok tells whether the unit is usable). */
static int lun_capacity(const struct opd_ufs_io *io, volatile uint8_t *dma, struct opd_ufs_read_lun *l)
{
	const uint8_t tur[6] = {0};
	int ret = command(io, dma, l->lun, tur, sizeof(tur), 0);
	if (ret < 0) return -1;
	if (ret) { printf("UFS_LUN %u not ready\n", l->lun); return 0; }
	const uint8_t cap10[10] = {0x25};
	ret = command(io, dma, l->lun, cap10, sizeof(cap10), 8);
	if (ret < 0) return -1;
	if (ret) return 0;
	l->last_lba = be32(dma + DATA); l->block = be32(dma + DATA + 4);
	if (l->last_lba == 0xffffffffU) {
		uint8_t cap16[16] = {0x9e, 0x10};
		put_be32(cap16 + 10, 32);
		ret = command(io, dma, l->lun, cap16, sizeof(cap16), 32);
		if (ret < 0) return -1;
		if (ret) return 0;
		l->last_lba = be64(dma + DATA); l->block = be32(dma + DATA + 8);
	}
	l->capacity_ok = 1;
	printf("UFS_LUN %u last_lba=%llu block=%u bytes=%llu\n", l->lun,
	       (unsigned long long)l->last_lba, l->block,
	       (unsigned long long)(l->last_lba + 1) * l->block);
	return 0;
}

static int survey_lun(const struct opd_ufs_io *io, volatile uint8_t *dma, struct opd_ufs_read_lun *l)
{
	int ret = lun_capacity(io, dma, l);
	if (ret < 0 || !l->capacity_ok) return ret;
	if (l->block != OPD_READ_BLOCK || l->last_lba < 64 || l->last_lba > 0xffffffffU) {
		printf("UFS_LUN %u GPT skipped: unsupported geometry\n", l->lun);
		return 0;
	}
	ret = read10(io, dma, l->lun, 0, OPD_READ_PRIMARY_BLOCKS);
	if (ret < 0) return -1;
	if (ret) return 0;
	for (unsigned b = 0; b < OPD_READ_PRIMARY_BLOCKS; ++b) {
		l->primary_crc[b] = opd2413_crc32(0, dma + DATA + b * OPD_READ_BLOCK, OPD_READ_BLOCK);
		printf("UFS_LUN %u LBA %u crc32=%08x\n", l->lun, b, l->primary_crc[b]);
	}
	const volatile uint8_t *h = dma + DATA + OPD_READ_BLOCK;
	uint64_t alt = le64(h + 32), entries = le64(h + 72);
	uint32_t count = le32(h + 80), size = le32(h + 84);
	if (!gpt_header(h, 1, l->last_lba) || entries != 2 || size != 128 || !count ||
	    (uint64_t)count * size > (OPD_READ_PRIMARY_BLOCKS - 2) * OPD_READ_BLOCK ||
	    opd2413_crc32(0, dma + DATA + 2 * OPD_READ_BLOCK, count * size) != le32(h + 88)) {
		printf("UFS_LUN %u primary GPT invalid\n", l->lun);
		return 0;
	}
	l->primary_ok = 1;
	printf("UFS_LUN %u GPT alt=%llu usable=%llu-%llu entries=%u entries_crc=%08x\n", l->lun,
	       (unsigned long long)alt, (unsigned long long)le64(h + 40),
	       (unsigned long long)le64(h + 48), count, le32(h + 88));
	for (uint32_t i = 0; i < count; ++i) {
		const volatile uint8_t *e = dma + DATA + 2 * OPD_READ_BLOCK + i * size;
		int used = 0;
		for (unsigned k = 0; k < 16; ++k) used |= e[k];
		if (!used) continue;
		char name[37];
		entry_name(e, name);
		++l->partitions;
		printf("UFS_PART %u.%u %s %llu-%llu\n", l->lun, i + 1, name,
		       (unsigned long long)le64(e + 32), (unsigned long long)le64(e + 40));
	}
	if (alt != l->last_lba) {
		printf("UFS_LUN %u backup GPT not at last LBA\n", l->lun);
		return 0;
	}
	ret = read10(io, dma, l->lun, (uint32_t)(alt - (OPD_READ_BACKUP_BLOCKS - 1)), OPD_READ_BACKUP_BLOCKS);
	if (ret < 0) return -1;
	if (ret) return 0;
	for (unsigned b = 0; b < OPD_READ_BACKUP_BLOCKS; ++b) {
		l->backup_crc[b] = opd2413_crc32(0, dma + DATA + b * OPD_READ_BLOCK, OPD_READ_BLOCK);
		printf("UFS_LUN %u LBA %llu crc32=%08x\n", l->lun,
		       (unsigned long long)(alt - (OPD_READ_BACKUP_BLOCKS - 1) + b), l->backup_crc[b]);
	}
	l->backup_ok = gpt_header(dma + DATA + (OPD_READ_BACKUP_BLOCKS - 1) * OPD_READ_BLOCK, alt, l->last_lba);
	printf("UFS_LUN %u backup GPT %s\n", l->lun, l->backup_ok ? "valid" : "invalid");
	return 0;
}

static int ssu(const struct opd_ufs_io *io, volatile uint8_t *dma, uint8_t condition)
{
	const uint8_t cdb[6] = {0x1b, 0, 0, 0, (uint8_t)(condition << 4), 0};
	printf("UFS_WAKE START STOP UNIT power_condition=%u\n", condition);
	return command(io, dma, DEVICE_WLUN, cdb, sizeof(cdb), 0);
}

/* Any transport fault: stop the list, never restore a possibly live base. */
static int session_fail(struct opd_ufs_session *s)
{
	const struct opd_ufs_io *io = s->io;
	wake_enabled = 0;
	printf("OPD2413_UFS_READ_FAIL last_cdb=%02x last_lun=%02x luns_done=%u DBR=%08x IS=%08x RSR=%08x; reset required\n",
	       last_cdb, last_lun, s->res->luns, host(io, DBR), host(io, IS), host(io, RSR));
	printf("UFS_READ SMMU GFSR=%08x GFSYNR1=%08x CB0_FSR=%08x FAR=%08x%08x FSYNR0=%08x\n",
	       smmu(io, 0x48), smmu(io, 0x54), smmu(io, 0x80058),
	       smmu(io, 0x80064), smmu(io, 0x80060), smmu(io, 0x80068));
	write_host(io, RSR, 0);
	s->open = 0; s->broken = 1;
	return -1;
}

int opd2413_ufs_open(struct opd_ufs_session *s, const struct opd_ufs_io *io, volatile void *window,
		     unsigned flags, struct opd_ufs_read_result *res)
{
	volatile uint8_t *dma = window;
	*s = (struct opd_ufs_session){.io_copy = *io, .dma = dma, .res = res};
	s->io = io = &s->io_copy;
	last_cdb = last_lun = 0xff;
	wake_enabled = 0;
	if (!res || (flags & ~OPD_UFS_WAKE)) return 1;
	for (size_t i = 0; i < sizeof(*res); ++i) ((uint8_t *)res)[i] = 0;
	printf("OPD2413_UFS_READ_BEGIN\n");
	int ret = opd2413_ufs_preflight(io, "UFS_READ", s->saved);
	if (ret) { s->broken = ret < 0; return ret; }
	ret = opd2413_ufs_hibern8(io, 0);
	if (ret) { s->broken = ret < 0; return ret; }
	for (unsigned i = 0; i < OPD_DMA_SIZE; ++i) dma[i] = 0;
	io->sync_io(io->ctx);
	printf("UFS_READ DMA=%llx original_queue=%08x%08x\n",
	       (unsigned long long)OPD_DMA_BASE, s->saved[1], s->saved[0]);
	write_host(io, RSR, 0);
	if (wait_host(io, RSR, 0) || host(io, DBR)) return session_fail(s);
	write_host(io, UTRLBA_HI, 0);
	write_host(io, UTRLBA, (uint32_t)OPD_DMA_BASE);
	if (host(io, UTRLBA) != OPD_DMA_BASE || host(io, UTRLBA_HI)) return session_fail(s);
	write_host(io, RSR, 1);
	if (wait_host(io, RSR, 1)) return session_fail(s);
	s->open = 1;

	if (read_queries(io, dma, res) < 0) return session_fail(s);
	if (flags & OPD_UFS_WAKE) {
		if (res->power_mode_ok && (res->power_mode == 0x33 || res->power_mode == 0x22)) {
			s->condition = res->power_mode >> 4;
			wake_enabled = 1;
			ret = ssu(io, dma, 1);
			if (ret < 0) return session_fail(s);
			if (ret) {
				/* Refused and completed: the device kept its state. */
				printf("UFS_WAKE refused; survey without waking\n");
				s->condition = 0; wake_enabled = 0;
			} else {
				uint32_t mode = 0;
				ret = query(io, dma, 0x03, 0x02, 0, &mode, NULL);
				if (ret < 0) return session_fail(s);
				res->woke = 1;
				printf("UFS_WAKE active bCurrentPowerMode=%s%02x\n", ret ? "?" : "", mode);
			}
		} else {
			printf("UFS_WAKE skipped: power mode %s%02x is not Sleep/PowerDown\n",
			       res->power_mode_ok ? "" : "?", res->power_mode);
		}
	}
	return 0;
}

int opd2413_ufs_luns(struct opd_ufs_session *s, uint8_t luns[OPD_READ_MAX_LUNS], unsigned *count)
{
	const struct opd_ufs_io *io = s->io;
	volatile uint8_t *dma = s->dma;
	unsigned nluns = 0;
	if (!s->open) return -1;
	uint8_t report[12] = {0xa0};
	put_be32(report + 6, REPORT_LUNS_BYTES);
	int ret = command(io, dma, REPORT_LUNS_WLUN, report, sizeof(report), REPORT_LUNS_BYTES);
	if (ret < 0) return session_fail(s);
	if (ret == CMD_OK) {
		uint32_t bytes = be32(dma + DATA);
		for (uint32_t off = 8; off + 8 <= 8 + bytes && off + 8 <= REPORT_LUNS_BYTES; off += 8) {
			const volatile uint8_t *e = dma + DATA + off;
			/* Peripheral addressing of a logical unit; well-known LUNs use 0xc1. */
			if (e[0] || e[1] >= 0x80) continue;
			if (nluns == OPD_READ_MAX_LUNS) break;
			luns[nluns++] = e[1];
		}
		printf("UFS_READ REPORT_LUNS bytes=%u luns=%u\n", bytes, nluns);
	}
	if (!nluns) {
		/* Absent units answer TEST UNIT READY with an error and are skipped. */
		printf("UFS_READ REPORT_LUNS unavailable; probing LUN 0-%u\n", OPD_READ_MAX_LUNS - 1);
		for (; nluns < OPD_READ_MAX_LUNS; ++nluns) luns[nluns] = nluns;
	}
	*count = nluns;
	return 0;
}

int opd2413_ufs_capacity(struct opd_ufs_session *s, struct opd_ufs_read_lun *l)
{
	if (!s->open) return -1;
	return lun_capacity(s->io, s->dma, l) < 0 ? session_fail(s) : 0;
}

int opd2413_ufs_read(struct opd_ufs_session *s, uint8_t lun, uint64_t lba, uint32_t blocks, void *buffer)
{
	uint8_t *out = buffer;
	const uint32_t chunk = DATA_MAX / OPD_READ_BLOCK;
	if (!s->open || lun >= 0x80 || lba > 0xffffffffULL || blocks > 0xffffffffULL - lba + 1 ||
	    lba + blocks > 0x100000000ULL) return 1;
	while (blocks) {
		uint32_t n = blocks < chunk ? blocks : chunk;
		int ret = read10(s->io, s->dma, lun, (uint32_t)lba, n);
		if (ret < 0) return session_fail(s);
		if (ret) return 1;
		dma_copy(out, s->dma + DATA, n * OPD_READ_BLOCK);
		out += n * OPD_READ_BLOCK; lba += n; blocks -= n;
	}
	return 0;
}

#ifdef OPD2413_BOOT_CHOICE_WRITER
int opd2413_ufs_write_choice(struct opd_ufs_session *s, uint8_t lun,
                            uint32_t lba, const void *page)
{
	if (!s->open || s->broken || !s->choice_ready || !page ||
	    lun != s->choice_lun ||
	    (lba != s->choice_lba[0] && lba != s->choice_lba[1]))
		return 1;
	uint8_t cdb[10] = {0x2a, 8, 0, 0, 0, 0, 0, 0, 1, 0};
	struct cmd_result result;
	put_be32(cdb + 2, lba);
	choice_data = page; choice_lun = lun; choice_lba = lba;
	int ret = submit(s->io, s->dma, lun, cdb, sizeof(cdb), OPD_READ_BLOCK, &result);
	choice_data = NULL;
	if (ret < 0) return session_fail(s);
	if (ret || result.residual) return 1;
	return 0;
}
#endif

int opd2413_ufs_close(struct opd_ufs_session *s)
{
	const struct opd_ufs_io *io = s->io;
	volatile uint8_t *dma = s->dma;
	struct opd_ufs_read_result *res = s->res;
#ifdef OPD2413_BOOT_CHOICE_WRITER
	s->choice_ready = 0;
#endif
	if (s->broken) return -1;
	if (!s->open) return 0;
	if (s->condition) {
		/* Back to exactly the power mode ABL left, verified by query. */
		uint32_t mode = 0;
		int ret = ssu(io, dma, s->condition);
		if (ret) return session_fail(s);
		ret = query(io, dma, 0x03, 0x02, 0, &mode, NULL);
		if (ret || mode != res->power_mode) {
			printf("UFS_WAKE restore mismatch bCurrentPowerMode=%02x expected %02x\n", mode, res->power_mode);
			return session_fail(s);
		}
		res->restored_power_mode = (uint8_t)mode;
		wake_enabled = 0;
		printf("UFS_WAKE restored bCurrentPowerMode=%02x\n", mode);
	}
	/* Every completion was cleared individually. Stop the idle list and
	 * restore ABL's queue, then its Hibern8 state, exactly as the NOP does. */
	write_host(io, RSR, 0);
	if (wait_host(io, RSR, 0) || host(io, DBR)) return session_fail(s);
	write_host(io, UTRLBA_HI, s->saved[1]);
	write_host(io, UTRLBA, s->saved[0]);
	write_host(io, RSR, s->saved[2]);
	if (host(io, UTRLBA) != s->saved[0] || host(io, UTRLBA_HI) != s->saved[1] ||
	    host(io, RSR) != s->saved[2] || host(io, DBR) || host(io, IE) ||
	    host(io, HCE) != 1 || host(io, IS) & (UFS_ERRORS | 1)) return session_fail(s);
	if (opd2413_ufs_hibern8(io, 1)) return session_fail(s);
	s->open = 0;
	printf("OPD2413_UFS_READ_PASS luns=%u original_queue_and_hibern8_restored\n", res->luns);
	return 0;
}

int opd2413_ufs_read_gpt(const struct opd_ufs_io *io, volatile void *window,
			 struct opd_ufs_read_result *out)
{
	return opd2413_ufs_survey(io, window, out, 0);
}

int opd2413_ufs_survey(const struct opd_ufs_io *io, volatile void *window,
		       struct opd_ufs_read_result *out, unsigned flags)
{
	struct opd_ufs_read_result local;
	struct opd_ufs_session s;
	uint8_t luns[OPD_READ_MAX_LUNS];
	unsigned nluns = 0;
	int ret = opd2413_ufs_open(&s, io, window, flags, out ? out : &local);
	if (ret) return ret;
	if (opd2413_ufs_luns(&s, luns, &nluns) < 0) return -1;
	for (unsigned i = 0; i < nluns; ++i) {
		struct opd_ufs_read_lun *l = &s.res->lun[s.res->luns++];
		l->lun = luns[i];
		if (survey_lun(io, s.dma, l) < 0) return session_fail(&s);
	}
	return opd2413_ufs_close(&s);
}
