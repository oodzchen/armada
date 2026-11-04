#!/usr/bin/env python3
"""Build the OPD2413 Linux smoke-test initramfs (newc cpio, no root needed).

The init script only reads: it mounts proc, sysfs and devtmpfs, prints what
the kernel found to the console, then reboots after a countdown. It never
mounts or opens a block device.
"""
import argparse
import gzip
import hashlib
from pathlib import Path, PurePosixPath
import subprocess

INIT = r'''#!/bin/busybox sh
/bin/busybox --install -s /bin
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
mkdir -p /run
# Every line goes to the console and to /dev/kmsg, so it also lands in the
# ramoops console zone that Android shows after the reset.
say() { echo "$*"; echo "opd-smoke: $*" > /dev/kmsg; }
# A firmware reset can flip bits in retained RAM. Save 25 copies with a
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
		echo "usb=$(ls /sys/bus/usb/devices 2>/dev/null | tr '\n' ' ')"
		for f in /sys/class/udc/*/state /sys/class/usb_role/*/role /sys/class/typec/port*/data_role /sys/class/typec/port*/power_role /sys/class/remoteproc/remoteproc*/name /sys/class/remoteproc/remoteproc*/state; do
			[ -r "$f" ] && echo "$f=$(cat $f)"
		done
		if [ -r /sys/kernel/debug/devices_deferred ]; then
			echo 'deferred-probes:'
			cat /sys/kernel/debug/devices_deferred
		fi
		for e in /sys/class/input/event*; do
			[ -r "$e/device/name" ] && echo "input.$(basename $e)=$(cat $e/device/name)"
		done
		for f in /run/touch-*.events; do
			[ -f "$f" ] || continue
			echo "$(basename $f).bytes=$(wc -c < $f)"
			echo "$(basename $f).first240=$(head -c 240 $f | hexdump -v -e '/1 "%02x"')"
		done
		dmesg | grep -E 'nt36536.*(firmware|resume|chip id|fw_ver)' | tail -n 6
		for f in /sys/class/udc/*/current_speed /sys/class/udc/*/maximum_speed; do
			[ -r "$f" ] && echo "$f=$(cat $f)"
		done
		for c in /sys/devices/system/cpu/cpufreq/policy*; do
			[ -d "$c" ] && echo "$(basename $c)=$(cat $c/scaling_governor 2>/dev/null) cur=$(cat $c/scaling_cur_freq 2>/dev/null) max=$(cat $c/scaling_max_freq 2>/dev/null)"
		done
		grep -Ei 'dwc3|usb' /proc/interrupts | tr -s ' ' | head -n 4
		for f in /run/www/results.txt /run/ncm-host-verdict.txt; do
			[ -r "$f" ] && { echo "$f:"; cat "$f"; }
		done
		[ -e /run/www ] && ip -s link
		dmesg | grep -Ei 'remoteproc|dwc3|usb |scmi|ucsi|glink|typec|wcd939|ncm|usb0|gadget' | tail -n 60
	} > /run/smoke-state
	digest=$(sha256sum /run/smoke-state | cut -d ' ' -f 1)
	length=$(wc -c < /run/smoke-state)
	base64 /run/smoke-state > /run/smoke-state.b64
	replica=0
	while [ $replica -lt 25 ]; do
		copy=$((replica % 5))
		record "OPD_STATE_V1 BEGIN $stage $copy $length $digest"
		part=0
		while IFS= read -r line; do
			record "OPD_STATE_V1 DATA $stage $copy $part $line"
			part=$((part + 1))
		done < /run/smoke-state.b64
		record "OPD_STATE_V1 END $stage $copy"
		replica=$((replica + 1))
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
mount -t debugfs debugfs /sys/kernel/debug 2>/dev/null
# USB_LOG_HOOK
for e in /sys/class/input/event*; do
	if [ -r "$e/device/name" ] && grep -Eiq 'novatek|nt36536|nvt' "$e/device/name"; then
		say "capturing $(basename $e): $(cat $e/device/name)"
		timeout 115 cat /dev/input/$(basename $e) > /run/touch-$(basename $e).events &
	fi
done
snapshot initial
# NCM_TEST_HOOK
say "==== OPD2413_LINUX_SMOKE: rebooting in @WAIT@ s (PSCI reset, back to Android) ===="
i=@WAIT@
while [ $i -gt 0 ]; do
	[ -e /run/ncm-done ] && { say 'NCM test finished; rebooting early'; break; }
	[ $((i % 10)) -eq 0 ] && say "reboot in $i s"
	[ $i -eq $((@WAIT@ - 20)) ] && snapshot settled
	sleep 1
	i=$((i - 1))
done
snapshot final
say "rebooting"
reboot -f
'''

DEBUG_SHELL = r'''
	# Unauthenticated root shell for this RAM test, bound to the point-to-point
	# NCM address only (opd_shell.py on the host).
	mkdir -p /dev/pts && mount -t devpts devpts /dev/pts 2>/dev/null
	telnetd -b 10.231.0.1:23 -l /bin/sh && say 'netroot: debug shell on 10.231.0.1:23'
'''

USBROOT = r'''
# Boot the Armada root image from a USB disk labelled @USB_LABEL@: the cable to
# the PC is swapped for the disk after the boot menu. The squashfs image is
# checked against the SHA-256 stored next to it (and built into this
# initramfs), and the writable layer lives on the disk, so it persists.
usbroot() {
	say "==== usbroot: unplug the PC cable and plug the USB disk @USB_LABEL@ now (waiting @USB_WAIT@ s) ===="
	dev=
	i=0
	while [ -z "$dev" ] && [ $i -lt @USB_WAIT@ ]; do
		dev=$(findfs LABEL=@USB_LABEL@ 2>/dev/null)
		[ -n "$dev" ] || { sleep 2; i=$((i + 2)); [ $((i % 20)) -eq 0 ] && say "usbroot: waiting for @USB_LABEL@ ($i s), role $(cat /sys/class/usb_role/*/role 2>/dev/null)"; }
	done
	[ -n "$dev" ] || { say 'usbroot: no USB disk found'; return; }
	say "usbroot: found $dev, role $(cat /sys/class/usb_role/*/role 2>/dev/null), speed $(cat /sys/bus/usb/devices/*/speed 2>/dev/null | tr '\n' ' ')"
	mkdir -p /usb /lower /sysroot
	mount -t ext4 -o rw,noatime "$dev" /usb || { say 'usbroot: cannot mount the USB disk'; return; }
	img=/usb/opd/root.squashfs
	size=$(stat -c %s $img 2>/dev/null)
	if [ "$size" != @ROOT_SIZE@ ]; then say "usbroot: image size $size, expected @ROOT_SIZE@"; umount /usb; return; fi
	say 'usbroot: checking image SHA-256...'
	digest=$(sha256sum $img | cut -d ' ' -f 1)
	if [ "$digest" != @ROOT_SHA256@ ]; then say "usbroot: image digest mismatch: $digest"; umount /usb; return; fi
	say "usbroot: image verified, $size bytes"
	insmod /opd/overlay.ko || { say 'usbroot: overlay module failed'; umount /usb; return; }
	mount -t squashfs -o ro,loop $img /lower || { say 'usbroot: squashfs mount failed'; umount /usb; return; }
	mkdir -p /usb/opd/upper /usb/opd/work
	mount -t overlay -o lowerdir=/lower,upperdir=/usb/opd/upper,workdir=/usb/opd/work overlay /sysroot || { say 'usbroot: overlay mount failed'; return; }
	cp -af /opd/overlay/. /sysroot/ || { say 'usbroot: test overlay copy failed'; return; }
	if grep -qw opd.nofw /proc/cmdline && [ -d /opd/overlay/usr/lib/firmware ]; then
		(cd /opd/overlay && find usr/lib/firmware -type f) | while read -r f; do rm -f "/sysroot/$f"; done
		say 'usbroot: opd.nofw, test firmware removed'
	fi
	[ -x /sysroot/usr/lib/systemd/systemd ] || { say 'usbroot: no systemd in image'; return; }
	snapshot handoff
	killall httpd telnetd 2>/dev/null
	# Keep the disk reachable in the new root so shutdown remounts it read-only.
	mkdir -p /sysroot/opd-usb && mount --move /usb /sysroot/opd-usb || say 'usbroot: could not move the disk mount'
	say 'usbroot: switch_root to systemd'
	mount --move /dev /sysroot/dev && mount --move /proc /sysroot/proc && mount --move /sys /sysroot/sys || { echo 'usbroot: cannot move mounts' > /dev/console; reboot -f; }
	exec switch_root /sysroot /usr/lib/systemd/systemd
}
usbroot
'''

DISPLAY_PROBE = r'''
# Unattended display bring-up probe for repeated boots: after @PROBE_AT@ s of
# uptime, record whether the panel came up, serve it on the NCM link and
# return to Android once the host has fetched it (or after the wait).
ncm_dev=
display_probe() {
	i=0
	while [ -z "$ncm_dev" ] && [ $i -lt 40 ]; do
		for n in /sys/class/net/*; do
			[ "$(cat $n/address 2>/dev/null)" = @DEVICE_MAC@ ] && ncm_dev=$(basename $n)
		done
		[ -n "$ncm_dev" ] || sleep 1
		i=$((i + 1))
	done
	if [ -n "$ncm_dev" ]; then
		mkdir -p /run/www
		ip link set "$ncm_dev" up && ip addr add 10.231.0.1/24 dev "$ncm_dev" && httpd -p 10.231.0.1:80 -h /run/www
	fi
	while [ "$(cut -d. -f1 /proc/uptime)" -lt @PROBE_AT@ ]; do sleep 1; done
	{
		echo "uptime=$(cut -d' ' -f1 /proc/uptime)"
		grep dsi_isr /proc/interrupts | tr -s ' '
		echo "panel_on=$(dmesg | grep -c 'panel prepared')"
		echo "panel_attempt_failed=$(dmesg | grep -c 'panel on attempt .* failed')"
		echo "dcs_failed=$(dmesg | grep -c 'sending dcs data')"
		echo "pll_lock_failed=$(dmesg | grep -c 'DSI PLL(0) lock failed')"
		echo "phy_not_ready=$(dmesg | grep -c 'phy driver is not ready')"
		echo "vblank_timeout=$(dmesg | grep -c 'vblank timeout')"
		echo "touch_panel_prepared=$(dmesg | grep -c 'panel_prepared')"
		echo 'log:'
		dmesg | grep -E 'panel on|dcs data|cmd dma|PLL\(|phy driver|panel_prepared|vblank timeout|msmdrmfb|bound ae9' | head -n 40
	} > /run/www/.display.tmp
	dmesg > /run/www/.dmesg.tmp && mv /run/www/.dmesg.tmp /run/www/dmesg.txt
	mv /run/www/.display.tmp /run/www/display.txt
	say "display probe: $(grep -E '^(panel_on|panel_attempt_failed|dcs_failed)=' /run/www/display.txt | tr '\n' ' ')"
	snapshot settled
	# The host acknowledges by connecting to 5009; return to Android either way.
	[ -n "$ncm_dev" ] && timeout 90 nc -l -p 5009 < /dev/null > /dev/null
	touch /run/ncm-done
}
display_probe &
'''

USB_GADGET = r'''
# A RAM-only USB gadget: an output-only ACM kernel log and/or the NCM test link.
usb_gadget() {
	mount -t configfs configfs /sys/kernel/config || return
	g=/sys/kernel/config/usb_gadget/opd-smoke
	mkdir -p "$g" || return
	echo 0x1d6b > "$g/idVendor" || return
	echo 0x0104 > "$g/idProduct" || return
	mkdir -p "$g/strings/0x409" "$g/configs/c.1/strings/0x409"
	echo armada-opd-smoke > "$g/strings/0x409/serialnumber"
	echo Armada > "$g/strings/0x409/manufacturer"
	echo 'OPD2413 RAM kernel log' > "$g/strings/0x409/product"
	echo '@FUNCTIONS@' > "$g/configs/c.1/strings/0x409/configuration"
	echo 100 > "$g/configs/c.1/MaxPower"
@FUNCTION_SETUP@	i=0
	while [ $i -lt 30 ]; do
		set -- /sys/class/udc/*
		if [ -e "$1" ]; then
			[ $# -eq 1 ] || { say 'USB log: ambiguous UDC'; return; }
			udc=$(basename "$1")
			echo "$udc" > "$g/UDC" || return
			say "USB gadget: bound to $udc"
			[ -e "$g/functions/acm.GS0" ] && cat /dev/kmsg > /dev/ttyGS0
			return
		fi
		sleep 1
		i=$((i + 1))
	done
	say 'USB gadget: no UDC after 30 s'
}
usb_gadget &
'''

ACM_SETUP = r'''	mkdir "$g/functions/acm.GS0" || return
	ln -s "$g/functions/acm.GS0" "$g/configs/c.1/acm.GS0" || return
'''

# Fixed, locally administered MACs let the host bind a profile to this link only.
NCM_DEVICE_MAC = '02:4f:50:44:00:01'
NCM_HOST_MAC = '02:4f:50:44:00:02'

NCM_SETUP = r'''	mkdir "$g/functions/ncm.usb0" || return
	echo @DEVICE_MAC@ > "$g/functions/ncm.usb0/dev_addr" || return
	echo @HOST_MAC@ > "$g/functions/ncm.usb0/host_addr" || return
	ln -s "$g/functions/ncm.usb0" "$g/configs/c.1/ncm.usb0" || return
'''

NCM_TEST = r'''
# Point-to-point USB NCM transfer test (10.231.0.1 here, 10.231.0.2 on the host).
# The host opens every connection. Received data stays in RAM; only lengths and
# SHA-256 digests (whole file and per 64 MiB chunk) are reported back.
ncm_dev=
R=/run/www/results.txt
chunk_sums() {
	size=$(stat -c %s "$1")
	echo "$2 bytes=$size sha256=$(sha256sum "$1" | cut -d ' ' -f 1)" >> $R
	n=0
	while [ $((n * 67108864)) -lt $size ]; do
		echo "$2 chunk=$n sha256=$(dd if="$1" bs=1048576 skip=$((n * 64)) count=64 2>/dev/null | sha256sum | cut -d ' ' -f 1)" >> $R
		n=$((n + 1))
	done
}
ncm_test() {
	i=0
	while [ -z "$ncm_dev" ] && [ $i -lt 40 ]; do
		for n in /sys/class/net/*; do
			[ "$(cat $n/address 2>/dev/null)" = @DEVICE_MAC@ ] && ncm_dev=$(basename $n)
		done
		[ -n "$ncm_dev" ] || sleep 1
		i=$((i + 1))
	done
	[ -n "$ncm_dev" ] || { say 'NCM: no interface after 40 s'; return; }
	mkdir -p /run/www
	dd if=/dev/urandom of=/run/www/tx.bin bs=1048576 count=@TX_MIB@ 2>/dev/null
	chunk_sums /run/www/tx.bin tx
	ip link set "$ncm_dev" up && ip addr add 10.231.0.1/24 dev "$ncm_dev" || { say "NCM: cannot configure $ncm_dev"; return; }
	httpd -p 10.231.0.1:80 -h /run/www || { say 'NCM: httpd failed'; return; }
	say "NCM: $ncm_dev up at 10.231.0.1, speed $(cat /sys/class/udc/*/current_speed 2>/dev/null)"
	for label in small large; do
		nc -l -p 5001 < /dev/null > /run/rx-$label.bin || say "NCM: $label receive failed"
		chunk_sums /run/rx-$label.bin $label
		echo "$label done" >> $R
		say "NCM: $(grep "^$label bytes=" $R)"
		rm -f /run/rx-$label.bin
	done
	nc -l -p 5009 < /dev/null > /run/ncm-host-verdict.txt
	say "NCM host verdict: $(head -c 200 /run/ncm-host-verdict.txt)"
	touch /run/ncm-done
}
ncm_test &
'''

NETROOT = r'''
# Download the Armada root image over NCM into its own RAM tmpfs, check its
# length and SHA-256 against the values built into this initramfs, then boot it
# read-only with a tmpfs writable layer. Any failure returns to the countdown
# and the reset back to Android. No block device other than loop is used.
ncm_dev=
R=/run/www/results.txt
netroot() {
	i=0
	while [ -z "$ncm_dev" ] && [ $i -lt 60 ]; do
		for n in /sys/class/net/*; do
			[ "$(cat $n/address 2>/dev/null)" = @DEVICE_MAC@ ] && ncm_dev=$(basename $n)
		done
		[ -n "$ncm_dev" ] || sleep 1
		i=$((i + 1))
	done
	[ -n "$ncm_dev" ] || { say 'netroot: no NCM interface after 60 s'; return; }
	mkdir -p /run/www
	ip link set "$ncm_dev" up && ip addr add 10.231.0.1/24 dev "$ncm_dev" || { say "netroot: cannot configure $ncm_dev"; return; }
	httpd -p 10.231.0.1:80 -h /run/www || { say 'netroot: httpd failed'; return; }
	# DEBUG_SHELL_HOOK
	echo "expect bytes=@ROOT_SIZE@ sha256=@ROOT_SHA256@" > $R
	# Without a verified image in time, record the state and return to Android.
	( sleep @NETROOT_TIMEOUT@; say 'netroot: no verified image in time'; snapshot final; reboot -f ) &
	watchdog=$!
	say "netroot: $ncm_dev up at 10.231.0.1, speed $(cat /sys/class/udc/*/current_speed 2>/dev/null)"
	mkdir -p /netroot
	mount -t tmpfs -o size=@ROOT_TMPFS_MIB@m,mode=0700 netroot /netroot || { say 'netroot: tmpfs failed'; return; }
	nc -l -p 5002 < /dev/null > /netroot/root.squashfs || { say 'netroot: receive failed'; return; }
	size=$(stat -c %s /netroot/root.squashfs)
	digest=$(sha256sum /netroot/root.squashfs | cut -d ' ' -f 1)
	echo "root bytes=$size sha256=$digest" >> $R
	if [ "$size" != @ROOT_SIZE@ ] || [ "$digest" != @ROOT_SHA256@ ]; then
		echo 'root mismatch' >> $R
		say "netroot: image mismatch: $size bytes, sha256=$digest"
		kill $watchdog
		return
	fi
	echo 'root verified' >> $R
	say "netroot: image verified, $size bytes"
	insmod /opd/overlay.ko || { say 'netroot: overlay module failed'; return; }
	mkdir -p /lower /rw /sysroot
	mount -t squashfs -o ro,loop /netroot/root.squashfs /lower || { say 'netroot: squashfs mount failed'; return; }
	mount -t tmpfs -o size=@UPPER_MIB@m,mode=0755 rw /rw && mkdir -p /rw/upper /rw/work || { say 'netroot: upper tmpfs failed'; return; }
	mount -t overlay -o lowerdir=/lower,upperdir=/rw/upper,workdir=/rw/work overlay /sysroot || { say 'netroot: overlay mount failed'; return; }
	cp -af /opd/overlay/. /sysroot/ || { say 'netroot: test overlay copy failed'; return; }
	if grep -qw opd.nofw /proc/cmdline && [ -d /opd/overlay/usr/lib/firmware ]; then
		(cd /opd/overlay && find usr/lib/firmware -type f) | while read -r f; do rm -f "/sysroot/$f"; done
		say 'netroot: opd.nofw, test firmware removed'
	fi
	[ -x /sysroot/usr/lib/systemd/systemd ] || { say 'netroot: no systemd in image'; return; }
	echo 'switching root' >> $R
	snapshot handoff
	kill $watchdog
	killall httpd telnetd 2>/dev/null
	say 'netroot: switch_root to systemd'
	mount --move /dev /sysroot/dev && mount --move /proc /sysroot/proc && mount --move /sys /sysroot/sys || { echo 'netroot: cannot move mounts' > /dev/console; reboot -f; }
	exec switch_root /sysroot /usr/lib/systemd/systemd
}
netroot
'''


def entry(name, mode, data=b'', ino=1, rdev=(0, 0)):
    name_b = name.encode() + b'\0'
    fields = [ino, mode, 0, 0, 1, 0, len(data), 0, 0, rdev[0], rdev[1], len(name_b), 0]
    head = b'070701' + b''.join(b'%08X' % f for f in fields)
    pad = lambda n: b'\0' * (-n % 4)
    return head + name_b + pad(len(head) + len(name_b)) + data + pad(len(data))


def tree_entries(root, prefix):
    """Directories, files and symlinks under root, as (name, mode, data); owned by root."""
    yield prefix.split('/')[0], 0o40755, b''
    yield prefix, 0o40755, b''
    for path in sorted(root.rglob('*')):
        name = f'{prefix}/{path.relative_to(root).as_posix()}'
        if path.is_symlink():
            yield name, 0o120777, str(path.readlink()).encode()
        elif path.is_dir():
            yield name, 0o40000 | (path.stat().st_mode & 0o7777), b''
        elif path.is_file():
            yield name, 0o100000 | (path.stat().st_mode & 0o7777), path.read_bytes()
        else:
            raise ValueError('unsupported file type: ' + str(path))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--busybox', type=Path, required=True, help='static aarch64 busybox')
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--compression', choices=('gzip', 'zstd'), default='gzip',
                    help='zstd requires host zstd and kernel CONFIG_RD_ZSTD=y')
    ap.add_argument('--usb-log', action='store_true',
                    help='Start an output-only ACM kernel log gadget (CONFIG_USB_CONFIGFS_ACM=y)')
    ap.add_argument('--usb-ncm', action='store_true',
                    help='Add the NCM transfer test (CONFIG_USB_CONFIGFS_NCM=y); ends the wait early')
    ap.add_argument('--ncm-tx-mib', type=int, default=256,
                    help='Random data the tablet generates for the host to download')
    ap.add_argument('--netroot', metavar='SQUASHFS',
                    help='Boot this root image received over NCM (needs --overlay-module, --netroot-tree)')
    ap.add_argument('--usbroot', metavar='SQUASHFS',
                    help='Boot this root image from the USB disk --usb-label (writable layer on the disk)')
    ap.add_argument('--usb-label', default='OPDARMADA')
    ap.add_argument('--usb-wait', type=int, default=300, help='seconds to wait for the USB disk')
    ap.add_argument('--overlay-module', type=Path, help='overlay.ko from the same kernel build')
    ap.add_argument('--netroot-tree', type=Path, help='Test files copied over the root (netroot_overlay.py)')
    ap.add_argument('--upper-mib', type=int, default=3072, help='tmpfs writable layer size')
    ap.add_argument('--debug-shell', action='store_true',
                    help='With --netroot: unauthenticated telnetd on 10.231.0.1:23 until switch_root')
    ap.add_argument('--netroot-timeout', type=int, default=1200,
                    help='Reset back to Android unless the image is verified within this time')
    ap.add_argument('--display-probe', type=int, metavar='SECONDS',
                    help='Record panel bring-up at this uptime, serve it over NCM, then reboot')
    ap.add_argument('--wait', type=int, default=120,
                    help='Seconds before the PSCI reset back to Android')
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
    if a.wait < 30 or not 1 <= a.ncm_tx_mib <= 4096:
        raise ValueError('--wait must be at least 30 s and --ncm-tx-mib 1..4096')
    if a.netroot and (a.usb_ncm or not a.overlay_module or not a.netroot_tree):
        raise ValueError('--netroot replaces --usb-ncm and needs --overlay-module and --netroot-tree')
    if a.usbroot and (a.netroot or a.usb_ncm or not a.overlay_module or not a.netroot_tree):
        raise ValueError('--usbroot excludes --netroot/--usb-ncm and needs --overlay-module and --netroot-tree')
    root_size = root_sha256 = None
    root_image = a.netroot or a.usbroot
    if root_image:
        digest = hashlib.sha256()
        with open(root_image, 'rb') as image:
            while block := image.read(1 << 24):
                digest.update(block)
        root_size, root_sha256 = Path(root_image).stat().st_size, digest.hexdigest()
        for name, mode, data in tree_entries(a.netroot_tree, 'opd/overlay'):
            blob += entry(name, mode, data, ino=ino); ino += 1
        blob += entry('opd/overlay.ko', 0o100644, a.overlay_module.read_bytes(), ino=ino); ino += 1
    functions = ([('ACM log', ACM_SETUP)] if a.usb_log else []) + \
                ([('NCM test', NCM_SETUP)] if a.usb_ncm or a.netroot or a.display_probe else [])
    gadget = ''
    if functions:
        gadget = USB_GADGET.replace('@FUNCTIONS@', ' + '.join(name for name, _ in functions)) \
                           .replace('@FUNCTION_SETUP@', ''.join(setup for _, setup in functions))
    init = INIT.replace('# USB_LOG_HOOK', gadget) \
               .replace('# NCM_TEST_HOOK', NETROOT if a.netroot else USBROOT if a.usbroot else NCM_TEST if a.usb_ncm
                        else DISPLAY_PROBE if a.display_probe else '') \
               .replace('@PROBE_AT@', str(a.display_probe or 0)) \
               .replace('@USB_LABEL@', a.usb_label).replace('@USB_WAIT@', str(a.usb_wait)) \
               .replace('\t# DEBUG_SHELL_HOOK\n', DEBUG_SHELL.lstrip('\n') if a.debug_shell else '') \
               .replace('@WAIT@', str(a.wait)) \
               .replace('@TX_MIB@', str(a.ncm_tx_mib)) \
               .replace('@ROOT_SIZE@', str(root_size)).replace('@ROOT_SHA256@', str(root_sha256)) \
               .replace('@ROOT_TMPFS_MIB@', str(((root_size or 0) >> 20) + 64)) \
               .replace('@UPPER_MIB@', str(a.upper_mib)) \
               .replace('@NETROOT_TIMEOUT@', str(a.netroot_timeout)) \
               .replace('@DEVICE_MAC@', NCM_DEVICE_MAC).replace('@HOST_MAC@', NCM_HOST_MAC)
    assert '@' not in init.replace('$@', ''), 'unreplaced placeholder'
    blob += entry('init', 0o100755, init.encode(), ino=ino); ino += 1
    blob += entry('TRAILER!!!', 0)
    if a.compression == 'zstd':
        compressed = subprocess.run(['zstd', '-q', '-19', '-T1', '-c'], input=blob,
                                    stdout=subprocess.PIPE, check=True).stdout
    else:
        compressed = gzip.compress(blob, 9)
    a.out.write_bytes(compressed)
    print(f'{a.out}: {a.out.stat().st_size} bytes')


if __name__ == '__main__':
    main()
