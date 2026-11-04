#!/usr/bin/env bash
set -euo pipefail
repo="$(cd "$(dirname "$0")/.." && pwd)"
python3 -B - "${repo}" <<'PY'
import importlib.util
from pathlib import Path
import re
import subprocess
import sys
import tempfile

root = Path(sys.argv[1])
spec = importlib.util.spec_from_file_location('smoke_initramfs', root / 'porting/oneplus-pad-2-pro/linux/smoke_initramfs.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
block = module.USBROOT.split('# BOOTSTRAP_DEFAULTS_BEGIN', 1)[1].split('# BOOTSTRAP_DEFAULTS_END', 1)[0]
with tempfile.TemporaryDirectory() as temporary:
    work = Path(temporary)
    source, target = work / 'bootstrap', work / 'existing-root'
    for directory in (source, target):
        (directory / 'etc').mkdir(parents=True)
        (directory / 'var/home/armada').mkdir(parents=True)
    for name, data in [('etc/controller.conf', 'user-choice\n'),
                       ('var/home/armada/preferences', 'user-data\n')]:
        (target / name).write_text(data)
        (target / name).chmod(0o600)
        (source / name).write_text('bootstrap-default\n')
    (target / 'etc/link').symlink_to('controller.conf')
    (source / 'etc/link').symlink_to('new-default.conf')
    (source / 'etc/new-default.conf').write_text('needed-for-first-boot\n')
    (source / 'etc/private').mkdir(mode=0o700)
    (source / 'etc/private/key').write_text('bootstrap-key\n')
    command = 'bootstrap_defaults=1\n' + block.replace('/opd/overlay', str(source)).replace('/sysroot/', str(target) + '/')
    subprocess.run(['sh', '-eu', '-c', command], check=True)
    assert (target / 'etc/controller.conf').read_text() == 'user-choice\n'
    assert (target / 'etc/controller.conf').stat().st_mode & 0o777 == 0o600
    assert (target / 'var/home/armada/preferences').read_text() == 'user-data\n'
    assert (target / 'etc/link').readlink() == Path('controller.conf')
    assert (target / 'etc/new-default.conf').read_text() == 'needed-for-first-boot\n'
    assert (target / 'etc/private/key').read_text() == 'bootstrap-key\n'
    assert (target / 'etc/private').stat().st_mode & 0o777 == 0o700
    # For an established upper layer, preserve deletions too: bootstrap is
    # skipped entirely, so a missing path/whiteout cannot be recreated.
    (source / 'etc/user-deleted.conf').write_text('deleted-default\n')
    skip = command.replace('bootstrap_defaults=1', 'bootstrap_defaults=0')
    subprocess.run(['sh', '-eu', '-c', 'say() { :; };\n' + skip], check=True)
    assert not (target / 'etc/user-deleted.conf').exists()
    state = module.USBROOT.split('# USB_LAYER_STATE_BEGIN', 1)[1].split('# USB_LAYER_STATE_END', 1)[0]
    upper = work / 'upper'
    state = state.replace('/usb/opd/upper', str(upper)) + '\nprintf "%s" "$bootstrap_defaults"'
    assert subprocess.check_output(['sh', '-eu', '-c', state], text=True) == '1'
    upper.mkdir()
    assert subprocess.check_output(['sh', '-eu', '-c', state], text=True) == '1'
    (upper / '.existing-state').touch()
    assert subprocess.check_output(['sh', '-eu', '-c', state], text=True) == '0'
print('USB bootstrap preserves configuration, data, modes, symlinks and deletions')
PY
