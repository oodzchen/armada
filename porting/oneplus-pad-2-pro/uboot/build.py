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
    ap.add_argument('--ufs-snapshot', action='store_true', help='Map whitelisted UFS/GCC/PHY pages read-only and snapshot ABL state')
    ap.add_argument('--smmu-snapshot', action='store_true', help='Also snapshot the ABL UFS stream/context mapping (implies --ufs-snapshot)')
    ap.add_argument('--ufs-nop', action='store_true', help='Send one NOP on the pre-initialized link and restore the original queue; no storage commands')
    ap.add_argument('--ufs-link-check', action='store_true', help='DME_GET link state only, without DMA')
    ap.add_argument('--halt-on-failure', action='store_true', help='Debug: if the Android return is refused, stay on screen instead of the firmware reset that loses the RAM log')
    ap.add_argument('--ufs-read', action='store_true', help='Read-only LUN/capacity/GPT survey on the pre-initialized link; restores the original queue')
    a = ap.parse_args()
    if a.ufs_nop + a.ufs_link_check + a.ufs_read > 1:
        ap.error('Select only one of --ufs-nop, --ufs-link-check and --ufs-read')
    if a.halt_on_failure and not (a.ufs_nop or a.ufs_link_check or a.ufs_read):
        ap.error('--halt-on-failure applies only to --ufs-nop, --ufs-link-check or --ufs-read')
    a.smmu_snapshot |= a.ufs_nop or a.ufs_link_check or a.ufs_read
    a.ufs_snapshot |= a.smmu_snapshot
    src, out, fdt = a.source.resolve(), a.out.resolve(), a.fdt.resolve()
    overlay = Path(__file__).resolve().parent
    jobs = max(1, min(a.jobs, max(1, (os.cpu_count() or 1) - 1)))
    out.mkdir(parents=True, exist_ok=True)
    with (out/'build-observer.log').open('w') as log:
        run([sys.executable, overlay/'prepare.py', src], log)
        make = ['make', '-C', src, f'O={out}/build', 'CROSS_COMPILE=aarch64-linux-gnu-']
        run(make + ['opd2413_observer_defconfig'], log)
        if a.ufs_snapshot or a.smmu_snapshot:
            run([src/'scripts/config', '--file', out/'build/.config', '--enable', 'OPD2413_UFS_SNAPSHOT'], log)
        if a.smmu_snapshot:
            run([src/'scripts/config', '--file', out/'build/.config', '--enable', 'OPD2413_SMMU_SNAPSHOT'], log)
        if a.ufs_nop:
            run([src/'scripts/config', '--file', out/'build/.config', '--enable', 'OPD2413_UFS_NOP'], log)
        if a.ufs_link_check:
            run([src/'scripts/config', '--file', out/'build/.config', '--enable', 'OPD2413_UFS_LINK_CHECK'], log)
        if a.ufs_read:
            run([src/'scripts/config', '--file', out/'build/.config', '--enable', 'OPD2413_UFS_READ'], log)
        if a.ufs_nop or a.ufs_link_check or a.ufs_read:
            # Keep the probe result visible during the countdown instead of
            # scrolling it away with the long bdinfo memory-bank listing.
            env = out/'probe.env'
            text = (overlay/'observer.env').read_text().replace('version; bdinfo;', 'version;')
            if a.halt_on_failure:
                if text.count('opd_android; reset') != 1:raise ValueError('observer.env bootcmd changed')
                text = text.replace('opd_android; reset', 'opd_android; echo OPD2413_HALTED firmware reset withheld for log reading; echo Hold Power and Volume Up to restart')
            env.write_text(text)
            run([src/'scripts/config', '--file', out/'build/.config', '--set-str',
                 'ENV_DEFAULT_ENV_TEXT_FILE', env], log)
        if a.ufs_snapshot or a.smmu_snapshot:
            run(make + ['olddefconfig'], log)
        run(make + [f'-j{jobs}', 'u-boot-nodtb.bin'], log)
        config = (out/'build/.config').read_text().splitlines()
        for symbol, enabled in [('OPD2413_UFS_SNAPSHOT', a.ufs_snapshot or a.smmu_snapshot),
                                ('OPD2413_SMMU_SNAPSHOT', a.smmu_snapshot),
                                ('OPD2413_UFS_NOP', a.ufs_nop),
                                ('OPD2413_UFS_LINK_CHECK', a.ufs_link_check),
                                ('OPD2413_UFS_READ', a.ufs_read)]:
            if (f'CONFIG_{symbol}=y' in config) != enabled:
                raise ValueError('Snapshot configuration mismatch: ' + symbol)
        # This observer cannot initialize or write storage or persistent env.
        for symbol in ['UFS', 'MMC', 'SCSI', 'USB', 'MTD', 'SPI_FLASH', 'FASTBOOT',
                       'CMD_SAVEENV', 'EFI_LOADER']:
            if f'CONFIG_{symbol}=y' in config:
                raise ValueError('Unexpected observer capability: ' + symbol)
        if 'CONFIG_ENV_IS_NOWHERE=y' not in config:
            raise ValueError('Observer environment must stay in RAM')
        shutil.copyfile(overlay/'layout.h', out/'opd2413_layout.h')
        shutil.copyfile(overlay/'ufs_snapshot.h', out/'opd2413_ufs_snapshot.h')
        shutil.copyfile(overlay/'smmu_snapshot.h', out/'opd2413_smmu_snapshot.h')
        shutil.copyfile(overlay/'ufs_nop.h', out/'opd2413_ufs_nop.h')
        shutil.copyfile(overlay/'ufs_link.h', out/'opd2413_ufs_link.h')
        shutil.copyfile(overlay/'ufs_read.h', out/'opd2413_ufs_read.h')
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
        run(['cc', '-g', '-O1', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
             '-DOPD2413_HOST_TEST', '-I'+str(lib), '-I'+str(out),
             overlay/'ufs_snapshot.c', overlay/'ufs_snapshot_test.c', *[lib/s for s in sources],
             '-o', out/'ufs-snapshot-test'], log)
        run([out/'ufs-snapshot-test', fdt], log)
        run(['cc', '-g', '-O1', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
             '-DOPD2413_HOST_TEST', '-I'+str(lib), '-I'+str(out),
             overlay/'smmu_snapshot.c', overlay/'smmu_snapshot_test.c', *[lib/s for s in sources],
             '-o', out/'smmu-snapshot-test'], log)
        run([out/'smmu-snapshot-test', fdt], log)
        run(['cc', '-g', '-O1', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
             '-I'+str(out), overlay/'ufs_nop.c', overlay/'ufs_link.c', overlay/'ufs_nop_test.c',
             '-o', out/'ufs-nop-test'], log)
        run([out/'ufs-nop-test'], log)
        run(['cc', '-g', '-O1', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
             '-I'+str(out), overlay/'ufs_link.c', overlay/'ufs_link_test.c',
             '-o', out/'ufs-link-test'], log)
        run([out/'ufs-link-test'], log)
        run(['cc', '-g', '-O1', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
             '-I'+str(out), overlay/'ufs_read.c', overlay/'ufs_nop.c', overlay/'ufs_link.c',
             overlay/'ufs_read_test.c', '-o', out/'ufs-read-test'], log)
        run([out/'ufs-read-test'], log)
    print('PASS: observer built, storage disabled, ASan/UBSan FDT and memory checks passed')
    print(out/'build/u-boot-nodtb.bin')


if __name__ == '__main__':
    main()
