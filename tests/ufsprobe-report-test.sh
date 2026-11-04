#!/usr/bin/env bash
# Run the initramfs UFS probe against a fake sysfs/dev tree: every UFS disk and
# partition is set read-only, only UFS disks are reported and removed, and the
# GPT/partition digests match the backing data.
set -euo pipefail
repo="$(cd "$(dirname "$0")/.." && pwd)"
"${PYTHON:-python3}" -B - "${repo}" <<'PY'
import hashlib
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(sys.argv[1])
spec = importlib.util.spec_from_file_location('smoke_initramfs', root / 'porting/oneplus-pad-2-pro/linux/smoke_initramfs.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
SHELL = os.environ.get('USBROOT_TEST_SH', 'sh').split()

with tempfile.TemporaryDirectory() as temporary:
    t = Path(temporary)
    sysblock, dev, platform, usb = t / 'sys/block', t / 'dev', t / 'sys/devices', t / 'usb'
    for d in (sysblock, dev, usb):
        d.mkdir(parents=True)
    calls = t / 'calls'
    disks = {'sda': ('ufs', 64), 'sdb': ('ufs', 16), 'sdc': ('usb', 8)}
    data = {}
    for name, (kind, mib) in disks.items():
        path = ('soc@0/1d84000.ufs/host0/target0:0:0/0:0:0:%d' if kind == 'ufs' else 'usb1/1-1/host1/target1:0:0/1:0:0:%d') % len(data)
        device = platform / path
        device.mkdir(parents=True)
        (device / 'vendor').write_text('VENDOR  \n')
        (device / 'model').write_text('MODEL X\n')
        (device / 'rev').write_text('0001\n')
        b = sysblock / name
        (b / 'queue').mkdir(parents=True)
        (b / 'device').symlink_to(device)
        size = mib << 20
        content = hashlib.sha256(name.encode()).digest() * (size // 32)
        (dev / name).write_bytes(content)
        data[name] = content
        for f, v in [('size', size // 512), ('ro', 0), ('queue/logical_block_size', 4096), ('queue/physical_block_size', 4096)]:
            (b / f).write_text(f'{v}\n')
        # One small partition at 1 MiB (hashed) and one over 128 MiB (skipped).
        for number, start, sectors, label in [(1, 2048, 4096, 'boot_b'), (2, 8192, 300000, 'super')]:
            p = b / f'{name}{number}'
            p.mkdir()
            (p / 'partition').write_text(f'{number}\n')
            (p / 'start').write_text(f'{start}\n')
            (p / 'size').write_text(f'{sectors}\n')
            (p / 'ro').write_text('0\n')
            (p / 'uevent').write_text(f'DEVTYPE=partition\nPARTNAME={label}\n')
            (dev / f'{name}{number}').write_bytes(content[start * 512:(start + sectors) * 512])
    stats = t / 'diskstats'
    stats.write_text(''.join(f'   8 {i} {n} 10 0 80 0 0 0 0 0 0 0 0 0 0 0 0 0\n' for i, n in enumerate(disks)))
    script = module.UFS_PROBE.replace('/sys/block', str(sysblock)).replace('/proc/diskstats', str(stats)) \
        .replace('/sys/bus/platform/devices/1d84000.ufs', str(t / 'nohost')).replace('/usb/', str(usb) + '/') \
        .replace('/dev/null', '@NULL@').replace('/dev/', f'{dev}/').replace('@NULL@', '/dev/null')
    harness = f'''
say() {{ echo "$*" >> {t}/say; }}
blockdev() {{
    case "$1" in
    --setro) echo "setro $2" >> {calls}; n=${{2##*/}}; for f in {sysblock}/$n/ro {sysblock}/*/$n/ro {sysblock}/$n/$n*/ro; do [ -e "$f" ] && echo 1 > "$f"; done; return 0 ;;
    --getsize64) wc -c < "$2" ;;
    esac
}}
sleep() {{ :; }}
dmesg() {{ echo "ufshcd-qcom 1d84000.ufs: fake"; }}
{script}
ufsprobe
'''
    # SCSI delete files: plain files here; the probe writes 1 to each UFS one.
    for name, (kind, _) in disks.items():
        (platform / (sysblock / name / 'device').resolve().relative_to(platform) / 'delete').write_text('')
    subprocess.run([*SHELL, '-eu', '-c', harness], check=True)
    report = (usb / 'opd/probes/ufs-1.txt').read_text()
    lines = report.splitlines()
    assert lines[0] == 'ufs-probe v1', lines[0]
    setro = sorted(set(calls.read_text().split('\n')) - {''})
    assert setro == sorted(f'setro {dev}/{n}' for n in ('sda', 'sda1', 'sda2', 'sdb', 'sdb1', 'sdb2')), setro
    assert not any('sdc' in line for line in lines), 'USB disk must not be probed'
    for name in ('sda', 'sdb'):
        content = data[name]
        primary = hashlib.sha256(content[:24576]).hexdigest()
        backup = hashlib.sha256(content[-20480:]).hexdigest()
        assert f'gpt {name} primary24k={primary} backup20k={backup}' in lines, name
        disk = next(line for line in lines if line.startswith(f'disk {name} '))
        assert 'ro=1' in disk and 'lbs=4096' in disk and 'vendor=VENDOR' in disk, disk
        small = hashlib.sha256(content[2048 * 512:(2048 + 4096) * 512]).hexdigest()
        assert f'part {name}1 name=boot_b start512=2048 bytes=2097152 ro=1 sha256={small}' in lines
        assert f'part {name}2 name=super start512=8192 bytes=153600000 ro=1 sha256=-' in lines
        assert f'before diskstat {name} reads 10 sectors_read 80 writes 0 sectors_written 0 flushes 0' in lines
        assert (platform / (sysblock / name / 'device').resolve().relative_to(platform) / 'delete').read_text().strip() == '1'
    assert (platform / (sysblock / 'sdc' / 'device').resolve().relative_to(platform) / 'delete').read_text() == ''
    assert 'ufshcd-qcom 1d84000.ufs: fake' in lines
    # opd.ufsguard: same tree, sda2 becomes ARMADA_ROOT. Expect LUN0's other
    # partitions and the whole other LUN read-only, ARMADA_ROOT and LUN0 itself
    # writable, the USB disk untouched, and no device removed.
    for f in sysblock.glob('*/ro'):
        f.write_text('0\n')
    for f in sysblock.glob('*/*/ro'):
        f.write_text('0\n')
    calls.unlink()
    deletes = [platform / (sysblock / name / 'device').resolve().relative_to(platform) / 'delete' for name in disks]
    for f in deletes:
        f.write_text('')
    (sysblock / 'sda/sda2/uevent').write_text('DEVTYPE=partition\nPARTNAME=ARMADA_ROOT\n')
    guard = module.UFS_PROBE.replace('/sys/block', str(sysblock)).replace('/usb/', str(usb) + '/') \
        .replace('/dev/null', '@NULL@').replace('/dev/', f'{dev}/').replace('@NULL@', '/dev/null')
    harness = harness.split('\n# Read-only look')[0] + '\n' + guard + '\nufsguard\n'
    subprocess.run([*SHELL, '-eu', '-c', harness], check=True)
    setro = sorted(set(calls.read_text().split('\n')) - {''})
    assert setro == sorted(f'setro {dev}/{n}' for n in ('sda1', 'sdb')), setro
    said = (t / 'say').read_text()
    assert 'ufsguard: 2 UFS disks' in said and 'writable: sda sda2\n' in said, said
    assert all(f.read_text() == '' for f in deletes), 'guard mode must not remove devices'
print('UFS probe: read-only before reading, UFS disks only, digests and removal correct; guard mode locks all but ARMADA*')
PY
