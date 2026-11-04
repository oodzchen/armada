// SPDX-License-Identifier: GPL-2.0+
/*
 * Read-only U-Boot block devices ("opdufs", devnum = UFS LUN) on top of the
 * verified OPD2413 survey session: ABL's link is woken from Hibern8, the
 * device from UFS-PowerDown, and every block read is a whitelisted READ(10).
 * Writes always fail with -EROFS. opd2413_ufs_blk_stop() restores the power
 * mode, ABL's queue and Hibern8 before the original kernel is re-entered.
 */
#include <blk.h>
#include <dm.h>
#include <dm/device-internal.h>
#include <dm/lists.h>
#include <dm/root.h>
#include <errno.h>
#include <log.h>
#include <stdio.h>
#include <string.h>
#include "opd2413_ufs_read.h"
#include "opd2413_ufs_blk.h"

static struct opd_ufs_session session;
static struct opd_ufs_read_result result;

#ifdef CONFIG_OPD2413_UFS_BLK_RAMDISK
/*
 * QEMU-only test backend, never in a device build: one LUN described by an
 * extent table at OPD_RAMDISK_BASE (built by make_ramdisk.py); blocks outside
 * the extents read as zeros. Exercises this driver and U-Boot's partition and
 * filesystem code on the real ARM binary without UFS hardware.
 */
#define OPD_RAMDISK_BASE 0x900000000ULL
#define OPD_RAMDISK_MAGIC 0x4b534452U /* "RDSK" */
struct opd_ramdisk_extent { uint64_t lba, count, offset; };
struct opd_ramdisk { uint32_t magic, count; uint64_t lun, blocks; struct opd_ramdisk_extent ext[]; };
static const struct opd_ramdisk *ramdisk = (const void *)OPD_RAMDISK_BASE;

static void ramdisk_read(lbaint_t lba, lbaint_t blkcnt, uint8_t *out)
{
	for (lbaint_t b = 0; b < blkcnt; ++b, ++lba, out += OPD_READ_BLOCK) {
		const uint8_t *src = NULL;
		for (uint32_t i = 0; i < ramdisk->count; ++i) {
			const struct opd_ramdisk_extent *e = &ramdisk->ext[i];
			if (lba >= e->lba && lba - e->lba < e->count)
				src = (const uint8_t *)ramdisk + e->offset + (lba - e->lba) * OPD_READ_BLOCK;
		}
		if (src) memcpy(out, src, OPD_READ_BLOCK);
		else memset(out, 0, OPD_READ_BLOCK);
	}
}
#endif

#ifdef CONFIG_OPD2413_PROGRESS
void opd2413_progress_read(u64 bytes); /* observer.c */
#endif

static unsigned long opd_blk_read(struct udevice *dev, lbaint_t start, lbaint_t blkcnt, void *buffer)
{
	struct blk_desc *desc = dev_get_uclass_plat(dev);
#ifdef CONFIG_OPD2413_PROGRESS
	opd2413_progress_read((u64)blkcnt * OPD_READ_BLOCK);
#endif
#ifdef CONFIG_OPD2413_UFS_TRACE
	printf("opdufs: read lun %d lba %llu count %llu buffer %p\n", desc->devnum,
	       (unsigned long long)start, (unsigned long long)blkcnt, buffer);
#endif
#ifdef CONFIG_OPD2413_UFS_BLK_RAMDISK
	if (start + blkcnt > desc->lba) return -EIO;
	ramdisk_read(start, blkcnt, buffer);
	return blkcnt;
#endif
	if (!session.open || start + blkcnt > desc->lba) return -EIO;
	int ret = opd2413_ufs_read(&session, (uint8_t)desc->devnum, start, (uint32_t)blkcnt, buffer);
#ifdef CONFIG_OPD2413_UFS_TRACE
	printf("opdufs: read done ret=%d\n", ret);
#endif
	if (ret < 0) printf("opdufs: transport failure; Android return will be refused\n");
	return ret ? -EIO : blkcnt;
}

static unsigned long opd_blk_write(struct udevice *dev, lbaint_t start, lbaint_t blkcnt, const void *buffer)
{
	printf("opdufs: write of %lu blocks at %lu refused (read-only)\n", (unsigned long)blkcnt, (unsigned long)start);
	return -EROFS;
}

static unsigned long opd_blk_erase(struct udevice *dev, lbaint_t start, lbaint_t blkcnt)
{
	printf("opdufs: erase refused (read-only)\n");
	return -EROFS;
}

static const struct blk_ops opd_blk_ops = {
	.read = opd_blk_read,
	.write = opd_blk_write,
	.erase = opd_blk_erase,
};

U_BOOT_DRIVER(opd2413_ufs_blk) = {
	.name = "opd2413_ufs_blk",
	.id = UCLASS_BLK,
	.ops = &opd_blk_ops,
};

U_BOOT_DRIVER(opd2413_ufs) = {
	.name = "opd2413_ufs",
	.id = UCLASS_OPD2413_UFS,
};

UCLASS_DRIVER(opd2413_ufs) = {
	.name = "opdufs",
	.id = UCLASS_OPD2413_UFS,
};

#ifdef CONFIG_OPD2413_UFS_BLK_RAMDISK
int opd2413_ufs_blk_start_ramdisk(void)
{
	struct udevice *parent, *bdev;
	char name[16];
	if (ramdisk->magic != OPD_RAMDISK_MAGIC) { printf("opdufs: no ramdisk\n"); return 1; }
	int ret = device_bind_driver(dm_root(), "opd2413_ufs", "opdufs", &parent);
	if (ret) return ret;
	snprintf(name, sizeof(name), "lun%u", (unsigned)ramdisk->lun);
	ret = blk_create_devicef(parent, "opd2413_ufs_blk", name, UCLASS_OPD2413_UFS,
				 ramdisk->lun, OPD_READ_BLOCK, ramdisk->blocks, &bdev);
	if (!ret) ret = blk_probe_or_unbind(bdev);
	printf("OPD2413_UFS_BLK ramdisk opdufs %u blocks=%llu ret=%d\n", (unsigned)ramdisk->lun,
	       (unsigned long long)ramdisk->blocks, ret);
	return ret;
}
#endif

int opd2413_ufs_blk_start(const struct opd_ufs_io *io)
{
	struct udevice *parent;
	uint8_t luns[OPD_READ_MAX_LUNS];
	unsigned count = 0;
	int ret = opd2413_ufs_open(&session, io, (volatile void *)OPD_DMA_BASE, OPD_UFS_WAKE, &result);
	if (ret) return ret;
	if (!result.woke) {
		printf("opdufs: device not awake; no block devices\n");
		return opd2413_ufs_close(&session) ? -1 : 1;
	}
	if (opd2413_ufs_luns(&session, luns, &count) < 0) return -1;
	ret = device_bind_driver(dm_root(), "opd2413_ufs", "opdufs", &parent);
	if (ret) {
		printf("opdufs: bind failed %d\n", ret);
		return opd2413_ufs_close(&session) ? -1 : 1;
	}
	for (unsigned i = 0; i < count; ++i) {
		struct opd_ufs_read_lun *l = &result.lun[result.luns++];
		struct udevice *bdev;
		char name[16];
		l->lun = luns[i];
		if (opd2413_ufs_capacity(&session, l) < 0) return -1;
		if (!l->capacity_ok || l->block != OPD_READ_BLOCK || l->last_lba >= 0xffffffffULL) continue;
		snprintf(name, sizeof(name), "lun%u", l->lun);
		ret = blk_create_devicef(parent, "opd2413_ufs_blk", name, UCLASS_OPD2413_UFS,
					 l->lun, OPD_READ_BLOCK, l->last_lba + 1, &bdev);
		if (!ret) ret = blk_probe_or_unbind(bdev);
		if (ret) { printf("opdufs: LUN %u block device failed %d\n", l->lun, ret); continue; }
		struct blk_desc *desc = dev_get_uclass_plat(bdev);
		snprintf(desc->vendor, sizeof(desc->vendor), "OPD2413");
		snprintf(desc->product, sizeof(desc->product), "UFS LUN %u RO", l->lun);
		desc->removable = 0;
		printf("OPD2413_UFS_BLK opdufs %u blocks=%llu\n", l->lun, (unsigned long long)(l->last_lba + 1));
	}
	return 0;
}

int opd2413_ufs_blk_stop(void)
{
#ifdef CONFIG_OPD2413_UFS_BLK_RAMDISK
	return 0;
#endif
	/* Reads fail from here on; the devices stay bound but unusable. */
	return opd2413_ufs_close(&session);
}
