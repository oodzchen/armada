/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "choice_fat.h"
#include "choice_journal.h"
static unsigned u16(const uint8_t *p) { return p[0] | (unsigned)p[1] << 8; }
static int power2(unsigned n) { return n && !(n & (n - 1)); }
/* Check the complete FAT mirrors and all live directory references. This
 * rejects cross-links into the journal, including a file starting inside
 * its chain without an incoming FAT edge. No file contents are scanned.
 */
static int ownership(choice_read_bytes read, void *ctx, uint64_t fat,
                      uint64_t fat_size, unsigned clusters, uint64_t root,
                      unsigned roots, uint64_t data, uint64_t cluster_bytes,
                      unsigned first, unsigned count, uint64_t own_entry)
{
    static uint8_t a[4096], b[4096], visited[8192];
    unsigned queue[256] = {0}, head = 0, tail = 1, walked = 0;
    memset(visited, 0, sizeof(visited));
    for (uint64_t off = 0; off < fat_size; off += sizeof(a)) {
        size_t n = fat_size - off; if (n > sizeof(a)) n = sizeof(a);
        if (read(ctx, fat + off, a, n) || read(ctx, fat + fat_size + off, b, n) ||
            memcmp(a, b, n)) return -1;
        for (unsigned i = 0; i + 1 < n; i += 2) {
            unsigned from = (off + i) / 2, to = u16(a + i);
            if (from < 2 || from > clusters + 1 || to < first || to >= first + count) continue;
            if (from < first || from >= first + count - 1 || to != from + 1) return -1;
        }
    }
    while (head < tail) {
        unsigned cluster = queue[head++];
        for (;;) {
            if (cluster) {
                if (cluster < 2 || cluster > clusters + 1 || ++walked > 256 ||
                    (cluster >= first && cluster < first + count) ||
                    (visited[cluster / 8] & (1U << (cluster % 8)))) return -1;
                visited[cluster / 8] |= 1U << (cluster % 8);
            }
            uint64_t base = cluster ? data + (uint64_t)(cluster - 2) * cluster_bytes : root;
            unsigned entries = cluster ? cluster_bytes / 32 : roots;
            int end = 0;
            for (unsigned i = 0; i < entries; ++i) {
                uint8_t e[32]; uint64_t where = base + (uint64_t)i * 32;
                if (read(ctx, where, e, sizeof(e))) return -1;
                if (!e[0]) { end = 1; break; }
                if (e[0] == 0xe5 || e[11] == 0x0f || (e[11] & 8)) continue;
                unsigned child = u16(e + 26);
                if (child >= first && child < first + count && where != own_entry) return -1;
                if (!(e[11] & 0x10) || e[0] == '.') continue;
                if (u16(e + 20) || child < 2 || child > clusters + 1 || tail == 256) return -1;
                queue[tail++] = child;
            }
            if (!cluster || end) break;
            uint8_t next[2];
            if (read(ctx, fat + 2 * cluster, next, 2)) return -1;
            cluster = u16(next);
            if (cluster >= 0xfff8) break;
        }
    }
    return 0;
}
int choice_fat_locate(choice_read_bytes read, void *ctx, uint64_t bytes,
                      uint64_t *page_offset)
{
    uint8_t b[512], entry[32], f[2], mirror[2];
    if (!read || !page_offset || read(ctx, 0, b, sizeof(b)) ||
        b[510] != 0x55 || b[511] != 0xaa) return -1;
    unsigned sector = u16(b + 11), spc = b[13], reserved = u16(b + 14);
    unsigned roots = u16(b + 17), spf = u16(b + 22);
    uint64_t sectors = u16(b + 19) ? u16(b + 19) : choice_u32(b + 32);
    if (!power2(sector) || sector < 512 || sector > 4096 ||
        !power2(spc) || spc > 128 || !reserved || b[16] != 2 ||
        !roots || roots > 4096 || !spf || sectors > bytes / sector)
        return -1;
    uint64_t fat = (uint64_t)reserved * sector;
    uint64_t fat_size = (uint64_t)spf * sector;
    uint64_t root = fat + 2 * fat_size;
    uint64_t data = root + ((uint64_t)roots * 32 + sector - 1) / sector * sector;
    uint64_t cluster_bytes = (uint64_t)spc * sector;
    if (data >= sectors * sector) return -1;
    uint64_t clusters = (sectors * sector - data) / cluster_bytes;
    if (clusters < 4085 || clusters >= 65525 || (clusters + 2) * 2 > fat_size)
        return -1; /* This backend deliberately accepts FAT16 only. */
    unsigned first = 0, found = 0; uint64_t own_entry = 0;
    for (unsigned i = 0; i < roots; ++i) {
        if (read(ctx, root + (uint64_t)i * 32, entry, sizeof(entry))) return -1;
        if (!entry[0]) break;
        if (entry[0] == 0xe5 || (entry[11] & 0x18) || entry[11] == 0x0f) continue;
        if (memcmp(entry, "KSHIM   CHO", 11)) continue;
        if (++found != 1 || choice_u32(entry + 28) != 16384 || u16(entry + 20))
            return -1;
        first = u16(entry + 26);
        own_entry = root + (uint64_t)i * 32;
    }
    if (found != 1 || first < 2 || first > clusters + 1) return -1;
    unsigned count = (16384 + cluster_bytes - 1) / cluster_bytes;
    for (unsigned i = 0; i < count; ++i) {
        unsigned cluster = first + i;
        if (cluster > clusters + 1 ||
            read(ctx, fat + 2 * cluster, f, 2) ||
            read(ctx, fat + fat_size + 2 * cluster, mirror, 2) ||
            memcmp(f, mirror, 2)) return -1;
        unsigned next = u16(f);
        if (i + 1 < count ? next != cluster + 1 : next < 0xfff8) return -1;
    }
    uint64_t file = data + (uint64_t)(first - 2) * cluster_bytes;
    uint64_t aligned = (file + CHOICE_PAGE - 1) & ~(uint64_t)(CHOICE_PAGE - 1);
    if (file > sectors * sector - 16384 || aligned - file > 16384 - 3 * CHOICE_PAGE)
        return -1;
    if (ownership(read, ctx, fat, fat_size, clusters, root, roots, data,
                   cluster_bytes, first, count, own_entry)) return -1;
    *page_offset = aligned;
    return 0;
}
