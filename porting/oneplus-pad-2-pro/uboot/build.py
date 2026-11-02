#!/usr/bin/env python3
"""Build and host-check the RAM-only observer in a dedicated U-Boot checkout."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys


def run(argv, log):
    subprocess.run(list(map(str, argv)), check=True, stdout=log, stderr=subprocess.STDOUT)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source', type=Path, required=True, help='Dedicated checkout at prepare.py PIN')
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--fdt', type=Path, required=True, help='Private OPD2413 ABL FDT for host validation')
    ap.add_argument('--jobs', type=int, default=12)
    a = ap.parse_args()
    src, out, fdt = a.source.resolve(), a.out.resolve(), a.fdt.resolve()
    overlay = Path(__file__).resolve().parent
    jobs = max(1, min(a.jobs, max(1, (os.cpu_count() or 1) - 1)))
    out.mkdir(parents=True, exist_ok=True)
    with (out/'build-observer.log').open('w') as log:
        run([sys.executable, overlay/'prepare.py', src], log)
        make = ['make', '-C', src, f'O={out}/build', 'CROSS_COMPILE=aarch64-linux-gnu-']
        run(make + ['opd2413_observer_defconfig'], log)
        run(make + [f'-j{jobs}', 'u-boot-nodtb.bin'], log)
        config = (out/'build/.config').read_text().splitlines()
        # This observer cannot initialize or write storage or persistent env.
        for symbol in ['UFS', 'MMC', 'SCSI', 'USB', 'MTD', 'SPI_FLASH', 'FASTBOOT',
                       'CMD_SAVEENV', 'EFI_LOADER']:
            if f'CONFIG_{symbol}=y' in config:
                raise ValueError('Unexpected observer capability: ' + symbol)
        if 'CONFIG_ENV_IS_NOWHERE=y' not in config:
            raise ValueError('Observer environment must stay in RAM')
        shutil.copyfile(overlay/'layout.h', out/'opd2413_layout.h')
        lib = src/'scripts/dtc/libfdt'
        sources = ['fdt.c', 'fdt_ro.c', 'fdt_check.c', 'fdt_sw.c', 'fdt_rw.c',
                   'fdt_wip.c', 'fdt_empty_tree.c', 'fdt_addresses.c']
        run(['cc', '-g', '-O1', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
             '-DOPD2413_HOST_TEST', '-I'+str(lib), '-I'+str(out),
             overlay/'filter.c', overlay/'filter_test.c', *[lib/s for s in sources],
             '-o', out/'filter-test'], log)
        run([out/'filter-test', fdt], log)
        run(['cc', '-g', '-O1', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
             overlay/'ramconsole_test.c', '-o', out/'ramconsole-test'], log)
        run([out/'ramconsole-test', out/'ramconsole-fixture.bin'], log)
        run([sys.executable, overlay/'test_decode.py', out/'ramconsole-fixture.bin'], log)
    print('PASS: observer built, storage disabled, ASan/UBSan FDT and memory checks passed')
    print(out/'build/u-boot-nodtb.bin')


if __name__ == '__main__':
    main()
