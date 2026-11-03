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
    ap.add_argument('--ufs-blk', action='store_true', help='Read-only opdufs block devices with FAT/ext4 read tests (implies --ufs-wake); no writes, no EFI')
    ap.add_argument('--efi-hello', action='store_true', help='With the opdufs tests, enable the EFI loader (volatile variables only, ABL handoff memory reserved and CRC-checked) and run bootefi hello (implies --ufs-blk)')
    ap.add_argument('--efi-esp', type=Path, metavar='BOOTAA64.EFI', help='With --efi-hello: embed a FAT ESP holding this EFI program as /EFI/BOOT/BOOTAA64.EFI plus loader/loader.conf, map it with blkmap and bootefi it (implies --efi-hello)')
    ap.add_argument('--esp-blob', action='store_true', help='With --efi-esp: write the ESP to esp.img for a kShimC Type=Blob entry named "OPD2413 ESP" instead of embedding it in U-Boot')
    ap.add_argument('--linux', type=Path, metavar='VMLINUZ.EFI', help='With --efi-menu: add a systemd-boot entry for this EFI-stub kernel')
    ap.add_argument('--linux-dtb', type=Path, help='Device tree for the --linux entry')
    ap.add_argument('--linux-initrd', type=Path, help='initramfs for the --linux entry')
    ap.add_argument('--linux-options', default='', help='Kernel command line for the --linux entry')
    ap.add_argument('--linux-title', default='Linux', help='Menu title for the --linux entry')
    ap.add_argument('--linux-entry', action='append', default=[], metavar='TITLE|DTB|OPTIONS',
                    help='Additional entry with the same --linux kernel and initrd (repeatable)')
    ap.add_argument('--esp-default', default='auto-reboot', help='systemd-boot default entry id (e.g. linux.conf for an unattended QEMU run)')
    ap.add_argument('--keys', action='store_true', help='Read-only volume/power keys over SPMI as U-Boot input, with a key test step (implies --ufs-blk)')
    ap.add_argument('--efi-menu', action='store_true', help='With --efi-esp: systemd-boot menu with an EFI hello entry and Reboot (EFI reset returns to Android), driven by the keys (implies --keys)')
    ap.add_argument('--ufs-trace', action='store_true', help='Debug with --ufs-blk: stack-frame check around doorbells after board init; a main_loop re-entry halts with no further UFS command')
    ap.add_argument('--ufs-blk-ramdisk', action='store_true', help='QEMU test only: opdufs served from a RAM extent table instead of UFS (implies --ufs-blk); never boot it on the device')
    ap.add_argument('--ufs-wake', action='store_true', help='With --ufs-read: START STOP UNIT to Active from Sleep/PowerDown, then restore and verify the original power mode (implies --ufs-read)')
    ap.add_argument('--ufs-read', action='store_true', help='Read-only LUN/capacity/GPT survey on the pre-initialized link; restores the original queue')
    a = ap.parse_args()
    if a.efi_menu and a.efi_esp is None:
        ap.error('--efi-menu needs --efi-esp')
    if a.esp_blob and a.efi_esp is None:
        ap.error('--esp-blob needs --efi-esp')
    if a.linux and not (a.efi_menu and a.linux_dtb and a.linux_initrd):
        ap.error('--linux needs --efi-menu, --linux-dtb and --linux-initrd')
    a.keys |= a.efi_menu
    a.efi_hello |= a.efi_esp is not None
    a.ufs_blk |= a.ufs_blk_ramdisk or a.efi_hello or a.keys
    if a.ufs_trace and not a.ufs_blk:
        ap.error('--ufs-trace applies only to --ufs-blk')
    a.ufs_wake |= a.ufs_blk
    a.ufs_read |= a.ufs_wake
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
        if a.ufs_wake:
            run([src/'scripts/config', '--file', out/'build/.config', '--enable', 'OPD2413_UFS_WAKE'], log)
        if a.ufs_blk:
            if a.ufs_blk_ramdisk:
                run([src/'scripts/config', '--file', out/'build/.config', '--enable', 'OPD2413_UFS_BLK_RAMDISK'], log)
            if a.ufs_trace:
                run([src/'scripts/config', '--file', out/'build/.config', '--enable', 'OPD2413_UFS_TRACE'], log)
            if a.keys:
                run([src/'scripts/config', '--file', out/'build/.config', '--enable', 'OPD2413_KEYS'], log)
            for symbol in ['BLK', 'PARTITIONS', 'EFI_PARTITION', 'CMD_PART', 'FS_FAT', 'CMD_FAT',
                           'FS_EXT4', 'CMD_EXT4', 'CMD_FS_GENERIC', 'CMD_TIME', 'CMD_HASH', 'HUSH_PARSER', 'OPD2413_UFS_BLK']:
                run([src/'scripts/config', '--file', out/'build/.config', '--enable', symbol], log)
            # EFI_LOADER defaults on with BLK; only --efi-hello keeps it.
            for symbol in ['CMD_CRC32', *([] if a.efi_hello else ['EFI_LOADER']), 'FAT_WRITE', 'EXT4_WRITE', 'CMD_EXT4_WRITE', 'CMD_GPT', 'CMD_MBR', 'DOS_PARTITION',
                           'ISO_PARTITION', 'AMIGA_PARTITION', 'MAC_PARTITION']:
                run([src/'scripts/config', '--file', out/'build/.config', '--disable', symbol], log)
        if a.efi_hello:
            for symbol in ['EFI_LOADER', 'CMD_BOOTEFI', 'CMD_BOOTEFI_BINARY', 'BOOTEFI_HELLO_COMPILE', 'CMD_BOOTEFI_HELLO',
                           'CMD_EFIDEBUG', 'EFI_VARIABLE_NO_STORE', 'OPD2413_EFI',
                           # EFI programs such as systemd-boot draw with ANSI escapes.
                           'VIDEO_ANSI']:
                run([src/'scripts/config', '--file', out/'build/.config', '--enable', symbol], log)
            # No persistent variables, capsules, time setting, TPM or network boot.
            for symbol in ['EFI_VARIABLE_FILE_STORE', 'EFI_VARIABLE_SF_STORE', 'EFI_MM_COMM_TEE', 'EFI_RT_VOLATILE_STORE',
                           'EFI_VARIABLES_PRESEED', 'EFI_HAVE_CAPSULE_SUPPORT', 'EFI_RUNTIME_UPDATE_CAPSULE',
                           'EFI_CAPSULE_ON_DISK', 'EFI_CAPSULE_ON_DISK_EARLY', 'EFI_CAPSULE_FIRMWARE_FIT',
                           'EFI_CAPSULE_FIRMWARE_RAW', 'EFI_CAPSULE_FIRMWARE_MANAGEMENT', 'EFI_SET_TIME',
                           'EFI_TCG2_PROTOCOL', 'EFI_HTTP_BOOT', 'EFI_IP4_CONFIG2_PROTOCOL', 'EFI_HTTP_PROTOCOL',
                           'EFI_SECURE_BOOT', 'CMD_NVEDIT_EFI', 'CMD_EFICONFIG',
                           'CMD_BOOTEFI_SELFTEST', 'EFI_BOOTMGR']:
                run([src/'scripts/config', '--file', out/'build/.config', '--disable', symbol], log)
            if a.efi_esp:
                esp = out/'esp.img'
                esp.unlink(missing_ok=True)
                extra = [e.split('|', 2) for e in a.linux_entry]
                if any(len(e) != 3 for e in extra):
                    raise ValueError('--linux-entry must be TITLE|DTB|OPTIONS')
                payload = [a.efi_esp] + ([a.linux, a.linux_dtb, a.linux_initrd] if a.linux else []) + [Path(e[1]) for e in extra]
                kib = 4096 + sum(f.stat().st_size for f in payload) // 1024 * 11 // 10
                run(['mkfs.vfat', '-C', '-n', 'OPDESP', esp, str(-(-kib // 1024) * 1024)], log)
                run(['mmd', '-i', esp, '::/EFI', '::/EFI/BOOT', '::/EFI/opd', '::/loader', '::/loader/entries'], log)
                if a.linux:
                    for f, name in [(a.linux, 'vmlinuz.efi'), (a.linux_dtb, 'board.dtb'), (a.linux_initrd, 'initramfs.cpio.gz')]:
                        run(['mcopy', '-i', esp, f.resolve(), '::/EFI/opd/' + name], log)
                    (out/'linux.conf').write_text(f'title {a.linux_title}\nlinux /EFI/opd/vmlinuz.efi\n'
                                                  f'devicetree /EFI/opd/board.dtb\ninitrd /EFI/opd/initramfs.cpio.gz\n'
                                                  f'options {a.linux_options}\n')
                    run(['mcopy', '-i', esp, out/'linux.conf', '::/loader/entries/linux.conf'], log)
                    for n, (title, dtb, options) in enumerate(extra, 2):
                        run(['mcopy', '-i', esp, Path(dtb).resolve(), f'::/EFI/opd/board{n}.dtb'], log)
                        (out/f'linux{n}.conf').write_text(f'title {title}\nlinux /EFI/opd/vmlinuz.efi\n'
                                                          f'devicetree /EFI/opd/board{n}.dtb\ninitrd /EFI/opd/initramfs.cpio.gz\n'
                                                          f'options {options}\n')
                        run(['mcopy', '-i', esp, out/f'linux{n}.conf', f'::/loader/entries/linux{n}.conf'], log)
                run(['mcopy', '-i', esp, a.efi_esp.resolve(), '::/EFI/BOOT/BOOTAA64.EFI'], log)
                if a.efi_menu:
                    # U-Boot's own hello world as a chainloaded entry; "Reboot"
                    # (EFI ResetSystem) returns to Android and is the default.
                    run(make + ['olddefconfig'], log)
                    run(make + [f'-j{jobs}', 'lib/efi_loader/'], log)
                    run(['mcopy', '-i', esp, out/'build/lib/efi_loader/helloworld.efi', '::/EFI/opd/hello.efi'], log)
                    (out/'hello.conf').write_text('title EFI hello (returns to this menu)\nefi /EFI/opd/hello.efi\n')
                    run(['mcopy', '-i', esp, out/'hello.conf', '::/loader/entries/hello.conf'], log)
                    # The QEMU ramdisk build has no keys: a short timeout picks Reboot.
                    (out/'loader.conf').write_text(f'timeout {3 if a.ufs_blk_ramdisk else 30}\ndefault {a.esp_default}\nauto-reboot yes\nauto-firmware no\n'
                                                   'editor no\nconsole-mode keep\n')
                else:
                    (out/'loader.conf').write_text('timeout 5\nconsole-mode keep\n')
                run(['mcopy', '-i', esp, out/'loader.conf', '::/loader/loader.conf'], log)
                for symbol in ['BLKMAP', 'CMD_BLKMAP', 'OPD2413_EFI_ESP_BLOB' if a.esp_blob else 'OPD2413_EFI_ESP']:
                    run([src/'scripts/config', '--file', out/'build/.config', '--enable', symbol], log)
                if not a.esp_blob:
                    run([src/'scripts/config', '--file', out/'build/.config', '--set-str', 'OPD2413_EFI_ESP_IMAGE', esp], log)
        if a.ufs_nop or a.ufs_link_check or a.ufs_read:
            # Keep the probe result visible during the countdown instead of
            # scrolling it away with the long bdinfo memory-bank listing.
            env = out/'probe.env'
            text = (overlay/'observer.env').read_text().replace('version; bdinfo;', 'version;')
            if a.ufs_blk:
                # Read-only exercise of block, partition, FAT and ext4 paths.
                # 0xc0000000 lies in usable RAM clear of the ABL kernel,
                # ramdisk, FDT, the U-Boot slot/DMA window and relocation.
                load = 'c0000000'
                # Hush is enabled for $? and test (sandbox-checked with these steps).
                # Long listings go only to the RAM console ("run q"): scrolling
                # a 3392x2400 vidconsole is slow and a photo of it is
                # unreadable. The screen shows one marker per step, so a stall
                # or a print loop is pinned to its step even if the log is lost.
                # U-Boot partition numbers are hex: modem_b = 42 = 0x2a,
                # dsp_b = 45 = 0x2d.
                tests = []
                def step(name, cmd, quiet, out=None):
                    (tests if out is None else out).extend([f'echo OPD2413_STEP {name}', *(['run q'] if quiet else []),
                                  f'time {cmd}', 'setenv r $?', *(['run v'] if quiet else []),
                                  f'echo OPD2413_STEP {name} rc=${{r}}'])
                # A dangling io pointer once made every command-line UFS
                # command restart main_loop silently, re-running bootcmd until
                # hush ran out of memory. Any re-entry now returns to Android
                # (UFS restored first, log kept) or halts.
                # With --ufs-trace even the restore would issue UFS commands,
                # so a re-entry halts at once with the diagnosis on screen.
                reentry = (['echo OPD2413_HALTED no further UFS command; echo Hold Power and Volume Up to restart'] if a.ufs_trace else
                           ['opd_android', 'echo OPD2413_HALTED Android return refused after re-entry'])
                tests.extend(['if test -n "${opd_ran}"; then echo OPD2413_REENTERED main_loop restarted bootcmd',
                              *reentry, 'while test 1 = 1; do sleep 3600; done', 'fi', 'setenv opd_ran 1'])
                step('fatinfo', 'fatinfo opdufs 4:2a', False)
                for path in ['/image/adsp.b00', '/image/adsp.b09', '/image/adsp.b38']:
                    tests.append(f'echo OPD2413_FILE fat {path}')
                    step('load', f'load opdufs 4:2a {load} {path}', False)
                    tests.append(f'hash crc32 {load} ${{filesize}}')
                step('ext4ls', 'ext4ls opdufs 4:2d /adsp', True)
                tests.append('echo OPD2413_FILE ext4 /adsp/AudioSphereModule.so.1')
                step('load', f'load opdufs 4:2d {load} /adsp/AudioSphereModule.so.1', False)
                tests.append(f'hash crc32 {load} ${{filesize}}')
                tests.append('echo OPD2413_BLK_TESTS_END')
                if a.keys:
                    step('keys', 'opd_keys 30', False)
                if a.efi_hello:
                    # Only after the observer reserved and CRC'd the ABL handoff memory.
                    efi = []
                    step('efi_memmap', 'efidebug memmap', True, efi)
                    step('efi_hello', 'bootefi hello', False, efi)
                    if a.efi_esp:
                        # The embedded ESP as a RAM block device; systemd-boot
                        # is loaded from it so its volume is that ESP.
                        efi.extend(['blkmap create esp', 'blkmap map esp 0 ${opd_esp_blocks} mem ${opd_esp_addr}',
                                    'blkmap get esp dev opd_esp_dev'])
                        step('esp_ls', 'ls blkmap ${opd_esp_dev} /EFI/BOOT', False, efi)
                        step('esp_load', f'load blkmap ${{opd_esp_dev}} {load} /EFI/BOOT/BOOTAA64.EFI', False, efi)
                        efi.append(f'hash crc32 {load} ${{filesize}}')
                        # No :size, so bootefi keeps load's blkmap device path
                        # (an explicit size switches to a bare memory path).
                        if a.efi_menu:
                            keys_ok = 'test 1 = 1' if a.ufs_blk_ramdisk else 'test "${opd_keys_ok}" = 1'
                            efi.extend([f'if {keys_ok}; then setenv stdin serial,opdkeys',
                                        'echo OPD2413_ESP menu: Volume Up/Down move, Power selects, Reboot is the default after 30 s and returns to Android'])
                            step('esp_boot', f'bootefi {load}', False, efi)
                            efi.extend(['setenv stdin serial', 'else echo OPD2413_ESP menu skipped: keys not confirmed', 'fi'])
                        else:
                            efi.append('echo OPD2413_ESP expected: systemd-boot reads the ESP, finds no entries and returns (rc=1)')
                            step('esp_boot', f'bootefi {load}', False, efi)
                    tests.extend(['if test "${opd_efi_safe}" = 1; then ' + efi[0], *efi[1:],
                                  'else echo OPD2413_EFI skipped: handoff memory not protected', 'fi'])
                if a.ufs_trace:
                    # Regression for the session-io fix: the first block read
                    # from the command line (partition 0x42) used to restart
                    # main_loop. It must now fail cleanly with rc=1.
                    step('repro', 'fatinfo opdufs 4:42', False)
                if text.count('version; echo OPD2413_OBSERVER_END;') != 1:raise ValueError('observer.env bootcmd changed')
                text = text.replace('version; echo OPD2413_OBSERVER_END;', 'version; ' + '; '.join(tests) + '; echo OPD2413_OBSERVER_END;')
                text += 'q=setenv stdout serial; setenv stderr serial\nv=setenv stdout serial,vidconsole; setenv stderr serial,vidconsole\n'
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
                                ('OPD2413_UFS_READ', a.ufs_read),
                                ('OPD2413_UFS_WAKE', a.ufs_wake),
                                ('OPD2413_UFS_BLK', a.ufs_blk),
                                ('OPD2413_UFS_BLK_RAMDISK', a.ufs_blk_ramdisk),
                                ('OPD2413_UFS_TRACE', a.ufs_trace),
                                ('OPD2413_EFI', a.efi_hello),
                                ('OPD2413_EFI_ESP', a.efi_esp is not None and not a.esp_blob),
                                ('OPD2413_EFI_ESP_BLOB', a.esp_blob),
                                ('BLKMAP', a.efi_esp is not None),
                                ('OPD2413_KEYS', a.keys)]:
            if (f'CONFIG_{symbol}=y' in config) != enabled:
                raise ValueError('Snapshot configuration mismatch: ' + symbol)
        # This observer cannot initialize or write storage or persistent env.
        for symbol in ['UFS', 'MMC', 'SCSI', 'USB', 'MTD', 'SPI_FLASH', 'FASTBOOT',
                       'CMD_SAVEENV', *([] if a.efi_hello else ['EFI_LOADER'])]:
            if f'CONFIG_{symbol}=y' in config:
                raise ValueError('Unexpected observer capability: ' + symbol)
        if a.ufs_blk:
            for symbol in ['FAT_WRITE', 'EXT4_WRITE', 'CMD_EXT4_WRITE', 'CMD_GPT', 'CMD_MBR', 'CMD_WRITE', 'CMD_MEMORY']:
                if f'CONFIG_{symbol}=y' in config:
                    raise ValueError('Read-only block build has a write capability: ' + symbol)
            for symbol in ['BLK', 'PARTITIONS', 'EFI_PARTITION', 'FS_FAT', 'FS_EXT4', 'CMD_TIME', 'CMD_HASH', 'HUSH_PARSER', 'HUSH_OLD_PARSER']:
                if f'CONFIG_{symbol}=y' not in config:
                    raise ValueError('Read-only block build is missing ' + symbol)
        if a.efi_hello:
            for symbol in ['EFI_VARIABLE_FILE_STORE', 'EFI_VARIABLE_SF_STORE', 'EFI_MM_COMM_TEE', 'EFI_RT_VOLATILE_STORE',
                           'EFI_HAVE_CAPSULE_SUPPORT', 'EFI_CAPSULE_ON_DISK', 'EFI_SET_TIME', 'EFI_TCG2_PROTOCOL',
                           'EFI_HTTP_BOOT', 'CMD_NVEDIT_EFI', 'CMD_EFICONFIG']:
                if f'CONFIG_{symbol}=y' in config:
                    raise ValueError('EFI build has a persistent or unneeded capability: ' + symbol)
            for symbol in ['EFI_VARIABLE_NO_STORE', 'CMD_BOOTEFI_HELLO', 'OPD2413_EFI', 'VIDEO_ANSI']:
                if f'CONFIG_{symbol}=y' not in config:
                    raise ValueError('EFI build is missing ' + symbol)
        if a.ufs_nop or a.ufs_read:
            # The image and the pre-relocation stack (SYS_INIT_SP_BSS_OFFSET above
            # __bss_end, used downwards) must stay clear of the UFS DMA window.
            dma, dma_end = 0xb9800000, 0xb9810000
            syms = {l.split()[2]: int(l.split()[0], 16) for l in subprocess.run(
                ['aarch64-linux-gnu-nm', out/'build/u-boot'], check=True, capture_output=True, text=True).stdout.splitlines()
                if len(l.split()) == 3}
            offset = next(int(l.split('=')[1], 16) for l in config if l.startswith('CONFIG_SYS_INIT_SP_BSS_OFFSET='))
            sp = syms['__bss_end'] + offset
            if syms['__bss_end'] > dma or (sp > dma and sp - 0x40000 < dma_end):
                raise ValueError(f'U-Boot image or early stack overlaps the DMA window (bss_end={syms["__bss_end"]:#x}, sp={sp:#x})')
        if 'CONFIG_ENV_IS_NOWHERE=y' not in config:
            raise ValueError('Observer environment must stay in RAM')
        shutil.copyfile(overlay/'layout.h', out/'opd2413_layout.h')
        shutil.copyfile(overlay/'ufs_snapshot.h', out/'opd2413_ufs_snapshot.h')
        shutil.copyfile(overlay/'smmu_snapshot.h', out/'opd2413_smmu_snapshot.h')
        shutil.copyfile(overlay/'ufs_nop.h', out/'opd2413_ufs_nop.h')
        shutil.copyfile(overlay/'ufs_link.h', out/'opd2413_ufs_link.h')
        shutil.copyfile(overlay/'ufs_read.h', out/'opd2413_ufs_read.h')
        shutil.copyfile(overlay/'ufs_blk.h', out/'opd2413_ufs_blk.h')
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
        shutil.copyfile(overlay/'keys.h', out/'opd2413_keys.h')
        run(['cc', '-g', '-O1', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
             '-I'+str(out), overlay/'keys.c', overlay/'keys_test.c', '-o', out/'keys-test'], log)
        run([out/'keys-test'], log)
    print('PASS: observer built, storage disabled, ASan/UBSan FDT and memory checks passed')
    print(out/'build/u-boot-nodtb.bin')


if __name__ == '__main__':
    main()
