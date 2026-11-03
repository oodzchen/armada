#!/usr/bin/env python3
"""Build the OPD2413 Linux smoke-test initramfs (gzip newc cpio, no root needed).

The init script only reads: it mounts proc, sysfs and devtmpfs, prints what
the kernel found to the console, then reboots after a countdown. It never
mounts or opens a block device.
"""
import argparse
import gzip
import hashlib
from pathlib import Path, PurePosixPath

INIT = r'''#!/bin/busybox sh
/bin/busybox --install -s /bin
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
mkdir -p /run
# Every line goes to the console and to /dev/kmsg, so it also lands in the
# ramoops console zone that Android shows after the reset.
say() { echo "$*"; echo "opd-smoke: $*" > /dev/kmsg; }
# A firmware reset can flip bits in retained RAM. Save five copies with a
# digest in pmsg, independently of the human-readable console log.
record() {
	if [ -w /dev/pmsg0 ]; then echo "$*" > /dev/pmsg0
	else echo "$*" > /dev/kmsg; fi
}
snapshot() {
	stage=$1
	{
		echo "kernel=$(uname -r)"
		echo "cpus=$(cat /sys/devices/system/cpu/online)"
		grep MemTotal /proc/meminfo
		echo "efi=$(ls /sys/firmware/efi 2>/dev/null | tr '\n' ' ')"
		echo "fb=$(cat /proc/fb 2>/dev/null | tr '\n' ' ')"
		echo "drm=$(ls /sys/class/drm 2>/dev/null | tr '\n' ' ')"
		echo "block=$(ls /sys/class/block 2>/dev/null | tr '\n' ' ')"
		for e in /sys/class/input/event*; do
			[ -r "$e/device/name" ] && echo "input.$(basename $e)=$(cat $e/device/name)"
		done
		for f in /run/touch-*.events; do
			[ -f "$f" ] || continue
			echo "$(basename $f).bytes=$(wc -c < $f)"
			echo "$(basename $f).first240=$(head -c 240 $f | hexdump -v -e '/1 "%02x"')"
		done
		dmesg | grep -E 'nt36536.*(firmware|resume|chip id|fw_ver)' | tail -n 6
	} > /run/smoke-state
	digest=$(sha256sum /run/smoke-state | cut -d ' ' -f 1)
	length=$(wc -c < /run/smoke-state)
	base64 /run/smoke-state > /run/smoke-state.b64
	copy=0
	while [ $copy -lt 5 ]; do
		record "OPD_STATE_V1 BEGIN $stage $copy $length $digest"
		part=0
		while IFS= read -r line; do
			record "OPD_STATE_V1 DATA $stage $copy $part $line"
			part=$((part + 1))
		done < /run/smoke-state.b64
		record "OPD_STATE_V1 END $stage $copy"
		copy=$((copy + 1))
	done
	say "state $stage saved: $length bytes, sha256=$digest"
}
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
for e in /sys/class/input/event*; do
	if [ -r "$e/device/name" ] && grep -Eiq 'novatek|nt36536|nvt' "$e/device/name"; then
		say "capturing $(basename $e): $(cat $e/device/name)"
		timeout 115 cat /dev/input/$(basename $e) > /run/touch-$(basename $e).events &
	fi
done
snapshot initial
say "==== OPD2413_LINUX_SMOKE: rebooting in 120 s (PSCI reset, back to Android) ===="
i=120
while [ $i -gt 0 ]; do
	[ $((i % 10)) -eq 0 ] && say "reboot in $i s"
	[ $i -eq 100 ] && snapshot settled
	sleep 1
	i=$((i - 1))
done
snapshot final
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
    ap.add_argument('--firmware', action='append', default=[], metavar='NAME=SHA256=FILE',
                    help='Add a hash-verified private firmware file under /lib/firmware (repeatable)')
    a = ap.parse_args()
    items = [('dev', 0o40755), ('proc', 0o40755), ('sys', 0o40755), ('bin', 0o40755)]
    blob = b''
    ino = 1
    for name, mode in items:
        blob += entry(name, mode, ino=ino); ino += 1
    directories = set(name for name, _ in items)
    installed = set()
    for spec in a.firmware:
        name, expected, source = spec.split('=', 2)
        relative = PurePosixPath(name)
        if relative.is_absolute() or '..' in relative.parts or not relative.parts or name in installed:
            raise ValueError('Unsafe or duplicate firmware name: ' + name)
        data = Path(source).read_bytes()
        if hashlib.sha256(data).hexdigest() != expected:
            raise ValueError('Firmware SHA-256 mismatch: ' + name)
        destination = PurePosixPath('lib/firmware') / relative
        for parent in reversed(destination.parents):
            if str(parent) != '.' and str(parent) not in directories:
                blob += entry(str(parent), 0o40755, ino=ino); ino += 1
                directories.add(str(parent))
        blob += entry(str(destination), 0o100644, data, ino=ino); ino += 1
        installed.add(name)
    blob += entry('dev/console', 0o20600, ino=ino, rdev=(5, 1)); ino += 1
    blob += entry('dev/tty0', 0o20620, ino=ino, rdev=(4, 0)); ino += 1
    blob += entry('bin/busybox', 0o100755, a.busybox.read_bytes(), ino=ino); ino += 1
    blob += entry('init', 0o100755, INIT.encode(), ino=ino); ino += 1
    blob += entry('TRAILER!!!', 0)
    a.out.write_bytes(gzip.compress(blob, 9))
    print(f'{a.out}: {a.out.stat().st_size} bytes')


if __name__ == '__main__':
    main()
