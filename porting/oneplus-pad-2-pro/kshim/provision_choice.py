#!/usr/bin/env python3
"""Provision the existing empty KSHIM.CHO through the mounted ESP filesystem.

Only the immutable third page is written. Never writes raw block devices.
The kShimC backend independently validates GPT, FAT mirrors and ownership.
"""
import argparse
import hashlib
import json
import mmap
import os
from pathlib import Path
import struct
import subprocess
import uuid
import zlib


def direct_read(device, offset, count):
    # A buffered raw-block read can retain stale data after a filesystem
    # write to the same sectors. Bypass that separate block-device cache.
    start = offset & ~4095
    length = (offset - start + count + 4095) & ~4095
    fd = os.open(device, os.O_RDONLY | os.O_DIRECT)
    try:
        with mmap.mmap(-1, length) as buf:
            assert os.preadv(fd, [buf], start) == length
            return buf[offset - start:offset - start + count]
    finally:
        os.close(fd)


def provision(mount, device):
    device = device.resolve(strict=True)
    path = mount / 'KSHIM.CHO'
    assert mount.stat().st_dev == device.stat().st_rdev, 'mount/device mismatch'
    fd = os.open(path, os.O_RDWR | os.O_NOFOLLOW)
    try:
        assert os.fstat(fd).st_dev == mount.stat().st_dev
        assert os.fstat(fd).st_size == 16384, 'unexpected file size'
        previous = os.pread(fd, 16384, 0)
        os.fsync(fd)
        os.sync()
        with device.open('rb', buffering=0) as disk:
            b = disk.read(512)
            sector = struct.unpack_from('<H', b, 11)[0]
            spc, fats = b[13], b[16]
            reserved, roots, spf = (struct.unpack_from('<H', b, n)[0] for n in (14, 17, 22))
            assert b[510:512] == b'\x55\xaa' and sector in (512, 1024, 2048, 4096)
            assert fats == 2 and spf and roots and spc and not spc & (spc - 1)
            fat_start = reserved * sector
            fat_size = spf * sector
            disk.seek(fat_start)
            fat = disk.read(fat_size)
            assert disk.read(fat_size) == fat, 'FAT mirrors differ'
            root = fat_start + fats * fat_size
            disk.seek(root)
            directory = disk.read(roots * 32)
            entries = []
            for i in range(roots):
                entry = directory[i * 32:(i + 1) * 32]
                if entry[0] == 0:
                    break
                if entry[:11] == b'KSHIM   CHO':
                    entries.append(entry)
            assert len(entries) == 1, 'journal missing or ambiguous'
            entry = entries[0]
            assert not entry[11] & 0x18 and struct.unpack_from('<H', entry, 20)[0] == 0
            assert struct.unpack_from('<I', entry, 28)[0] == 16384
            first = struct.unpack_from('<H', entry, 26)[0]
            cluster_bytes = spc * sector
            count = (16384 + cluster_bytes - 1) // cluster_bytes
            for i in range(count):
                next_cluster = struct.unpack_from('<H', fat, 2 * (first + i))[0]
                assert next_cluster == first + i + 1 if i + 1 < count else next_cluster >= 0xfff8
            data = root + (roots * 32 + sector - 1) // sector * sector
            file_offset = data + (first - 2) * cluster_bytes
            page_offset = (file_offset + 4095) & ~4095
            marker_in_file = page_offset - file_offset + 8192
            assert 0 <= marker_in_file <= 12288
            assert direct_read(device, file_offset, 16384) == previous, 'raw extent does not match the file'
        partuuid = subprocess.check_output(['blkid', '-s', 'PARTUUID', '-o', 'value',
                                            str(device)], text=True).strip()
        guid = uuid.UUID(partuuid).bytes_le
        marker = previous[marker_in_file:marker_in_file + 4096]
        if previous == bytes(16384):
            marker = bytearray(4096)
            marker[:8] = b'KSHPROV1'
            struct.pack_into('<II', marker, 8, 1, 16384)
            marker[16:32] = guid
            struct.pack_into('<QII', marker, 32, page_offset, 4096, 2)
            marker[48:64] = os.urandom(16)
            struct.pack_into('<I', marker, 4092, zlib.crc32(marker[:4092]))
            assert os.pwrite(fd, marker, marker_in_file) == 4096
            os.fsync(fd)
            os.sync()
        assert marker[:8] == b'KSHPROV1' and struct.unpack_from('<II', marker, 8) == (1, 16384)
        assert marker[16:32] == guid and struct.unpack_from('<QII', marker, 32) == (page_offset, 4096, 2)
        assert any(marker[48:64]) and not any(marker[64:4092])
        assert zlib.crc32(marker[:4092]) == struct.unpack_from('<I', marker, 4092)[0]
        assert os.pread(fd, 4096, marker_in_file) == marker
        assert direct_read(device, page_offset + 8192, 4096) == marker, 'raw readback differs'
        return {'partition_uuid': partuuid, 'page_offset': page_offset,
                'file_offset': file_offset, 'marker_in_file': marker_in_file,
                'marker_sha256': hashlib.sha256(marker).hexdigest(),
                'journal_pages_unchanged': direct_read(device, page_offset, 8192) ==
                                           previous[page_offset - file_offset:page_offset - file_offset + 8192]}
    finally:
        os.close(fd)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mount', type=Path, default=Path('/boot/efi'))
    parser.add_argument('--device', type=Path, default=Path('/dev/disk/by-partlabel/ARMADA'))
    parser.add_argument('--record', type=Path, required=True)
    args = parser.parse_args()
    result = provision(args.mount, args.device)
    assert result['journal_pages_unchanged']
    args.record.write_text(json.dumps(result, indent=2) + '\n')
    print('ESP journal provisioned; marker and unchanged empty state pages verified')


if __name__ == '__main__':
    main()
