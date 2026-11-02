/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef OPD2413_LAYOUT_H
#define OPD2413_LAYOUT_H
#include <stdint.h>
#include <stddef.h>

#define OPD_MAX_RANGES 128
struct opd_range { uint64_t base, size; };

static inline int opd_range_valid(struct opd_range r)
{
	return r.size && !(r.base & 4095) && !(r.size & 4095) &&
		r.base <= ((uint64_t)-1) - r.size;
}

/* Subtract a reservation without truncating addresses above 4 GiB. */
static inline int opd_carve(struct opd_range *ranges, size_t *count, struct opd_range cut)
{
	if (!cut.size) return 0;
	if (!opd_range_valid(cut)) return -1;
	for (size_t i = 0; i < *count; ++i) {
		struct opd_range old = ranges[i];
		uint64_t end = old.base + old.size, stop = cut.base + cut.size;
		if (cut.base >= end || stop <= old.base) continue;
		uint64_t left = cut.base > old.base ? cut.base - old.base : 0;
		uint64_t right = stop < end ? end - stop : 0;
		if (left && right) {
			if (*count == OPD_MAX_RANGES) return -1;
			ranges[i].size = left;
			ranges[(*count)++] = (struct opd_range){stop, right};
		} else if (left) ranges[i].size = left;
		else if (right) ranges[i] = (struct opd_range){stop, right};
		else { ranges[i--] = ranges[--*count]; }
	}
	return 0;
}

static inline int opd_contains(const struct opd_range *ranges, size_t count,
			uint64_t base, uint64_t size)
{
	if (!size || base > ((uint64_t)-1) - size) return 0;
	for (size_t i = 0; i < count; ++i)
		if (base >= ranges[i].base && size <= ranges[i].size &&
		    base - ranges[i].base <= ranges[i].size - size) return 1;
	return 0;
}
#endif
