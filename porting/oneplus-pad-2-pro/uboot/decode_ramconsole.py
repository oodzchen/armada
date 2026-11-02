#!/usr/bin/env python3
"""Recover a replicated observer pstore log and reject unresolved corruption."""
import argparse
from collections import Counter
from pathlib import Path
import struct
import zlib
STRIDE = 0xc000
COPIES = 5


def majority(blocks):
    return bytes(sum((1 << bit) for bit in range(8) if sum((v >> bit) & 1 for v in group) >= 3)
                 for group in zip(*blocks))


def decode(data, raw=False):
    if raw:
        magic, start, size = struct.unpack_from('<III', data)
        if magic != 0x43474244 or start != size or size > len(data)-12:
            raise ValueError('Invalid persistent RAM header')
        data = data[12:12+size]
    if len(data) < (COPIES-1)*STRIDE+8:
        raise ValueError('Log lacks five replicas')
    blocks = [data[i*STRIDE:] for i in range(COPIES)]
    length, crc = struct.unpack('<II', majority([b[:8] for b in blocks]))
    if length > STRIDE-8 or any(len(b) < 8+length for b in blocks):
        raise ValueError('Invalid replicated length')
    texts = [b[8:8+length] for b in blocks]
    result = majority(texts)
    if zlib.crc32(result) != crc:
        raise ValueError('CRC mismatch after majority recovery; log remains corrupted')
    changed = sum(a != b for a,b in zip(texts[0],result))
    return result, changed


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('input',type=Path)
    ap.add_argument('--raw',action='store_true',help='Input includes persistent_ram header')
    ap.add_argument('--out',type=Path)
    a=ap.parse_args()
    text, corrected=decode(a.input.read_bytes(),a.raw)
    if a.out:a.out.write_bytes(text)
    else:print(text.decode(errors='replace'))
    print(f'CRC verified; {corrected} bytes recovered relative to first replica')

if __name__=='__main__':main()
