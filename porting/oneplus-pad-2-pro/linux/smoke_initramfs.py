#!/usr/bin/env python3
"""Build the OPD2413 Linux smoke-test initramfs (gzip newc cpio, no root needed).

The init script only reads: it mounts proc, sysfs and devtmpfs, prints what
the kernel found to the console, then reboots after a countdown. It never
mounts or opens a block device.
"""
import argparse
import gzip
from pathlib import Path

INIT = r'''#!/bin/busybox sh
/bin/busybox --install -s /bin
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
# Every line goes to the console and to /dev/kmsg, so it also lands in the
# ramoops console zone that Android shows after the reset.
say() { echo "$*"; echo "opd-smoke: $*" > /dev/kmsg; }
# led-backlight starts powered down until a panel driver enables it; with
# MDSS disabled nothing does, so turn the ABL-lit panel's backlight back on.
for b in /sys/class/backlight/*; do
	[ -e "$b/bl_power" ] && echo 0 > "$b/bl_power" && say "backlight $(basename $b) bl_power -> $(cat $b/bl_power)"
done
say "==== OPD2413_LINUX_SMOKE: Linux reached userspace ===="
say "$(uname -a)"
say "cmdline: $(cat /proc/cmdline)"
say "cpus online: $(cat /sys/devices/system/cpu/online)"
say "$(grep MemTotal /proc/meminfo)"
say "EFI: $(ls /sys/firmware/efi 2>/dev/null | tr '\n' ' ')"
say "/proc/fb: $(cat /proc/fb 2>/dev/null | tr '\n' ' ')"
for f in /sys/class/graphics/fb0/name /sys/class/graphics/fb0/virtual_size /sys/class/graphics/fb0/bits_per_pixel /sys/class/graphics/fb0/blank; do
	[ -e "$f" ] && say "$f = $(cat $f 2>&1)"
done
say "vtcon: $(for v in /sys/class/vtconsole/vtcon*; do echo "$(basename $v)=$(cat $v/name) bind=$(cat $v/bind)"; done | tr '\n' ' ')"
for b in /sys/class/backlight/*; do
	[ -e "$b" ] && say "backlight $(basename $b): brightness=$(cat $b/brightness) actual=$(cat $b/actual_brightness 2>/dev/null) max=$(cat $b/max_brightness) power=$(cat $b/bl_power)"
done
say "regulators: $(ls /sys/class/regulator 2>/dev/null | wc -l), enabled: $(cat /sys/class/regulator/*/state 2>/dev/null | grep -c enabled)"
say "drm: $(ls /sys/class/drm 2>/dev/null | tr '\n' ' ') block (not opened): $(ls /sys/class/block 2>/dev/null | tr '\n' ' ')"
say "==== OPD2413_LINUX_SMOKE: rebooting in 120 s (PSCI reset, back to Android) ===="
i=120
while [ $i -gt 0 ]; do
	[ $((i % 10)) -eq 0 ] && say "reboot in $i s"
	sleep 1
	i=$((i - 1))
done
say "rebooting"
reboot -f
'''


def entry(name, mode, data=b'', ino=1, rdev=(0, 0)):
    name_b = name.encode() + b'\0'
    fields = [ino, mode, 0, 0, 1, 0, len(data), 0, 0, rdev[0], rdev[1], len(name_b), 0]
    head = b'070701' + b''.join(b'%08X' % f for f in fields)
    pad = lambda n: b'\0' * (-n % 4)
    return head + name_b + pad(len(head) + len(name_b)) + data + pad(len(data))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--busybox', type=Path, required=True, help='static aarch64 busybox')
    ap.add_argument('--out', type=Path, required=True)
    a = ap.parse_args()
    items = [('dev', 0o40755), ('proc', 0o40755), ('sys', 0o40755), ('bin', 0o40755)]
    blob = b''
    ino = 1
    for name, mode in items:
        blob += entry(name, mode, ino=ino); ino += 1
    blob += entry('dev/console', 0o20600, ino=ino, rdev=(5, 1)); ino += 1
    blob += entry('dev/tty0', 0o20620, ino=ino, rdev=(4, 0)); ino += 1
    blob += entry('bin/busybox', 0o100755, a.busybox.read_bytes(), ino=ino); ino += 1
    blob += entry('init', 0o100755, INIT.encode(), ino=ino); ino += 1
    blob += entry('TRAILER!!!', 0)
    a.out.write_bytes(gzip.compress(blob, 9))
    print(f'{a.out}: {a.out.stat().st_size} bytes')


if __name__ == '__main__':
    main()
