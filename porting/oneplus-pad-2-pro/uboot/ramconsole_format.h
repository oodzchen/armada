/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef OPD_RAMCONSOLE_FORMAT_H
#define OPD_RAMCONSOLE_FORMAT_H
#include <stdint.h>
#define OPD_LOG_STRIDE 0xc000U
#define OPD_LOG_COPIES 5U
#define OPD_LOG_LAST ((OPD_LOG_COPIES - 1) * OPD_LOG_STRIDE)
#define OPD_LOG_CAPACITY (OPD_LOG_STRIDE - 8)
#define OPD_LOG_MAGIC 0x43474244U

static inline uint32_t opd_crc_byte(uint32_t crc, unsigned char byte)
{
    crc ^= byte;
    for (unsigned bit = 0; bit < 8; ++bit)
        crc = (crc >> 1) ^ ((0U - (crc & 1U)) & 0xedb88320U);
    return crc;
}
static inline void opd_log_init(volatile uint32_t *h)
{
    volatile unsigned char *body = (volatile unsigned char *)(h + 3);
    if (h[0] == OPD_LOG_MAGIC && h[1] == h[2] &&
        h[1] >= OPD_LOG_LAST + 8 && h[1] <= OPD_LOG_LAST + OPD_LOG_STRIDE)
        return;
    uint32_t length = h[0] == OPD_LOG_MAGIC && h[1] == h[2] && h[2] < OPD_LOG_CAPACITY ? h[2] : 0;
    /* Preserve the single-copy kShimC log while making room for record metadata. */
    for (uint32_t i = length; i; --i) body[i + 7] = body[i - 1];
    uint32_t crc = 0xffffffffU;
    for (uint32_t i = 0; i < length; ++i) crc = opd_crc_byte(crc, body[8 + i]);
    for (unsigned copy = 0; copy < OPD_LOG_COPIES; ++copy) {
        volatile unsigned char *record = body + copy * OPD_LOG_STRIDE;
        volatile uint32_t *meta = (volatile uint32_t *)record;
        if (copy) for (uint32_t i = 0; i < length; ++i) record[8 + i] = body[8 + i];
        meta[0] = length; meta[1] = crc ^ 0xffffffffU;
        /* Do not expose stale RAM in gaps included in the pstore record. */
        if (copy + 1 < OPD_LOG_COPIES)
            for (uint32_t i = 8 + length; i < OPD_LOG_STRIDE; ++i) record[i] = 0;
    }
    h[0] = OPD_LOG_MAGIC;
    h[1] = h[2] = OPD_LOG_LAST + 8 + length;
}
static inline int opd_log_putc(volatile uint32_t *h, unsigned char ch)
{
    volatile unsigned char *body = (volatile unsigned char *)(h + 3);
    volatile uint32_t *first = (volatile uint32_t *)body;
    uint32_t length = first[0];
    if (length >= OPD_LOG_CAPACITY) return 0;
    uint32_t crc = opd_crc_byte(first[1] ^ 0xffffffffU, ch) ^ 0xffffffffU;
    for (unsigned copy = 0; copy < OPD_LOG_COPIES; ++copy) {
        volatile unsigned char *record = body + copy * OPD_LOG_STRIDE;
        volatile uint32_t *meta = (volatile uint32_t *)record;
        record[8 + length] = ch;
        meta[0] = length + 1; meta[1] = crc;
    }
    h[1] = h[2] = OPD_LOG_LAST + 8 + length + 1;
    return 1;
}
#endif
