#!/usr/bin/env python3
"""Generate the test-only files copied over an OPD2413 RAM-booted Armada root.

The tablet boots the full Armada image from RAM (smoke_initramfs.py
--netroot). Before switch_root, the initramfs copies this tree into the
writable tmpfs layer. It never touches the read-only image itself:

* masks units that write boot partitions, install, update, or take over the
  USB gadget used for the NCM link and log;
* keeps NetworkManager off the NCM interface (fixed MAC);
* forwards the journal to kmsg, so the ACM log carries userspace messages;
* serves read-only status pages at http://10.231.0.1/ with a static busybox;
* records a digest-checked state into pmsg and reboots back to Android after
  --return-after seconds, or when the host connects to port 5009;
* disables suspend and hibernation, which hang this port so far.
"""
import argparse
import hashlib
from pathlib import Path, PurePosixPath
import shutil

DEVICE_MAC = '02:4f:50:44:00:01'

STATE = r'''#!/etc/opd/busybox sh
# Usage: state.sh STAGE. Writes /run/opd-www/status.txt and, for pmsg stages,
# 25 digest-checked copies to /dev/pmsg0 (OPD_STATE_V1, as in the initramfs).
B=/etc/opd/busybox
mkdir -p /run/opd-www
stage=$1
{
	echo "stage=$stage"
	echo "uptime=$(cat /proc/uptime)"
	echo "kernel=$(uname -r)"
	grep -E '^(NAME|VERSION|ID|VARIANT_ID|IMAGE_VERSION)=' /etc/os-release
	echo "armada_version=$(cat /usr/lib/armada/version 2>/dev/null)"
	echo "system=$(systemctl is-system-running 2>&1)"
	echo "default=$(systemctl get-default 2>&1)"
	for t in multi-user.target graphical.target; do echo "$t=$(systemctl is-active $t 2>&1)"; done
	for u in sddm.service display-manager.service NetworkManager.service systemd-logind.service; do
		echo "$u=$(systemctl is-active $u 2>&1)"
	done
	echo 'failed:'; systemctl --failed --no-legend --plain 2>&1 | head -n 40
	echo 'jobs:'; systemctl list-jobs --no-legend --plain 2>&1 | head -n 20
	echo 'sessions:'; loginctl list-sessions --no-legend 2>&1 | head -n 10
	echo 'graphics:'; $B ps -o pid,user,args 2>/dev/null | grep -Ei 'sddm|kwin|plasma|gamescope|steam|Xwayland' | grep -v grep | head -n 20
	grep -E 'MemTotal|MemAvailable' /proc/meminfo
	echo "drm=$(ls /sys/class/drm 2>/dev/null | tr '\n' ' ')"
	for f in /sys/class/udc/*/state /sys/class/udc/*/current_speed; do [ -r "$f" ] && echo "$f=$(cat $f)"; done
	echo "modules=$(wc -l < /proc/modules)"
	for c in /sys/devices/system/cpu/cpufreq/policy*; do
		[ -d "$c" ] && echo "$(basename $c)=$(cat $c/scaling_governor 2>/dev/null) cur=$(cat $c/scaling_cur_freq 2>/dev/null) max=$(cat $c/scaling_max_freq 2>/dev/null)"
	done
	echo 'gpu:'; journalctl -k -b --no-pager -o cat | grep -Ei 'adreno|gmu|zap|a8xx|msm_dpu|gpu' | tail -n 25
	echo "render=$(ls /dev/dri 2>/dev/null | tr '\n' ' ')"
	echo 'mounts:'; grep -E ' / | /usr | /var | /etc ' /proc/mounts
	if [ "$stage" != poll ]; then
		# Heavier checks (they open the GPU) only for the recorded stages.
		echo 'vulkan:'; timeout 30 vulkaninfo --summary 2>&1 | grep -E 'deviceName|driverID|driverName|driverInfo|apiVersion|deviceType' | head -n 12
		echo 'egl:'; EGL_PLATFORM=surfaceless timeout 30 eglinfo -B 2>&1 | grep -Ei 'renderer|EGL driver name|version string' | head -n 8
		echo 'gamescope:'; journalctl -b --no-pager -o cat | grep -Ei 'physical device|vulkan: |Failed to (create|initialize)' | tail -n 6
		echo 'touch:'; timeout 20 libinput list-devices 2>&1 | grep -A9 -Ei 'novatek|nt36536|touch' | grep -E 'Device|Kernel|Capabilities|Size|Calibration' | head -n 12
		journalctl -k -b --no-pager -o cat | grep -E 'nt36536' | tail -n 4
	fi
} > /run/opd-www/.status.tmp 2>&1
# Publish complete files only: httpd may read at any time.
mv /run/opd-www/.status.tmp /run/opd-www/status.txt
journalctl -b --no-pager -o short-monotonic > /run/opd-www/.journal.tmp 2>&1
mv /run/opd-www/.journal.tmp /run/opd-www/journal.txt
journalctl -b --no-pager -p warning -o short-monotonic | tail -n 200 > /run/opd-www/.warnings.tmp 2>&1
mv /run/opd-www/.warnings.tmp /run/opd-www/warnings.txt
[ "$stage" = poll ] && exit 0
digest=$(sha256sum /run/opd-www/status.txt | cut -d ' ' -f 1)
length=$(wc -c < /run/opd-www/status.txt)
$B base64 /run/opd-www/status.txt > /run/opd-www/.status.b64
replica=0
while [ $replica -lt 25 ]; do
	copy=$((replica % 5))
	echo "OPD_STATE_V1 BEGIN $stage $copy $length $digest" > /dev/pmsg0
	part=0
	while IFS= read -r line; do
		echo "OPD_STATE_V1 DATA $stage $copy $part $line" > /dev/pmsg0
		part=$((part + 1))
	done < /run/opd-www/.status.b64
	echo "OPD_STATE_V1 END $stage $copy" > /dev/pmsg0
	replica=$((replica + 1))
done
echo "opd-netroot: state $stage saved: $length bytes, sha256=$digest" > /dev/kmsg
'''

UNITS = {
    'opd-httpd.service': '''[Unit]
Description=OPD2413 test: read-only status pages on the NCM link
After=local-fs.target

[Service]
ExecStartPre=/etc/opd/busybox mkdir -p /run/opd-www
ExecStart=/etc/opd/busybox httpd -f -p 10.231.0.1:80 -h /run/opd-www
Restart=on-failure
''',
    'opd-status.service': '''[Unit]
Description=OPD2413 test: refresh status pages

[Service]
Type=oneshot
ExecStart=/etc/opd/busybox sh /etc/opd/state.sh poll
''',
    'opd-status.timer': '''[Unit]
Description=OPD2413 test: refresh status pages every 15 s

[Timer]
OnActiveSec=5
OnUnitActiveSec=15
AccuracySec=1
''',
    'opd-booted.service': '''[Unit]
Description=OPD2413 test: record state once the default target is reached
After=graphical.target multi-user.target

[Service]
Type=oneshot
ExecStart=/etc/opd/busybox sh -c 'sleep 30; exec /etc/opd/busybox sh /etc/opd/state.sh booted'
''',
    'opd-return.service': '''[Unit]
Description=OPD2413 test: record final state and reboot back to Android

[Service]
Type=oneshot
ExecStart=/etc/opd/busybox sh /etc/opd/state.sh final
ExecStart=/usr/bin/systemctl --no-block reboot
''',
    'opd-return.timer': '''[Unit]
Description=OPD2413 test: return to Android after the test window

[Timer]
OnBootSec=@RETURN@
AccuracySec=1
''',
    'opd-control.service': '''[Unit]
Description=OPD2413 test: a host connection to port 5009 ends the test window
After=opd-httpd.service

[Service]
# busybox nc cannot bind an address, and the system may also be on Wi-Fi:
# listen on the NCM address only and record who asked.
ExecStart=/usr/bin/python3 -c "import socket, subprocess; s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind(('10.231.0.1', 5009)); s.listen(1); c, peer = s.accept(); data = c.recv(4096); c.close(); open('/run/opd-www/host-request.txt', 'w').write(repr(peer) + ' ' + data.decode(errors='replace')); subprocess.run(['systemctl', 'start', 'opd-return.service'])"
Restart=on-failure
RestartSec=2
''',
}

WANTS = {
    'multi-user.target.wants': ['opd-httpd.service', 'opd-booted.service', 'opd-control.service'],
    'timers.target.wants': ['opd-status.timer', 'opd-return.timer'],
}


NO_SUSPEND = '''# OPD2413 test system: suspend/resume is not validated on this port yet and
# an idle suspend hangs the tablet. Keep it awake until that is fixed.
[Sleep]
AllowSuspend=no
AllowHibernation=no
AllowHybridSleep=no
AllowSuspendThenHibernate=no
'''


DEBUG_SHELL_UNIT = '''[Unit]
Description=OPD2413 test: root shell on the NCM link only (opd_shell.py)
After=opd-httpd.service

[Service]
ExecStart=/etc/opd/busybox telnetd -F -b 10.231.0.1:23 -l /bin/bash
Restart=on-failure
RestartSec=2
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--busybox', type=Path, required=True, help='static aarch64 busybox')
    ap.add_argument('--out', type=Path, required=True, help='new directory for the tree')
    ap.add_argument('--return-after', type=int, default=600, help='seconds after boot')
    ap.add_argument('--mask', action='append', default=[], metavar='UNIT',
                    help='Unit to mask (repeatable); must exist in the image')
    ap.add_argument('--no-ncm', action='store_true',
                    help='USB-disk boots: omit the units that serve the NCM link')
    ap.add_argument('--ssh-key', type=Path, metavar='PUBKEY',
                    help='Enable sshd with key-only root login for this public key')
    ap.add_argument('--debug-shell', action='store_true',
                    help='Unauthenticated root telnetd bound to 10.231.0.1:23 (NCM link only)')
    ap.add_argument('--firmware', action='append', default=[], metavar='NAME=SHA256=FILE',
                    help='Private firmware for /usr/lib/firmware/NAME, hash-checked (repeatable); '
                         'the initramfs drops these when the kernel command line has opd.nofw')
    a = ap.parse_args()
    if a.return_after < 120:
        raise ValueError('--return-after must be at least 120 s')
    if a.out.exists():
        raise FileExistsError(a.out)
    etc = a.out / 'etc'
    opd = etc / 'opd'
    units = etc / 'systemd/system'
    opd.mkdir(parents=True)
    units.mkdir(parents=True)
    shutil.copyfile(a.busybox, opd / 'busybox')
    (opd / 'busybox').chmod(0o755)
    (opd / 'state.sh').write_text(STATE)
    (opd / 'state.sh').chmod(0o755)
    for name, text in UNITS.items():
        (units / name).write_text(text.replace('@RETURN@', f'{a.return_after}s'))
    if a.debug_shell:
        (units / 'opd-telnetd.service').write_text(DEBUG_SHELL_UNIT)
        WANTS['multi-user.target.wants'].append('opd-telnetd.service')
    if a.no_ncm:
        for name in ('opd-httpd.service', 'opd-control.service'):
            (units / name).unlink()
            WANTS['multi-user.target.wants'].remove(name)
    if a.ssh_key:
        key = a.ssh_key.read_text().strip()
        if not key.startswith(('ssh-ed25519 ', 'ssh-rsa ', 'ecdsa-')) or '\n' in key:
            raise ValueError('expected one OpenSSH public key')
        ssh = a.out / 'var/roothome/.ssh'
        ssh.mkdir(parents=True)
        ssh.chmod(0o700)
        (ssh / 'authorized_keys').write_text(key + '\n')
        (ssh / 'authorized_keys').chmod(0o600)
        conf = etc / 'ssh/sshd_config.d'
        conf.mkdir(parents=True)
        (conf / '10-opd-test.conf').write_text(
            'PasswordAuthentication no\nKbdInteractiveAuthentication no\nPermitRootLogin prohibit-password\n')
        WANTS['multi-user.target.wants'].append('/usr/lib/systemd/system/sshd.service')
    for wants, names in WANTS.items():
        (units / wants).mkdir()
        for name in names:
            target = name if name.startswith('/') else f'../{name}'
            (units / wants / Path(name).name).symlink_to(target)
    for unit in sorted(set(a.mask)):
        if '/' in unit or not unit.endswith(('.service', '.timer', '.path', '.socket')):
            raise ValueError('bad unit name: ' + unit)
        (units / unit).symlink_to('/dev/null')
    for spec in a.firmware:
        name, expected, source = spec.split('=', 2)
        relative = PurePosixPath(name)
        if relative.is_absolute() or '..' in relative.parts or not relative.parts:
            raise ValueError('unsafe firmware name: ' + name)
        data = Path(source).read_bytes()
        if hashlib.sha256(data).hexdigest() != expected:
            raise ValueError('firmware SHA-256 mismatch: ' + name)
        target = a.out / 'usr/lib/firmware' / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        target.chmod(0o644)
    # Armada resets autologin to Game Mode on every boot; with opd.desktop on the
    # kernel command line start the desktop instead, using Armada's own helper
    # so that "Return to Game Mode" keeps working within the session.
    dropin = units / 'armada-session-default.service.d'
    dropin.mkdir()
    (dropin / '90-opd-desktop.conf').write_text(
        "[Service]\nExecStart=\nExecStart=/bin/sh -c 'if grep -qw opd.desktop /proc/cmdline; "
        "then exec /usr/libexec/armada/session-control default-desktop; "
        "else exec /usr/libexec/armada/session-control default-gamemode; fi'\n")
    nm = etc / 'NetworkManager/conf.d'
    nm.mkdir(parents=True)
    (nm / '90-opd-ncm.conf').write_text(f'[keyfile]\nunmanaged-devices=mac:{DEVICE_MAC}\n')
    sleep = etc / 'systemd/sleep.conf.d'
    sleep.mkdir(parents=True)
    (sleep / '90-opd-no-suspend.conf').write_text(NO_SUSPEND)
    journald = etc / 'systemd/journald.conf.d'
    journald.mkdir(parents=True)
    (journald / '90-opd.conf').write_text('[Journal]\nForwardToKMsg=yes\nMaxLevelKMsg=info\nStorage=volatile\n')
    print(f'{a.out}: {len(UNITS)} units, {len(set(a.mask))} masked, {len(a.firmware)} firmware')


if __name__ == '__main__':
    main()
