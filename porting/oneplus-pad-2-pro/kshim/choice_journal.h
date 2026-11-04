/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef OPD_BOOT_CHOICE_JOURNAL_H
#define OPD_BOOT_CHOICE_JOURNAL_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define CHOICE_PAGE 4096U
#define CHOICE_BYTES (2U * CHOICE_PAGE)
#define CHOICE_BLOB_NAME "kShimC boot choice"
struct choice_record { uint64_t sequence; uint32_t entry; int slot; };

static inline uint32_t choice_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static inline uint64_t choice_u64(const uint8_t *p)
{
    return choice_u32(p) | (uint64_t)choice_u32(p + 4) << 32;
}
static inline void choice_put32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = v >> (8 * i);
}
static inline void choice_put64(uint8_t *p, uint64_t v)
{
    choice_put32(p, v); choice_put32(p + 4, v >> 32);
}
static inline uint32_t choice_crc(const void *data, size_t n)
{
    const uint8_t *p = data;
    uint32_t crc = ~0U;
    while (n--) {
        crc ^= *p++;
        for (unsigned j = 0; j < 8; ++j)
            crc = (crc >> 1) ^ (0xedb88320U & -(crc & 1));
    }
    return ~crc;
}
static inline int choice_decode(const uint8_t *p, uint32_t context,
                                const uint32_t *entry_hash, unsigned count,
                                struct choice_record *out)
{
    if (memcmp(p, "KSHMCHO1", 8) || choice_u32(p + 8) != 1 ||
        choice_u32(p + 24) != context || !choice_u64(p + 16) ||
        choice_u32(p + 12) >= count ||
        !entry_hash[choice_u32(p + 12)] ||
        choice_u32(p + 28) != entry_hash[choice_u32(p + 12)] ||
        choice_crc(p, CHOICE_PAGE - 4) != choice_u32(p + CHOICE_PAGE - 4))
        return 0;
    for (unsigned i = 32; i < CHOICE_PAGE - 4; ++i)
        if (p[i]) return 0;
    out->sequence = choice_u64(p + 16);
    out->entry = choice_u32(p + 12);
    return 1;
}
/* 1 = record; 0 = no valid record; -1 = conflicting generations. */
static inline int choice_latest(const uint8_t *pages, uint32_t context,
                                const uint32_t *entry_hash, unsigned count,
                                struct choice_record *out)
{
    struct choice_record a = {.slot = 0}, b = {.slot = 1};
    int av = choice_decode(pages, context, entry_hash, count, &a);
    int bv = choice_decode(pages + CHOICE_PAGE, context, entry_hash, count, &b);
    if (!av && !bv) return 0;
    if (av && bv && a.sequence == b.sequence && a.entry != b.entry) return -1;
    *out = !av || (bv && b.sequence > a.sequence) ? b : a;
    return 1;
}
static inline void choice_encode(uint8_t *p, uint32_t context, uint32_t entry_hash,
                                 uint64_t sequence, uint32_t entry)
{
    memset(p, 0, CHOICE_PAGE);
    memcpy(p, "KSHMCHO1", 8); choice_put32(p + 8, 1);
    choice_put32(p + 12, entry); choice_put64(p + 16, sequence);
    choice_put32(p + 24, context); choice_put32(p + 28, entry_hash);
    choice_put32(p + CHOICE_PAGE - 4, choice_crc(p, CHOICE_PAGE - 4));
}
/* Immutable third-page provision marker identifies the owned file extent. */
static inline int choice_provision_valid(const uint8_t *p, const uint8_t guid[16],
                                         uint64_t page_offset)
{
    if (memcmp(p, "KSHPROV1", 8) || choice_u32(p + 8) != 1 ||
        choice_u32(p + 12) != 16384 || memcmp(p + 16, guid, 16) ||
        choice_u64(p + 32) != page_offset || choice_u32(p + 40) != CHOICE_PAGE ||
        choice_u32(p + 44) != 2 ||
        choice_crc(p, CHOICE_PAGE - 4) != choice_u32(p + CHOICE_PAGE - 4)) return 0;
    unsigned nonce = 0;
    for (unsigned i = 48; i < 64; ++i) nonce |= p[i];
    if (!nonce) return 0;
    for (unsigned i = 64; i < CHOICE_PAGE - 4; ++i) if (p[i]) return 0;
    return 1;
}
#endif
