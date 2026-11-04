#!/usr/bin/env python3
"""opd_gpt.py and opd_install.py against a synthetic 4 KiB-sector LUN0."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import uuid
import zlib

ROOT = Path(__file__).resolve().parents[1]
LINUX = ROOT / 'porting/oneplus-pad-2-pro/linux'


def load(name):
    spec = importlib.util.spec_from_file_location(name, LINUX / f'{name}.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


gpt = load('opd_gpt')
inst = load('opd_install')
S = 4096
LBAS = 8 * 2**30 // S          # an 8 GiB LUN keeps the image small
ANDROID = [('persist', 8, 16391), ('metadata', 16392, 32775), ('super', 32776, 300000)]


def crc(data):
    return zlib.crc32(data) & 0xffffffff


def make_gpt(lbas=LBAS):
    """Protective MBR, header, 32x128 entry array; Android parts then userdata to the end."""
    last = lbas - 6
    entries = bytearray(32 * 128)
    parts = ANDROID + [('userdata', 300001, last)]
    for i, (name, first, end) in enumerate(parts):
        e = memoryview(entries)[i * 128:(i + 1) * 128]
        e[:16] = uuid.uuid4().bytes_le
        e[16:32] = uuid.uuid4().bytes_le
        struct.pack_into('<QQQ', e, 32, first, end, 0)
        e[56:56 + 2 * len(name)] = name.encode('utf-16le')
    disk_guid = uuid.uuid4().bytes_le

    def header(current, backup, entries_lba):
        h = bytearray(92)
        h[:8] = b'EFI PART'
        struct.pack_into('<III', h, 8, 0x10000, 92, 0)
        struct.pack_into('<QQQQ', h, 24, current, backup, 6, last)
        h[56:72] = disk_guid
        struct.pack_into('<QIII', h, 72, entries_lba, 32, 128, crc(entries))
        struct.pack_into('<I', h, 16, crc(h))
        return bytes(h) + bytes(S - 92)

    mbr = bytearray(S)
    mbr[450] = 0xee                                      # protective MBR entry
    struct.pack_into('<II', mbr, 454, 1, min(lbas - 1, 0xffffffff))
    mbr[510:512] = b'\x55\xaa'
    primary = bytes(mbr) + header(1, lbas - 1, 2) + bytes(entries) + bytes(3 * S)
    backup = bytes(entries) + bytes(3 * S) + header(lbas - 1, 1, lbas - 5)
    return primary, backup


class GptTests(unittest.TestCase):
    def setUp(self):
        self.primary, self.backup = make_gpt()
        self.table = gpt.Gpt(self.primary, self.backup)

    def test_plan_changes_only_userdata_end_new_entries_and_crcs(self):
        raw, change = gpt.plan(self.table, 2, 'seed')
        primary, backup = self.table.rebuild(bytes(raw))
        new = gpt.verify_change(self.table, primary, backup, change)
        names = [e['name'] for e in new.entries()]
        self.assertEqual(names, ['persist', 'metadata', 'super', 'userdata', 'ARMADA', 'ARMADA_BOOT', 'ARMADA_ROOT'])
        created = {c['name']: c for c in change['created']}
        mib = 2**20 // S
        self.assertEqual(created['ARMADA']['first'] % mib, 0)
        self.assertEqual(created['ARMADA']['last'] - created['ARMADA']['first'] + 1, 512 * mib)
        self.assertEqual(created['ARMADA_BOOT']['last'] - created['ARMADA_BOOT']['first'] + 1, 1024 * mib)
        self.assertEqual(created['ARMADA_ROOT']['last'] + 1, (self.table.ph['last'] + 1) // mib * mib)
        self.assertEqual(change['userdata']['new_last'] + 1, created['ARMADA']['first'])
        self.assertEqual(sum(c['last'] - c['first'] + 1 for c in change['created']), 2 * 2**30 // S)
        # Untouched Android entries are byte-identical; the MBR and padding too.
        self.assertEqual(primary[:S], self.primary[:S])
        self.assertEqual(bytes(new.entries_raw[:3 * 128]), bytes(self.table.entries_raw[:3 * 128]))
        self.assertEqual(primary[3 * S:], self.primary[3 * S:])

    def test_tampered_change_is_rejected(self):
        raw, change = gpt.plan(self.table, 2, 'seed')
        raw[5] ^= 1                              # inside the persist entry
        primary, backup = self.table.rebuild(bytes(raw))
        with self.assertRaises(gpt.GptError):
            gpt.verify_change(self.table, primary, backup, change)

    def test_refuses_second_plan_and_corrupt_tables(self):
        raw, change = gpt.plan(self.table, 2, 'seed')
        primary, backup = self.table.rebuild(bytes(raw))
        with self.assertRaisesRegex(gpt.GptError, 'not the last partition|already exist'):
            gpt.plan(gpt.Gpt(primary, backup), 2, 'seed')
        broken = bytearray(self.primary)
        broken[2 * S + 60] ^= 1
        with self.assertRaisesRegex(gpt.GptError, 'differ|CRC'):
            gpt.Gpt(bytes(broken), self.backup)
        with self.assertRaisesRegex(gpt.GptError, 'no userdata'):
            gpt.plan(self.table, 9, 'seed')

    def test_fdisk_reads_the_result(self):
        raw, change = gpt.plan(self.table, 2, 'seed')
        primary, backup = self.table.rebuild(bytes(raw))
        with tempfile.NamedTemporaryFile() as image:
            image.truncate(LBAS * S)
            image.seek(0); image.write(primary)
            image.seek((LBAS - 5) * S); image.write(backup)
            image.flush()
            out = subprocess.run(['sfdisk', '--sector-size', '4096', '-V', image.name], capture_output=True, text=True)
            self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
            dump = subprocess.run(['sfdisk', '--sector-size', '4096', '-d', image.name], capture_output=True, text=True).stdout
            self.assertIn('name="ARMADA_ROOT"', dump)
            self.assertIn('type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B', dump)


class InstallTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        t = self.t = Path(self.tmp.name)
        primary, backup = make_gpt()
        self.plan_dir = t / 'plan'
        table = gpt.Gpt(primary, backup)
        raw, change = gpt.plan(table, 2, 'seed')
        new_primary, new_backup = table.rebuild(bytes(raw))
        self.plan_dir.mkdir()
        (self.plan_dir / 'primary.bin').write_bytes(new_primary)
        (self.plan_dir / 'backup.bin').write_bytes(new_backup)
        change.update({'disk_lbas': LBAS, 'backup_region_lba': LBAS - 5,
                       'old_primary_sha256': hashlib.sha256(primary).hexdigest(),
                       'old_backup_sha256': hashlib.sha256(backup).hexdigest(),
                       'new_primary_sha256': hashlib.sha256(new_primary).hexdigest(),
                       'new_backup_sha256': hashlib.sha256(new_backup).hexdigest()})
        (self.plan_dir / 'plan.json').write_text(json.dumps(change))
        # Fake sysfs: UFS LUN0 (sda), UFS LUN4 (sde), a USB disk (sdg).
        self.sys, self.dev = t / 'sys', t / 'dev'
        self.dev.mkdir()
        for name, path in (('sda', 'soc@0/1d84000.ufs/host0/target0:0:0/0:0:0:0'),
                           ('sde', 'soc@0/1d84000.ufs/host0/target0:0:0/0:0:0:4'),
                           ('sdg', 'soc@0/a600000.usb/host1/target1:0:0/1:0:0:0')):
            device = self.sys / 'devices/platform' / path
            block = device / 'block' / name
            (block / 'queue').mkdir(parents=True)
            (block / 'device').symlink_to(device)
            (block / 'size').write_text(f'{LBAS * 8}\n')
            (block / 'queue/logical_block_size').write_text('4096\n')
            (self.sys / 'class/block').mkdir(parents=True, exist_ok=True)
            (self.sys / 'class/block' / name).symlink_to(block)
        image = self.dev / 'sda'
        with image.open('wb') as stream:
            stream.truncate(LBAS * S)
            stream.write(primary)
            stream.seek((LBAS - 5) * S)
            stream.write(backup)
        self.marker = b'android-data'
        with image.open('r+b') as stream:   # data inside userdata must survive
            stream.seek(400000 * S); stream.write(self.marker)

    def tearDown(self):
        self.tmp.cleanup()

    def disk(self):
        return inst.Disk(inst.lun0(self.sys), self.plan_dir, self.sys, self.dev)

    def test_lun0_is_the_ufs_lun_0(self):
        self.assertEqual(inst.lun0(self.sys), 'sda')

    def test_gpt_writes_backup_then_primary_once(self):
        disk = self.disk()
        self.assertEqual(disk.state(), 'old')
        writes = []
        real = inst.os.pwrite
        args = argparse.Namespace(write=True)
        with patch.object(inst, 'run'), patch.object(inst, 'require_unused'), \
                patch.object(inst.Disk, 'check_new_partitions'), \
                patch.object(inst.os, 'pwrite', side_effect=lambda fd, data, off: writes.append(off) or real(fd, data, off)):
            inst.cmd_gpt(disk, args)
            self.assertEqual(writes, [(LBAS - 5) * S, 0])
            self.assertEqual(disk.state(), 'new')
            inst.cmd_gpt(disk, args)            # already applied: no further write
            self.assertEqual(len(writes), 2)
        with (self.dev / 'sda').open('rb') as stream:
            stream.seek(400000 * S)
            self.assertEqual(stream.read(len(self.marker)), self.marker)

    def test_gpt_without_write_and_unknown_table_write_nothing(self):
        disk = self.disk()
        with patch.object(inst.os, 'pwrite') as pwrite, patch.object(inst, 'run'):
            inst.cmd_gpt(disk, argparse.Namespace(write=False))
            with (self.dev / 'sda').open('r+b') as stream:
                stream.seek(2 * S + 10); stream.write(b'x')
            with self.assertRaisesRegex(inst.InstallError, 'neither'):
                inst.cmd_gpt(disk, argparse.Namespace(write=True))
            pwrite.assert_not_called()

    def test_deploy_refuses_the_original_table(self):
        with self.assertRaisesRegex(inst.InstallError, 'planned GPT'):
            inst.cmd_deploy(self.disk(), argparse.Namespace(oci=str(self.t / 'x'), write=True))

    def test_bls_entry_and_kargs(self):
        kargs = self.t / 'kargs.d'
        kargs.mkdir()
        (kargs / '10-armada.toml').write_text('match-architectures = ["aarch64"]\nkargs = ["rootwait", "rootflags=x", "quiet"]\n')
        (kargs / '20-x86.toml').write_text('match-architectures = ["x86_64"]\nkargs = ["nope"]\n')
        args = inst.base_kargs(kargs) + inst.DEVICE_KARGS
        self.assertEqual(args, ['rootwait', 'quiet', *inst.DEVICE_KARGS])
        text, pointer = inst.bls_entry('title Fedora\nversion 1\noptions ostree=/ostree/boot.1/default/abc/0 rw\n'
                                       'linux /ostree/default-abc/vmlinuz-7.2.6\ninitrd /ostree/default-abc/initramfs-7.2.6.img\n',
                                       {'root': 'R', 'boot': 'B', 'esp': 'E'}, args)
        self.assertEqual(pointer, 'ostree=/ostree/boot.1/default/abc/0')
        self.assertIn('options root=UUID=R rw boot=UUID=B rootwait quiet clk_ignore_unused pd_ignore_unused '
                      'regulator_ignore_unused selinux=0 rootflags=subvol=root,noatime,compress=zstd:1,discard=async '
                      'ostree=/ostree/boot.1/default/abc/0\n', text)
        self.assertIn('linux /boot/ostree/default-abc/vmlinuz-7.2.6\n', text)


if __name__ == '__main__':
    unittest.main()
