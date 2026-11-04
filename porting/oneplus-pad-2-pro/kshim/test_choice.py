#!/usr/bin/env python3
"""Run native journal, FAT ownership, UFS wire and actual GPT-parser checks."""
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import uuid
import zlib

ROOT = Path(__file__).resolve().parent
UFS = ROOT.parent / 'uboot'


def function(source, name):
    start = source.index('static int ' + name + '(')
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def run(command):
    result = subprocess.run([str(x) for x in command], capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(' '.join(map(str, command)) + '\n' +
                           (result.stdout + result.stderr)[-6000:])
    return result.stdout


def snapshot(count, packed=False):
    block, last = 4096, 2000000
    disk = bytearray(11 * block)
    entries = bytearray(count * 128)
    e = bytearray(128)
    e[:16] = uuid.UUID('c12a7328-f81f-11d2-ba4b-00a0c93ec93b').bytes_le
    e[16:32] = bytes(range(16))
    struct.pack_into('<QQ', e, 32, 256, 256 + 131072 - 1)
    e[56:68] = 'ARMADA'.encode('utf-16-le')
    entries[15 * 128:16 * 128] = e
    table_blocks = (len(entries) + block - 1) // block
    backup_lba = last - table_blocks if packed else last - 4

    def header(self_lba, other, table_lba):
        h = bytearray(block)
        h[:8] = b'EFI PART'
        struct.pack_into('<II', h, 8, 0x10000, 92)
        struct.pack_into('<QQQQ', h, 24, self_lba, other, 6, last - 5)
        h[56:72] = bytes([42]) * 16
        struct.pack_into('<QIII', h, 72, table_lba, count, 128, zlib.crc32(entries))
        struct.pack_into('<I', h, 16, zlib.crc32(h[:92]))
        return h
    disk[block:2 * block] = header(1, last, 2)
    disk[2 * block:2 * block + len(entries)] = entries
    off = 6 * block + (backup_lba - (last - 4)) * block
    disk[off:off + len(entries)] = entries
    disk[10 * block:11 * block] = header(last, 1, backup_lba)
    return disk


GPT_HEAD = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "choice_journal.h"
#include "opd2413_ufs_read.h"
static uint8_t primary[6*4096],backup[5*4096],wire[11*4096];
static struct opd_ufs_session session;
static uint64_t last;
int opd2413_ufs_capacity(struct opd_ufs_session*s,struct opd_ufs_read_lun*l){
 (void)s;l->capacity_ok=1;l->block=4096;l->last_lba=last;return 0;}
int opd2413_ufs_read(struct opd_ufs_session*s,uint8_t lun,uint64_t lba,uint32_t blocks,void*b){
 (void)s;assert(!lun);if(!lba&&blocks==6)memcpy(b,wire,6*4096);
 else{assert(lba==last-4&&blocks==5);memcpy(b,wire+6*4096,5*4096);}return 0;}
'''
GPT_MAIN = r'''
int main(int argc,char**argv){assert(argc==3);FILE*f=fopen(argv[1],"rb");assert(f);
 assert(fread(wire,1,sizeof(wire),f)==sizeof(wire));fclose(f);
 last=choice_u64(wire+4096+32);uint64_t start=0,blocks=0;uint8_t guid[16];
 int ret=locate_esp(&start,&blocks,guid);assert((ret==0)==atoi(argv[2]));
 if(!ret){assert(start==256&&blocks==131072);}
 return 0;}
'''


def main():
    with tempfile.TemporaryDirectory() as directory:
        out = Path(directory)
        for header in ('ufs_read', 'ufs_nop', 'ufs_link', 'ufs_snapshot', 'smmu_snapshot'):
            shutil.copyfile(UFS / (header + '.h'), out / ('opd2413_' + header + '.h'))
        flags = ['cc', '-O1', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                 '-fno-pie', '-no-pie', '-I', ROOT, '-I', out]
        models = [
            ('journal', [ROOT / 'choice_journal_test.c'], []),
            ('fat', [ROOT / 'choice_fat.c', ROOT / 'choice_fat_test.c'], []),
            ('ufs-read', [UFS / (name + '.c') for name in
                          ('ufs_read_test', 'ufs_read', 'ufs_nop', 'ufs_link')], []),
            ('ufs-write', [UFS / (name + '.c') for name in
                           ('ufs_read_test', 'ufs_read', 'ufs_nop', 'ufs_link')],
             ['-DOPD2413_BOOT_CHOICE_WRITER=1']),
        ]
        for name, sources, extra in models:
            run(flags + extra + sources + ['-o', out / name])
            print(run([out / name]).strip().splitlines()[-1])
        source = (ROOT / 'boot_choice.c').read_text()
        gpt = GPT_HEAD + '\n'.join(function(source, name) for name in
                                   ('overlap', 'header_ok', 'name_is', 'locate_esp')) + GPT_MAIN
        (out / 'gpt.c').write_text(gpt)
        run(flags + [out / 'gpt.c', '-o', out / 'gpt'])
        cases = [('gpt32', snapshot(32), 1), ('gpt32-packed', snapshot(32, True), 1),
                 ('gpt128', snapshot(128), 1)]
        bad = snapshot(32, True)
        struct.pack_into('<I', bad, 10 * 4096 + 80, 128)
        cases.append(('mismatched-backup-count-at-end', bad, 0))
        bad = snapshot(32)
        bad[6 * 4096 + 15 * 128] ^= 1
        cases.append(('corrupt-backup-table', bad, 0))
        for name, data, expected in cases:
            path = out / (name + '.bin')
            path.write_bytes(data)
            run([out / 'gpt', path, expected])
        print('GPT: 32/128 entries, declared backup location, CRC and mismatched-count bounds passed')


if __name__ == '__main__':
    main()
