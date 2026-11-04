/* SPDX-License-Identifier: GPL-2.0-or-later */
/* OPD2413 only: FAT16 KSHIM.CHO on the existing ARMADA ESP, LUN 0.
 * No filesystem metadata writes. The common UFS reader stays read-only in
 * U-Boot builds; this explicit backend authorizes only two journal LBAs.
 */
#include <boot_choice.h>
#include <config.h>
#include <qcom_dt.h>
#include <timer.h>
#include <kshim_mmu.h>
#include <stdio.h>
#include "opd2413_ufs_read.h"
#include "opd2413_ufs_snapshot.h"
#include "opd2413_smmu_snapshot.h"
#include "choice_fat.h"
#include "choice_journal.h"

extern char __image_start_address__[];
static struct {
    uint64_t image, image_size, esp_start, esp_blocks, page_offset;
    const MANIFEST *manifest;
    uint32_t context, hashes[MANIFEST_MAX_ENTRIES];
    uint8_t guid[16], pages[CHOICE_BYTES];
    uint8_t provision[CHOICE_PAGE];
    struct choice_record record;
    int ready, valid, audited;
    const void *fdt;
} saved;
static struct opd_ufs_session session;
static struct opd_ufs_read_result result;
static uint8_t primary[6 * CHOICE_PAGE], backup[5 * CHOICE_PAGE];
static uint8_t cache[CHOICE_PAGE], current[CHOICE_BYTES], next_page[CHOICE_PAGE];
static uint8_t provision_check[CHOICE_PAGE];
static uint64_t cache_lba;
static int cache_valid;
/* Independent bring-up breadcrumbs beyond the U-Boot replicated log.
 * The latter ends at +0x3c000; this page belongs to the same reserved pool.
 */
static void probe(unsigned stage, uint64_t value)
{
#if CONFIG_KSHIM_DEBUG_MARKS
    uint64_t base = CONFIG_KSHIM_DEBUG_RAMOOPS_BASE + 0x3f000;
    if (stage > 10 || (kshim_mmu_is_enabled() &&
        !kshim_mmu_is_mapped(base, 128, KSHIM_MMU_WRITE))) return;
    volatile uint64_t *p = (void *)(uintptr_t)base;
    if (!stage) { p[0] = UINT64_C(0x314543494f484343); p[1] = 0; }
    p[2 + stage] = value; p[1] |= UINT64_C(1) << stage;
    __asm__ volatile("dsb sy" ::: "memory");
    for (unsigned i = 0; i < 128; i += 64)
        __asm__ volatile("dc cvac, %0" :: "r"(base + i) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
#else
    (void)stage; (void)value;
#endif
}

static int overlap(uint64_t a, uint64_t n, uint64_t b, uint64_t m)
{
    return !n || !m || a > UINT64_MAX - n || b > UINT64_MAX - m ||
           (a < b + m && b < a + n);
}
static int dma_audit(const void *fdt, uint64_t image, uint64_t size)
{
    int memory = 0, node, len;
    fdt_for_each_subnode(node, fdt, 0) {
        const char *type = fdt_getprop(fdt, node, "device_type", &len);
        if (!type || len != 7 || memcmp(type, "memory", 7)) continue;
        for (unsigned i = 0; i < 128; ++i) {
            struct dt_range r;
            /* ABL leaves zero-sized holes between valid memory tuples.
             * dt_reg() rejects those; raw_reg distinguishes an empty tuple
             * from reaching the end of the list. Root memory is physical. */
            if (dt_raw_reg(fdt, node, i, &r.base, &r.size)) break;
            if (dt_contains(r, OPD_DMA_BASE, OPD_DMA_SIZE)) memory = 1;
        }
    }
    struct dt_range ranges[128]; bool no_map[128]; size_t count;
    if (!memory || dt_reservations(fdt, ranges, no_map, 128, &count)) return -1;
    for (size_t i = 0; i < count; ++i)
        if (overlap(OPD_DMA_BASE, OPD_DMA_SIZE, ranges[i].base, ranges[i].size)) return -1;
    struct dt_range scratch = kshim_resources_get()->ui_scratch;
    if (overlap(OPD_DMA_BASE, OPD_DMA_SIZE, image, size) ||
        overlap(OPD_DMA_BASE, OPD_DMA_SIZE, (uintptr_t)fdt, fdt_totalsize(fdt)) ||
        overlap(OPD_DMA_BASE, OPD_DMA_SIZE, (uintptr_t)__image_start_address__, KSHIM_RUNTIME_SLOT_SIZE) ||
        (scratch.size && overlap(OPD_DMA_BASE, OPD_DMA_SIZE, scratch.base, scratch.size)))
        return -1;
    return 0;
}
static uint32_t rd32(uint64_t addr, void *ctx)
{
    (void)ctx; return *(volatile uint32_t *)(uintptr_t)addr;
}
static void sync_io(void *ctx) { (void)ctx; __asm__ volatile("dsb sy" ::: "memory"); }
static void wait_us(unsigned us, void *ctx) { (void)ctx; (void)delay(us); }
static void wr32(uint64_t addr, uint32_t value, void *ctx)
{
    (void)ctx;
    switch (addr - OPD_UFS_BASE) {
    case 0x20:
        if (value != 1 && value != 0x400 && value != 0x20 && value != 0x40) goto bad;
        break;
    case 0x50: case 0x54: case 0x58: case 0x60: break;
    case 0x90: if (value != 1 && value != 0x17 && value != 0x18) goto bad; break;
    case 0x94: case 0x98: case 0x9c: break;
    default: goto bad;
    }
    *(volatile uint32_t *)(uintptr_t)addr = value;
    sync_io(NULL); return;
bad:
    printf("boot choice: refused MMIO write\n");
    for (;;) (void)delay(100000);
}
static const struct opd_ufs_io io = {rd32, wr32, sync_io, wait_us, NULL};

static int header_ok(const uint8_t *h, uint64_t self, uint64_t alt,
                      uint64_t table, uint64_t last, const uint8_t *entries)
{
    uint8_t copy[92];
    unsigned count = choice_u32(h + 80);
    uint64_t table_blocks = ((uint64_t)count * 128 + CHOICE_PAGE - 1) / CHOICE_PAGE;
    if (memcmp(h, "EFI PART", 8) || choice_u32(h + 8) != 0x10000 ||
        choice_u32(h + 12) != 92 || choice_u32(h + 20) ||
        choice_u64(h + 24) != self || choice_u64(h + 32) != alt ||
        !count || count > 128 || choice_u64(h + 40) < 2 + table_blocks ||
        choice_u64(h + 48) >= last - table_blocks ||
        choice_u64(h + 40) > choice_u64(h + 48) ||
        choice_u64(h + 72) != table ||
        choice_u32(h + 84) != 128 ||
        choice_crc(entries, count * 128) != choice_u32(h + 88)) return 0;
    memcpy(copy, h, sizeof(copy)); memset(copy + 16, 0, 4);
    return choice_crc(copy, sizeof(copy)) == choice_u32(h + 16);
}
static int name_is(const uint8_t *entry, const char *name)
{
    for (unsigned i = 0; i < 36; ++i) {
        uint8_t want = *name;
        if (entry[56 + 2 * i] != want || entry[57 + 2 * i]) return 0;
        if (want) ++name;
    }
    return !*name;
}
static int locate_esp(uint64_t *start, uint64_t *blocks, uint8_t guid[16])
{
    static const uint8_t esp_type[16] = {
        0x28,0x73,0x2a,0xc1,0x1f,0xf8,0xd2,0x11,0xba,0x4b,0x00,0xa0,0xc9,0x3e,0xc9,0x3b};
    struct opd_ufs_read_lun lun = {.lun = 0};
    if (opd2413_ufs_capacity(&session, &lun) || !lun.capacity_ok ||
        lun.block != CHOICE_PAGE || lun.last_lba < 11 || lun.last_lba > UINT32_MAX ||
        opd2413_ufs_read(&session, 0, 0, 6, primary) ||
        opd2413_ufs_read(&session, 0, lun.last_lba - 4, 5, backup)) return -1;
    unsigned count = choice_u32(primary + CHOICE_PAGE + 80);
    if (!count || count > 128) return -1;
    unsigned table_blocks = (count * 128 + CHOICE_PAGE - 1) / CHOICE_PAGE;
    uint64_t backup_lba = choice_u64(backup + 16384 + 72);
    if (choice_u32(backup + 16384 + 80) != count ||
        backup_lba < lun.last_lba - 4 || backup_lba > lun.last_lba - table_blocks) return -1;
    uint64_t table_offset = (backup_lba - (lun.last_lba - 4)) * CHOICE_PAGE;
    if (table_offset + (uint64_t)count * 128 > 16384) return -1;
    const uint8_t *backup_table = backup + table_offset;
    if (!header_ok(primary + CHOICE_PAGE, 1, lun.last_lba, 2, lun.last_lba, primary + 8192) ||
        !header_ok(backup + 16384, lun.last_lba, 1, backup_lba, lun.last_lba, backup_table) ||
        choice_u64(backup + 16384 + 48) >= backup_lba ||
        memcmp(primary + 8192, backup_table, count * 128) ||
        memcmp(primary + CHOICE_PAGE + 40, backup + 16384 + 40, 32)) return -1;
    unsigned found = 0, selected = 0;
    for (unsigned i = 0; i < count; ++i) {
        const uint8_t *e = primary + 8192 + i * 128;
        if (!name_is(e, "ARMADA")) continue;
        if (++found != 1 || memcmp(e, esp_type, 16)) return -1;
        selected = i;
        *start = choice_u64(e + 32);
        uint64_t end = choice_u64(e + 40);
        if (*start < choice_u64(primary + CHOICE_PAGE + 40) || end < *start ||
            end > choice_u64(primary + CHOICE_PAGE + 48) ||
            end - *start + 1 != (512ULL << 20) / CHOICE_PAGE) return -1;
        *blocks = end - *start + 1; memcpy(guid, e + 16, 16);
    }
    if (found != 1) return -1;
    for (unsigned i = 0; i < count; ++i) {
        const uint8_t *e = primary + 8192 + i * 128;
        uint8_t used = 0; for (unsigned j = 0; j < 16; ++j) used |= e[j];
        if (!used || i == selected) continue;
        uint64_t first = choice_u64(e + 32), last = choice_u64(e + 40);
        if (first < choice_u64(primary + CHOICE_PAGE + 40) || last < first ||
            last > choice_u64(primary + CHOICE_PAGE + 48) ||
            overlap(*start, *blocks, first, last - first + 1)) return -1;
    }
    return 0;
}
struct media { uint64_t start, blocks; };
static int read_bytes(void *ctx, uint64_t offset, void *out, size_t n)
{
    struct media *m = ctx;
    if (offset > m->blocks * CHOICE_PAGE || n > m->blocks * CHOICE_PAGE - offset) return -1;
    uint8_t *dest = out;
    while (n) {
        uint64_t lba = m->start + offset / CHOICE_PAGE;
        unsigned at = offset % CHOICE_PAGE;
        size_t part = CHOICE_PAGE - at; if (part > n) part = n;
        if (!cache_valid || cache_lba != lba) {
            if (opd2413_ufs_read(&session, 0, lba, 1, cache)) return -1;
            cache_lba = lba; cache_valid = 1;
        }
        memcpy(dest, cache + at, part); dest += part; offset += part; n -= part;
    }
    return 0;
}
static int begin(uint64_t image, uint64_t size)
{
    const void *fdt = kshim_resources_get()->fdt;
    uint32_t sid;
    /* The ABL FDT and packed image remain immutable until payload copies.
     * Audit once while the caller's boot map is still cacheable; walking
     * this large DT with the MMU off after menu shutdown is very slow. */
    if (!saved.audited) {
        if (opd2413_ufs_resources(fdt) || opd2413_smmu_resources(fdt, &sid) || sid != 0x60 ||
            dma_audit(fdt, image, size)) return 1;
        saved.audited = 1; saved.fdt = fdt;
    } else if (fdt != saved.fdt || image != saved.image || size != saved.image_size) return 1;
    if (kshim_mmu_boot_disable() || kshim_mmu_is_enabled() || kshim_mmu_reset_disabled()) return -2;
    kshim_mmu_region_t maps[] = {
        {0, 0x80000000, KSHIM_MMU_DEVICE, KSHIM_MMU_READ | KSHIM_MMU_WRITE},
        /* The attached runtime executes inside this packed image. Match
         * its automatic RWX mapping, as core main.c boot_map() does. */
        {image, size, KSHIM_MMU_NORMAL_WB, KSHIM_MMU_READ | KSHIM_MMU_WRITE | KSHIM_MMU_EXECUTE},
        {(uintptr_t)fdt, fdt_totalsize(fdt), KSHIM_MMU_NORMAL_WB, KSHIM_MMU_READ},
        {OPD_DMA_BASE, OPD_DMA_SIZE, KSHIM_MMU_NORMAL_NC, KSHIM_MMU_READ | KSHIM_MMU_WRITE},
#if CONFIG_KSHIM_DEBUG_MARKS
        {CONFIG_KSHIM_DEBUG_RAMOOPS_BASE, CONFIG_KSHIM_DEBUG_RAMOOPS_SIZE,
         KSHIM_MMU_NORMAL_NC, KSHIM_MMU_READ | KSHIM_MMU_WRITE},
#endif
    };
    if (kshim_mmu_boot_enable(maps, sizeof(maps) / sizeof(*maps))) return 1;
    cache_valid = 0;
    int ret = opd2413_ufs_open(&session, &io, (void *)(uintptr_t)OPD_DMA_BASE, OPD_UFS_WAKE, &result);
    return ret < 0 ? -2 : ret;
}
static int finish(int ret)
{
    if (session.broken || (session.open && opd2413_ufs_close(&session))) ret = -2;
    if (kshim_mmu_boot_disable()) ret = -2;
    return ret;
}
uint32_t kshim_boot_choice_initial(uint64_t image, const MANIFEST *manifest,
                                  const MANIFEST_ENTRY *const *entries)
{
    memset(&saved, 0, sizeof(saved));
    probe(0, image);
    saved.image = image; saved.image_size = choice_u64((void *)(uintptr_t)(image + 0x10));
    saved.manifest = manifest;
    uint64_t size = choice_u64((void *)(uintptr_t)(image + 0x28));
    probe(1, size);
    printf("boot choice: initial image=%#llx size=%llu manifest=%#llx bytes=%llu entries=%u\n",
           (unsigned long long)image, (unsigned long long)saved.image_size,
           (unsigned long long)(uintptr_t)manifest, (unsigned long long)size,
           manifest->header.entry_count);
    if (size > CHOICE_PAGE || !size) return manifest->header.default_entry;
    for (unsigned i = 0; i + 1 < manifest->header.entry_count; ++i)
        if (entries[i]->flags & MF_ENTRY_FLAG_EXECUTABLE)
            saved.hashes[i] = choice_crc(entries[i], sizeof(*entries[i]) + entries[i]->name_len);
    int ret = begin(image, saved.image_size);
    probe(2, ret);
    if (ret) {
        ret = finish(ret);
        if (ret < 0) { printf("boot choice: unsafe initial UFS state\n"); for (;;) (void)delay(100000); }
        return manifest->header.default_entry;
    }
    ret = locate_esp(&saved.esp_start, &saved.esp_blocks, saved.guid);
    probe(3, ret);
    struct media m = {saved.esp_start, saved.esp_blocks};
    if (!ret) ret = choice_fat_locate(read_bytes, &m, m.blocks * CHOICE_PAGE, &saved.page_offset);
    probe(4, ret);
    if (!ret) ret = read_bytes(&m, saved.page_offset, saved.pages, sizeof(saved.pages));
    if (!ret) ret = read_bytes(&m, saved.page_offset + CHOICE_BYTES,
                                saved.provision, sizeof(saved.provision));
    if (!ret && !choice_provision_valid(saved.provision, saved.guid, saved.page_offset)) {
        printf("boot choice: missing or mismatched provision marker\n"); ret = 1;
    }
    ret = finish(ret ? 1 : 0);
    probe(5, ret);
    if (ret < 0) { printf("boot choice: initial UFS not restored\n"); for (;;) (void)delay(100000); }
    if (ret) { printf("boot choice: ESP journal unavailable\n"); return manifest->header.default_entry; }
    saved.context = choice_crc(manifest, size) ^ choice_crc(saved.guid, sizeof(saved.guid)) ^
                    choice_crc(saved.provision + 48, 16);
    saved.valid = choice_latest(saved.pages, saved.context, saved.hashes,
                                 manifest->header.entry_count - 1, &saved.record);
    saved.ready = saved.valid >= 0;
    probe(6, saved.valid);
    printf("boot choice: journal %d, entry %u, ESP page %llu\n", saved.valid,
           saved.valid == 1 ? saved.record.entry : manifest->header.default_entry,
           (unsigned long long)saved.page_offset);
    return saved.valid == 1 ? saved.record.entry : manifest->header.default_entry;
}
int kshim_boot_choice_commit(uint64_t image, const MANIFEST *manifest,
                             const MANIFEST_ENTRY *const *entries, uint32_t selected)
{
    (void)entries;
    probe(7, selected);
    if (!saved.ready || image != saved.image || manifest != saved.manifest ||
        selected >= manifest->header.entry_count - 1 || !saved.hashes[selected]) return 1;
    if ((saved.valid == 1 && saved.record.entry == selected) ||
        (!saved.valid && selected == manifest->header.default_entry)) return 0;
    if (saved.valid == 1 && saved.record.sequence == UINT64_MAX) return 1;
    int ret = begin(image, saved.image_size);
    probe(8, ret);
    if (ret) return finish(ret);
    uint64_t start = 0, blocks = 0, offset = 0; uint8_t guid[16];
    ret = locate_esp(&start, &blocks, guid);
    struct media m = {start, blocks};
    if (!ret) ret = choice_fat_locate(read_bytes, &m, blocks * CHOICE_PAGE, &offset);
    if (!ret && (start != saved.esp_start || blocks != saved.esp_blocks ||
        offset != saved.page_offset || memcmp(guid, saved.guid, 16))) ret = 1;
    if (!ret) ret = read_bytes(&m, offset, current, sizeof(current));
    if (!ret && memcmp(current, saved.pages, sizeof(current))) ret = 1;
    if (!ret) ret = read_bytes(&m, offset + CHOICE_BYTES, provision_check, sizeof(provision_check));
    if (!ret && memcmp(provision_check, saved.provision, sizeof(provision_check))) ret = 1;
    probe(9, ret);
    if (ret) return finish(1);
    unsigned slot = saved.valid == 1 ? 1U - saved.record.slot : 0;
    choice_encode(next_page, saved.context, saved.hashes[selected],
                  saved.valid == 1 ? saved.record.sequence + 1 : 1, selected);
    session.choice_lun = 0;
    session.choice_lba[0] = start + offset / CHOICE_PAGE;
    session.choice_lba[1] = session.choice_lba[0] + 1;
    session.choice_ready = 1;
    probe(10, session.choice_lba[slot]);
#if CONFIG_KSHIM_OPD2413_CHOICE_WRITE
    ret = opd2413_ufs_write_choice(&session, 0, session.choice_lba[slot], next_page);
    cache_valid = 0;
    if (!ret) ret = opd2413_ufs_read(&session, 0, session.choice_lba[slot], 1, cache);
    if (!ret && memcmp(cache, next_page, CHOICE_PAGE)) ret = 1;
    printf("boot choice: FUA journal slot %u, LBA %u, entry %u, result %d\n",
           slot, session.choice_lba[slot], selected, ret);
#else
    printf("boot choice: DRYRUN slot %u, LBA %u, entry %u\n", slot, session.choice_lba[slot], selected);
    ret = 1;
#endif
    return finish(ret ? 1 : 0);
}
