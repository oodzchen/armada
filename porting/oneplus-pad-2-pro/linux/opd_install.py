#!/usr/bin/env python3
"""Install Armada into the OPD2413 internal UFS, run as root on the tablet.

Runs from the USB-booted Armada with the UFS host enabled and the ufs-guard
udev rule active (Android's partitions read-only). Three steps, each checked
before it writes:

  opd_install.py check  --plan DIR            read-only: which GPT is on LUN0
  opd_install.py gpt    --plan DIR --write    old GPT -> planned GPT (LUN0 only)
  opd_install.py deploy --plan DIR --oci ARCHIVE --write
                        mkfs ARMADA/ARMADA_BOOT/ARMADA_ROOT, ostree deploy of
                        the OCI archive, fstab/BLS entry, ESP via armada-efi-update

DIR is the output of opd_gpt.py plan (primary.bin, backup.bin, plan.json).
The layout and filesystems follow armada-installer.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tomllib

SECTOR = 4096
UFS_HOST = '1d84000.ufs'
BTRFS_OPTS = 'noatime,compress=zstd:1,discard=async'
# Tested on this tablet; the rest comes from the image's bootc kargs.d.
DEVICE_KARGS = ['clk_ignore_unused', 'pd_ignore_unused', 'regulator_ignore_unused', 'selinux=0']
ORIGIN_ARCHIVE = '/var/lib/armada/opd2413/armada.ociarchive'
EFIUPDATE = '/usr/libexec/armada/armada-efi-update'
PARTS = ('ARMADA', 'ARMADA_BOOT', 'ARMADA_ROOT')


class InstallError(Exception):
    pass


def run(*command, **kwargs):
    print('+', ' '.join(map(str, command)), flush=True)
    result = subprocess.run([str(c) for c in command], **kwargs)
    if result.returncode:
        raise InstallError(f'command failed ({result.returncode}): {command[0]}')
    return result


def output(*command):
    return run(*command, capture_output=True, text=True).stdout.strip()


def sha(data):
    return hashlib.sha256(data).hexdigest()


def lun0(sysfs=Path('/sys')):
    """The UFS LUN0 block device, identified by host, SCSI LUN and size."""
    found = []
    for block in sorted((sysfs / 'class/block').glob('sd*')):
        device = block / 'device'
        if not device.exists() or (block / 'partition').exists():
            continue
        path = str(device.resolve())
        if f'/{UFS_HOST}/' in path and path.rsplit(':', 1)[-1] == '0':
            found.append(block.name)
    if len(found) != 1:
        raise InstallError(f'expected exactly one UFS LUN0 disk, found {found}')
    return found[0]


class Disk:
    def __init__(self, name, plan_dir, sysfs=Path('/sys'), dev=Path('/dev')):
        self.name, self.sysfs, self.node = name, sysfs, dev / name
        self.plan = json.loads((plan_dir / 'plan.json').read_text())
        self.new_primary = (plan_dir / 'primary.bin').read_bytes()
        self.new_backup = (plan_dir / 'backup.bin').read_bytes()
        if (sha(self.new_primary), sha(self.new_backup)) != (self.plan['new_primary_sha256'], self.plan['new_backup_sha256']):
            raise InstallError('plan files do not match plan.json')
        block = sysfs / 'class/block' / name
        if int((block / 'queue/logical_block_size').read_text()) != SECTOR:
            raise InstallError('LUN0 is not a 4 KiB sector disk')
        if int((block / 'size').read_text()) * 512 != self.plan['disk_lbas'] * SECTOR:
            raise InstallError('LUN0 size differs from the plan')

    def read_regions(self):
        with open(self.node, 'rb', buffering=0) as disk:
            primary = os.pread(disk.fileno(), 6 * SECTOR, 0)
            backup = os.pread(disk.fileno(), 5 * SECTOR, self.plan['backup_region_lba'] * SECTOR)
        return primary, backup

    def state(self):
        primary, backup = self.read_regions()
        key = (sha(primary), sha(backup))
        if key == (self.plan['old_primary_sha256'], self.plan['old_backup_sha256']):
            return 'old'
        if key == (self.plan['new_primary_sha256'], self.plan['new_backup_sha256']):
            return 'new'
        return 'unknown'

    def partition(self, number):
        node = Path(f'{self.node}{number}')
        if not node.exists():
            raise InstallError(f'{node} is missing')
        return node

    def check_new_partitions(self):
        """The kernel's view of 15..18 matches the plan; ARMADA* writable, userdata read-only."""
        block = self.sysfs / 'class/block' / self.name
        expected = [(self.plan['userdata']['number'], self.plan['userdata']['first'],
                     self.plan['userdata']['new_last'], 'userdata')]
        expected += [(c['number'], c['first'], c['last'], c['name']) for c in self.plan['created']]
        for number, first, last, name in expected:
            part = block / f'{self.name}{number}'
            start = int((part / 'start').read_text()) * 512 // SECTOR
            length = int((part / 'size').read_text()) * 512 // SECTOR
            if (start, start + length - 1) != (first, last):
                raise InstallError(f'kernel sees {name} at {start}+{length}, plan says {first}..{last}')
            label = (part / 'uevent').read_text()
            if f'PARTNAME={name}\n' not in label:
                raise InstallError(f'partition {number} is not labelled {name}')
            ro = (part / 'ro').read_text().strip()
            if ro != ('0' if name in PARTS else '1'):
                raise InstallError(f'{name} has ro={ro}; is the ufs-guard rule active?')


def require_unused(disk):
    mounts = Path('/proc/self/mountinfo').read_text()
    for part in sorted((disk.sysfs / 'class/block' / disk.name).glob(f'{disk.name}*')):
        dev = (part / 'dev').read_text().strip()
        if f' {dev} ' in mounts or any((part / 'holders').iterdir()):
            raise InstallError(f'{part.name} is in use')


def cmd_check(disk, _args):
    print(json.dumps({'disk': str(disk.node), 'gpt': disk.state()}))


def cmd_gpt(disk, args):
    state = disk.state()
    if state == 'new':
        print('LUN0 already has the planned GPT')
        disk.check_new_partitions()
        return
    if state != 'old':
        raise InstallError('LUN0 GPT is neither the backed-up original nor the plan; nothing written')
    if not args.write:
        print('LUN0 has the original GPT; rerun with --write to apply the plan')
        return
    require_unused(disk)
    # Backup copy first: until the primary is replaced, the original primary
    # stays the valid table.
    with open(disk.node, 'r+b', buffering=0) as stream:
        os.pwrite(stream.fileno(), disk.new_backup, disk.plan['backup_region_lba'] * SECTOR)
        os.fsync(stream.fileno())
        os.pwrite(stream.fileno(), disk.new_primary, 0)
        os.fsync(stream.fileno())
    run('blockdev', '--flushbufs', disk.node)
    if disk.state() != 'new':
        raise InstallError('GPT read-back differs from the plan')
    run('blockdev', '--rereadpt', disk.node)
    run('udevadm', 'settle')
    disk.check_new_partitions()
    print('LUN0 GPT written and verified; Android must format userdata before it boots again')


def base_kargs(directory):
    args = []
    for path in sorted(directory.glob('*.toml')):
        data = tomllib.loads(path.read_text())
        if 'match-architectures' in data and 'aarch64' not in data['match-architectures']:
            continue
        values = data.get('kargs', [])
        if not isinstance(values, list) or any(not isinstance(v, str) or any(c.isspace() for c in v) for v in values):
            raise InstallError(f'invalid kernel arguments in {path}')
        args += [v for v in values if not v.startswith(('root=', 'boot=', 'rootflags=', 'ostree='))]
    return args


def bls_entry(entry_text, uuids, kargs):
    """armada-installer's entry: root/boot by UUID, btrfs subvol, image + device kargs."""
    fields = dict(line.split(None, 1) for line in entry_text.splitlines() if line.strip())
    pointers = [a for a in fields.get('options', '').split() if a.startswith('ostree=')]
    if len(pointers) != 1 or not fields.get('linux') or not fields.get('initrd'):
        raise InstallError('incomplete boot entry')
    linux, initrd = (fields[k] if fields[k].startswith('/boot/') else '/boot' + fields[k] for k in ('linux', 'initrd'))
    options = [f"root=UUID={uuids['root']}", 'rw', f"boot=UUID={uuids['boot']}", *kargs,
               f'rootflags=subvol=root,{BTRFS_OPTS}', pointers[0]]
    return (f"title Armada\nversion {fields.get('version', '1')}\noptions {' '.join(options)}\n"
            f'linux {linux}\ninitrd {initrd}\nfdtdir {Path(linux).parent}/dtb\n'), pointers[0]


def cmd_deploy(disk, args):
    if disk.state() != 'new':
        raise InstallError('LUN0 does not have the planned GPT; run the gpt step first')
    disk.check_new_partitions()
    numbers = {c['name']: c['number'] for c in disk.plan['created']}
    esp, boot, root = (disk.partition(numbers[n]) for n in PARTS)
    archive = Path(args.oci).resolve()
    if not archive.is_file():
        raise InstallError(f'{archive} is missing')
    if not args.write:
        print(f'ready to format {esp} {boot} {root} and deploy {archive}; rerun with --write')
        return
    for node in (esp, boot, root):
        if any(f' {node} ' in line or line.startswith(f'{node} ') for line in Path('/proc/mounts').read_text().splitlines()):
            raise InstallError(f'{node} is mounted')
    work = Path('/run/opd-install')
    target, top = work / 'target', work / 'top'
    mounted = []

    def mount(source, where, options=None):
        where.mkdir(parents=True, exist_ok=True)
        run('mount', *(['-o', options] if options else []), source, where)
        mounted.append(where)

    try:
        run('mkfs.btrfs', '-f', '-L', 'root', root)
        mount(root, top)
        for name in ('root', 'var', 'home'):
            run('btrfs', 'subvolume', 'create', top / name)
        run('umount', top)
        mounted.remove(top)
        mount(root, target, BTRFS_OPTS + ',subvol=root')
        mount(root, target / 'var', BTRFS_OPTS + ',subvol=var')
        mount(root, target / 'var/home', BTRFS_OPTS + ',subvol=home')
        run('ostree', 'admin', 'init-fs', '--modern', target)
        run('mkfs.vfat', '-F', '16', '-S', SECTOR, '-s', 16384 // SECTOR, '-n', 'ARMADA', esp)
        run('mkfs.ext4', '-F', '-L', 'boot', boot)
        mount(boot, target / 'boot')
        mount(esp, target / 'boot/efi')
        uuids = {n: output('blkid', '-s', 'UUID', '-o', 'value', node) for n, node in (('root', root), ('boot', boot), ('esp', esp))}
        if not all(re.fullmatch(r'[A-Za-z0-9-]+', v) for v in uuids.values()):
            raise InstallError('could not read filesystem UUIDs')
        repo = target / 'ostree/repo'
        for key, value in (('sysroot.bootloader', 'none'), ('sysroot.bootprefix', 'true'), ('sysroot.readonly', 'true')):
            run('ostree', 'config', f'--repo={repo}', 'set', key, value)
        run('ostree', 'admin', 'os-init', f'--sysroot={target}', 'default')
        commit_file = work / 'commit'
        run('ostree', 'container', 'image', 'deploy', f'--sysroot={target}', '--stateroot=default',
            f'--imgref=ostree-unverified-image:oci-archive:{archive}',
            f'--target-imgref=ostree-unverified-image:oci-archive:{ORIGIN_ARCHIVE}',
            f'--write-commitid-to={commit_file}')
        checksum = commit_file.read_text().strip()
        # <checksum>.<serial> is the deployment; <checksum>.<serial>.origin sits next to it.
        deployments = [d for d in (target / 'ostree/deploy/default/deploy').glob(f'{checksum}.*') if d.is_dir()]
        if len(deployments) != 1:
            raise InstallError('expected one deployment of the new commit')
        deploy_root = deployments[0]
        seed = target / '.armada-var-seed'
        run('ostree', 'checkout', f'--repo={repo}', '--subpath=/var', checksum, seed)
        run('cp', '-a', f'{seed}/.', f'{target}/var/')
        shutil.rmtree(seed)
        (deploy_root / 'etc/fstab').write_text(
            f"UUID={uuids['boot']} /boot auto ro 0 0\n"
            f"UUID={uuids['esp']} /boot/efi vfat umask=0077,shortname=winnt 0 2\n"
            f"UUID={uuids['root']} /var btrfs {BTRFS_OPTS},subvol=var 0 0\n"
            f"UUID={uuids['root']} /var/home btrfs {BTRFS_OPTS},subvol=home 0 0\n")
        kargs = base_kargs(deploy_root / 'usr/lib/bootc/kargs.d') + DEVICE_KARGS
        entries = list((target / 'boot/loader/entries').glob('*.conf'))
        if len(entries) != 1:
            raise InstallError('expected one boot entry')
        text, pointer = bls_entry(entries[0].read_text(), uuids, kargs)
        if (target / pointer.split('=', 1)[1].lstrip('/')).resolve(strict=True) != deploy_root.resolve():
            raise InstallError('boot entry does not point at the deployment')
        entries[0].write_text(text)
        origin = target / ORIGIN_ARCHIVE.lstrip('/')
        origin.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(archive, origin)
        env = dict(os.environ, ESP=str(target / 'boot/efi'), BOOTROOT=str(target / 'boot'), SYSROOT=str(target))
        run(EFIUPDATE, env=env)
        esp_root = target / 'boot/efi'
        for name in ('EFI/BOOT/BOOTAA64.EFI', 'dtbs/qcom/sm8750-oneplus-pad2pro.dtb', 'loader/loader.conf'):
            if not (esp_root / name).is_file():
                raise InstallError(f'ESP is missing {name}')
        if not list((esp_root / 'loader/entries').glob('armada-*.conf')):
            raise InstallError('ESP has no armada boot entry')
        result = {'commit': checksum, 'uuids': uuids, 'kargs': kargs, 'entry': text,
                  'esp_entries': sorted(p.name for p in (esp_root / 'loader/entries').glob('*.conf'))}
        (work / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
        print(json.dumps(result, indent=2))
    finally:
        run('sync')
        for where in reversed(mounted):
            subprocess.run(['umount', str(where)])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('command', choices=('check', 'gpt', 'deploy'))
    ap.add_argument('--plan', type=Path, required=True)
    ap.add_argument('--oci')
    ap.add_argument('--write', action='store_true', help='actually write (otherwise only check)')
    a = ap.parse_args()
    if os.geteuid() != 0:
        raise InstallError('run as root')
    if a.command == 'deploy' and not a.oci:
        ap.error('deploy needs --oci')
    disk = Disk(lun0(), a.plan)
    {'check': cmd_check, 'gpt': cmd_gpt, 'deploy': cmd_deploy}[a.command](disk, a)


if __name__ == '__main__':
    try:
        main()
    except InstallError as error:
        sys.exit(f'opd_install: {error}')
