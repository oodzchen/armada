#!/usr/bin/env python3
"""Build the optional kShimC OPD2413 ESP boot-choice backend."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kshim-dir', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--enable-write', action='store_true', help='omit for the first dryrun')
    parser.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 2) - 1))
    args = parser.parse_args()
    root, out = args.kshim_dir.resolve(), args.out.resolve()
    genconfig = shutil.which('genconfig')
    if not genconfig:
        parser.error('activate a Python environment with kconfiglib/genconfig installed')
    jobs = min(max(1, args.jobs), max(1, (os.cpu_count() or 2) - 1))
    config = (root / 'configs/opd2413_debug_defconfig').read_text()
    config += '\nCONFIG_KSHIM_OPD2413_BOOT_CHOICE=y\n'
    config += ('CONFIG_KSHIM_OPD2413_CHOICE_WRITE=y\n' if args.enable_write else
               '# CONFIG_KSHIM_OPD2413_CHOICE_WRITE is not set\n')
    out.mkdir(parents=True, exist_ok=True)
    (out / '.config').write_text(config)
    subprocess.run(['cmake', '-S', str(root), '-B', str(out), '-G', 'Ninja',
                    '-DCMAKE_BUILD_TYPE=Release', '-DKSHIM_STANDALONE=OFF',
                    '-DKSHIM_RUNTIME_SLOT_SIZE=0x400000', '-DKSHIM_UEFI_PAYLOAD=',
                    f'-DPython3_EXECUTABLE={sys.executable}',
                    f'-DGENCONFIG_EXECUTABLE={genconfig}',
                    f'-DKSHIM_OPD2413_PORT_DIR={Path(__file__).resolve().parents[1]}'], check=True)
    subprocess.run(['cmake', '--build', str(out), '-j', str(jobs)], check=True)
    binary = out / 'kShimC.bin'
    if binary.stat().st_size != 0x400000:
        raise RuntimeError('runtime must fit the existing 4 MiB manifest slot')
    undefined = subprocess.check_output(['aarch64-linux-gnu-nm', '-u', str(out / 'kShimC.elf')], text=True)
    if undefined.strip():
        raise RuntimeError('undefined symbols: ' + undefined)
    report = {'sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
              'bytes': binary.stat().st_size, 'state_writes': args.enable_write,
              'hardware_tested': False}
    (out / 'validation.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
