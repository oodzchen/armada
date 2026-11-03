#!/usr/bin/env python3
"""Build the extent-table RAM disk read by CONFIG_OPD2413_UFS_BLK_RAMDISK (QEMU only).

Each --extent LBA:FILE places FILE (whole 4 KiB blocks) at that LBA of one
LUN; every other block reads as zeros. Layout, little endian:
magic "RDSK", count, lun, blocks, count x {lba, count, offset}, data.
"""
import argparse
from pathlib import Path
import struct

BLOCK = 4096


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--lun', type=int, required=True)
    ap.add_argument('--blocks', type=int, required=True, help='LUN capacity in 4 KiB blocks')
    ap.add_argument('--extent', action='append', required=True, metavar='LBA:FILE')
    ap.add_argument('--out', type=Path, required=True)
    a = ap.parse_args()
    extents = []
    for spec in a.extent:
        lba, path = spec.split(':', 1)
        data = Path(path).read_bytes()
        data += bytes(-len(data) % BLOCK)
        lba, count = int(lba, 0), len(data) // BLOCK
        if lba + count > a.blocks:
            raise ValueError(f'{path} ends past the LUN')
        extents.append((lba, count, data))
    extents.sort()
    for (l1, c1, _), (l2, _, _) in zip(extents, extents[1:]):
        if l1 + c1 > l2:
            raise ValueError(f'overlapping extents at LBA {l2}')
    header = 24 + 24 * len(extents)
    offset = -(-header // BLOCK) * BLOCK
    table, body = b'', b''
    for lba, count, data in extents:
        table += struct.pack('<QQQ', lba, count, offset + len(body))
        body += data
    blob = struct.pack('<IIQQ', 0x4b534452, len(extents), a.lun, a.blocks) + table
    a.out.write_bytes(blob + bytes(offset - len(blob)) + body)
    print(f'{a.out}: {len(extents)} extents, {len(body) // BLOCK} blocks of data')


if __name__ == '__main__':
    main()
