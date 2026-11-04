#!/usr/bin/env python3
"""Byte-level GPT edit for the OPD2413 UFS LUN0 (4 KiB sectors).

Works on the two raw GPT regions only: PRIMARY (LBA 0..5: protective MBR,
header, entry array, zero padding) and BACKUP (the last 5 LBAs: entry
array, zero padding, header). The entry array is 32 x 128 bytes here.
`plan` shrinks the partition named userdata and appends ESP/boot/root
partitions behind it, then recomputes the entry-array and header CRCs.
Everything else - protective MBR, disk GUID, the other entries, header
fields - is kept byte for byte, and `plan` proves that by diffing.

  opd_gpt.py show    PRIMARY BACKUP
  opd_gpt.py plan    PRIMARY BACKUP --armada-gib 256 --out DIR [--guid-seed S]
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
import uuid
import zlib

SECTOR = 4096
PRIMARY_LBAS, BACKUP_LBAS = 6, 5
ESP_TYPE = uuid.UUID('c12a7328-f81f-11d2-ba4b-00a0c93ec93b')
LINUX_TYPE = uuid.UUID('0fc63daf-8483-4772-8e79-3d69d8477de4')
MIB, GIB = 2**20 // SECTOR, 2**30 // SECTOR
# Same layout and names as armada-installer.
ARMADA_PARTS = (('ARMADA', ESP_TYPE, 512 * MIB), ('ARMADA_BOOT', LINUX_TYPE, 1024 * MIB), ('ARMADA_ROOT', LINUX_TYPE, None))


class GptError(Exception):
    pass


def crc(data):
    return zlib.crc32(data) & 0xffffffff


def parse_header(block, label):
    if block[:8] != b'EFI PART':
        raise GptError(f'{label}: no GPT signature')
    size = struct.unpack_from('<I', block, 12)[0]
    header = bytearray(block[:size])
    stored = struct.unpack_from('<I', header, 16)[0]
    struct.pack_into('<I', header, 16, 0)
    if crc(header) != stored:
        raise GptError(f'{label}: header CRC mismatch')
    fields = dict(zip(('current', 'backup', 'first', 'last'), struct.unpack_from('<QQQQ', block, 24)))
    fields['disk_guid'] = uuid.UUID(bytes_le=bytes(block[56:72]))
    fields['entries_lba'], fields['count'], fields['entry_size'], fields['entries_crc'] = struct.unpack_from('<QIII', block, 72)
    fields['size'] = size
    return fields


def entries_of(raw, count, size):
    out = []
    for i in range(count):
        e = raw[i * size:(i + 1) * size]
        if e[:16] == bytes(16):
            continue
        first, last, attrs = struct.unpack_from('<QQQ', e, 32)
        out.append({'number': i + 1, 'type': str(uuid.UUID(bytes_le=bytes(e[:16]))),
                    'guid': str(uuid.UUID(bytes_le=bytes(e[16:32]))), 'first': first, 'last': last,
                    'attrs': attrs, 'name': bytes(e[56:128]).decode('utf-16le').rstrip('\0')})
    return out


class Gpt:
    def __init__(self, primary, backup):
        if len(primary) != PRIMARY_LBAS * SECTOR or len(backup) != BACKUP_LBAS * SECTOR:
            raise GptError('expected 6 primary and 5 backup LBAs of 4 KiB')
        self.primary, self.backup = bytearray(primary), bytearray(backup)
        self.ph = parse_header(self.primary[SECTOR:2 * SECTOR], 'primary')
        self.bh = parse_header(self.backup[4 * SECTOR:], 'backup')
        ph, bh = self.ph, self.bh
        self.array_len = ph['count'] * ph['entry_size']
        if ph['entries_lba'] != 2 or not 0 < self.array_len <= 4 * SECTOR or self.array_len % SECTOR:
            raise GptError('unexpected primary entry array placement')
        if (bh['current'], bh['backup'], bh['entries_lba']) != (ph['backup'], 1, ph['backup'] - 4):
            raise GptError('backup header does not mirror the primary')
        for key in ('first', 'last', 'disk_guid', 'count', 'entry_size', 'entries_crc', 'size'):
            if ph[key] != bh[key]:
                raise GptError(f'primary/backup differ in {key}')
        self.entries_raw = bytearray(self.primary[2 * SECTOR:2 * SECTOR + self.array_len])
        if self.entries_raw != self.backup[:self.array_len]:
            raise GptError('primary and backup entry arrays differ')
        if crc(self.entries_raw) != ph['entries_crc']:
            raise GptError('entry array CRC mismatch')
        self.disk_lbas = ph['backup'] + 1

    def entries(self):
        return entries_of(self.entries_raw, self.ph['count'], self.ph['entry_size'])

    def rebuild(self, entries_raw):
        """New primary/backup regions around a changed entry array."""
        array_crc = crc(entries_raw)
        primary, backup = bytearray(self.primary), bytearray(self.backup)
        primary[2 * SECTOR:2 * SECTOR + self.array_len] = entries_raw
        backup[:self.array_len] = entries_raw
        for region, offset in ((primary, SECTOR), (backup, 4 * SECTOR)):
            size = self.ph['size']
            header = bytearray(region[offset:offset + size])
            struct.pack_into('<I', header, 88, array_crc)
            struct.pack_into('<I', header, 16, 0)
            struct.pack_into('<I', header, 16, crc(header))
            region[offset:offset + size] = header
        return bytes(primary), bytes(backup)


def plan(gpt, armada_gib, guid_seed):
    entries = gpt.entries()
    userdata = [e for e in entries if e['name'] == 'userdata']
    if len(userdata) != 1:
        raise GptError('expected exactly one userdata partition')
    userdata = userdata[0]
    if userdata['last'] != gpt.ph['last'] or max(e['last'] for e in entries) != userdata['last']:
        raise GptError('userdata is not the last partition ending at the last usable LBA')
    if any(e['name'].startswith('ARMADA') for e in entries):
        raise GptError('ARMADA partitions already exist')
    end = (gpt.ph['last'] + 1) // MIB * MIB          # exclusive, 1 MiB aligned
    start = end - armada_gib * GIB
    if start <= userdata['first']:
        raise GptError('Armada size leaves no userdata')
    used = {e['number'] for e in entries}
    numbers = [userdata['number'] + i for i in (1, 2, 3)]
    if any(n in used or n > gpt.ph['count'] for n in numbers):
        raise GptError('partition numbers after userdata are not free')
    raw = bytearray(gpt.entries_raw)
    size = gpt.ph['entry_size']
    struct.pack_into('<Q', raw, (userdata['number'] - 1) * size + 40, start - 1)
    cursor, created = start, []
    for number, (name, ptype, length) in zip(numbers, ARMADA_PARTS):
        last = (end if length is None else cursor + length) - 1
        guid = uuid.UUID(bytes=hashlib.sha256(f'{guid_seed}:{name}'.encode()).digest()[:16], version=4)
        entry = bytearray(size)
        entry[:16], entry[16:32] = ptype.bytes_le, guid.bytes_le
        struct.pack_into('<QQQ', entry, 32, cursor, last, 0)
        entry[56:56 + 2 * len(name)] = name.encode('utf-16le')
        raw[(number - 1) * size:number * size] = entry
        created.append({'number': number, 'name': name, 'first': cursor, 'last': last, 'guid': str(guid)})
        cursor = last + 1
    return raw, {'userdata': {'number': userdata['number'], 'first': userdata['first'],
                              'old_last': userdata['last'], 'new_last': start - 1}, 'created': created}


def verify_change(old, new_primary, new_backup, change):
    """Only the intended bytes differ; return a description of every changed range."""
    new = Gpt(new_primary, new_backup)
    size = old.ph['entry_size']
    allowed = set()
    ud = change['userdata']['number']
    allowed |= set(range((ud - 1) * size + 40, (ud - 1) * size + 48))
    for c in change['created']:
        allowed |= set(range((c['number'] - 1) * size, c['number'] * size))
    diff = [i for i in range(len(old.entries_raw)) if old.entries_raw[i] != new.entries_raw[i]]
    if not set(diff) <= allowed:
        raise GptError(f'unexpected entry bytes changed: {sorted(set(diff) - allowed)[:8]}')
    for region_old, region_new, header_at, label in ((old.primary, new.primary, SECTOR, 'primary'),
                                                     (old.backup, new.backup, 4 * SECTOR, 'backup')):
        array_at = 2 * SECTOR if label == 'primary' else 0
        for i in range(len(region_old)):
            if region_old[i] == region_new[i]:
                continue
            in_array = array_at <= i < array_at + old.array_len
            in_crc = i - header_at in range(16, 20) or i - header_at in range(88, 92)
            if not (in_array or in_crc):
                raise GptError(f'{label}: byte {i} changed outside the entry array and CRCs')
    for key in ('current', 'backup', 'first', 'last', 'disk_guid', 'count', 'entry_size', 'size'):
        if old.ph[key] != new.ph[key] or old.bh[key] != new.bh[key]:
            raise GptError(f'header field {key} changed')
    kept = [e for e in old.entries() if e['number'] != ud]
    if [e for e in new.entries() if e['number'] in {k['number'] for k in kept}] != kept:
        raise GptError('an untouched entry changed')
    return new


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('command', choices=('show', 'plan'))
    ap.add_argument('primary', type=Path)
    ap.add_argument('backup', type=Path)
    ap.add_argument('--armada-gib', type=int)
    ap.add_argument('--out', type=Path)
    ap.add_argument('--guid-seed', default='opd2413-armada')
    a = ap.parse_args()
    gpt = Gpt(a.primary.read_bytes(), a.backup.read_bytes())
    if a.command == 'show':
        print(json.dumps({'disk_guid': str(gpt.ph['disk_guid']), 'first': gpt.ph['first'], 'last': gpt.ph['last'],
                          'backup_lba': gpt.ph['backup'], 'entries': gpt.entries()}, indent=2))
        return
    if not a.armada_gib or not a.out:
        ap.error('plan needs --armada-gib and --out')
    raw, change = plan(gpt, a.armada_gib, a.guid_seed)
    new_primary, new_backup = gpt.rebuild(bytes(raw))
    new = verify_change(gpt, new_primary, new_backup, change)
    a.out.mkdir(parents=True, exist_ok=False)
    (a.out / 'primary.bin').write_bytes(new_primary)
    (a.out / 'backup.bin').write_bytes(new_backup)
    change.update({'disk_lbas': gpt.disk_lbas, 'backup_region_lba': gpt.disk_lbas - BACKUP_LBAS,
                   'old_primary_sha256': hashlib.sha256(gpt.primary).hexdigest(),
                   'old_backup_sha256': hashlib.sha256(gpt.backup).hexdigest(),
                   'new_primary_sha256': hashlib.sha256(new_primary).hexdigest(),
                   'new_backup_sha256': hashlib.sha256(new_backup).hexdigest(),
                   'entries': new.entries()})
    (a.out / 'plan.json').write_text(json.dumps(change, indent=2) + '\n')
    print(json.dumps({k: change[k] for k in ('userdata', 'created')}, indent=2))


if __name__ == '__main__':
    try:
        main()
    except GptError as error:
        sys.exit(f'opd_gpt: {error}')
