#!/usr/bin/env python3
"""Repeat unattended OPD2413 boots of a display-probe image and tabulate them.

Each iteration: read-only Android prechecks (boot completed, slot b, boot_b
hash), adb reboot bootloader, fastboot boot of the probe image (whose kShimC
and systemd-boot defaults select the probe), fetch display.txt over NCM,
acknowledge on port 5009 so the tablet resets to Android, and wait for
Android again. Nothing is flashed. Stops at the first failed precheck.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import time
import urllib.request

import ncm_test

SERIAL = None
ADB = ['adb']
HTTP = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def adb(*args, timeout=40):
    return subprocess.run(ADB + list(args), capture_output=True, timeout=timeout,
                          check=True).stdout.decode().strip()


def wait_android(deadline):
    while time.monotonic() < deadline:
        if SERIAL in subprocess.run(['adb', 'devices'], capture_output=True, text=True).stdout:
            try:
                if adb('shell', 'getprop sys.boot_completed') == '1':
                    return True
            except subprocess.TimeoutExpired:
                pass
        time.sleep(3)
    return False


def precheck(boot_b):
    slot = adb('shell', 'getprop ro.boot.slot_suffix')
    digest = adb('shell', 'sha256sum /dev/block/by-name/boot_b').split()[0]
    if slot != '_b' or digest != boot_b:
        raise RuntimeError(f'precheck failed: slot={slot} boot_b={digest}')


def fastboot_boot(image):
    start = time.monotonic()
    print('phase: rebooting Android to bootloader', flush=True)
    adb('reboot', 'bootloader')
    while SERIAL not in subprocess.run(['fastboot', 'devices'], capture_output=True, text=True).stdout:
        if time.monotonic() - start > 90:
            raise TimeoutError('fastboot did not appear')
        time.sleep(0.1)
    print('phase: fastboot detected; checking slot and sending image', flush=True)
    slot = subprocess.run(['fastboot', '-s', SERIAL, 'getvar', 'current-slot'],
                          capture_output=True, text=True, timeout=20)
    if 'current-slot: b' not in slot.stdout + slot.stderr:
        raise RuntimeError('fastboot slot is not b')
    subprocess.run(['fastboot', '-s', SERIAL, 'boot', str(image)], check=True,
                   capture_output=True, timeout=120)
    print('phase: fastboot boot completed; waiting for Linux probe', flush=True)


def fetch_display(deadline):
    activated = False
    while time.monotonic() < deadline:
        name = ncm_test.host_interface()
        if not name:
            time.sleep(1)
            continue
        if not activated:
            time.sleep(3)
            address = subprocess.run(['ip', '-4', '-o', 'addr', 'show', 'dev', name],
                                     capture_output=True, text=True).stdout
            if ncm_test.HOST_ADDRESS.split('/')[0] not in address:
                # NetworkManager does not always auto-activate the MAC-bound profile.
                subprocess.run(['nmcli', 'connection', 'up', ncm_test.PROFILE, 'ifname', name],
                               capture_output=True, text=True)
            activated = True
        try:
            with HTTP.open('http://10.231.0.1/display.txt', timeout=3) as r:
                return r.read().decode(errors='replace')
        except OSError:
            time.sleep(2)
    return None


def summarize(text):
    out = {}
    for line in (text or '').splitlines():
        if '=' in line and line.split('=', 1)[0].isidentifier():
            key, value = line.split('=', 1)
            out[key] = value
        elif 'dsi_isr' in line:
            out.setdefault('dsi_isr', []).append(int(line.split()[1]))
    return out


def display_passed(summary):
    # Missing counters are failures, rather than an empty report passing.
    expected = {'panel_on': '1', 'panel_attempt_failed': '0',
                'dcs_failed': '0', 'vblank_timeout': '0'}
    return all(summary.get(k) == v for k, v in expected.items()) and \
        int(summary.get('touch_panel_prepared', '0')) > 0


def iommu_fault_count(dmesg):
    # An empty or partial HTTP response is not evidence of a fault-free boot.
    if b'Linux version ' not in dmesg or \
            b'OPD2413_LINUX_SMOKE: Linux reached userspace' not in dmesg:
        return None
    return dmesg.count(b'Unhandled context fault')


def boot_handoff(dmesg):
    # msm_mdss reports how it stopped the bootloader scanout before its reset.
    for line in dmesg.decode(errors='replace').splitlines():
        if 'bootloader scanout' in line:
            return line.split('bootloader scanout', 1)[1].lstrip(': ').strip()
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--image', type=Path, required=True)
    ap.add_argument('--sha256', required=True)
    ap.add_argument('--boot-b', required=True, help='expected boot_b SHA-256')
    ap.add_argument('--count', type=int, default=10)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--stop-on-display-failure', action='store_true',
                    help='Return to Android and stop at the first failed display report')
    ap.add_argument('--require-no-iommu-fault', action='store_true',
                    help='Fail if dmesg is unavailable or contains an SMMU context fault')
    ap.add_argument('--serial', default=os.environ.get('ANDROID_SERIAL'),
                    help='adb/fastboot serial of the tablet (default: $ANDROID_SERIAL)')
    a = ap.parse_args()
    if not a.serial:
        raise SystemExit('pass --serial or set ANDROID_SERIAL')
    global SERIAL, ADB
    SERIAL = a.serial
    ADB = ['adb', '-s', SERIAL]
    if hashlib.sha256(a.image.read_bytes()).hexdigest() != a.sha256:
        raise SystemExit('image hash mismatch')
    a.out.mkdir(parents=True, exist_ok=True)
    ncm_test.ensure_profile()
    previous_failed = None
    try:
        for i in range(1, a.count + 1):
            if not wait_android(time.monotonic() + 600):
                raise RuntimeError('Android did not come back')
            if previous_failed:
                # The probe never answered: keep the kernel log the reset left
                # in pstore before the next boot overwrites it (read-only).
                for name in ('console-ramoops-0', 'pmsg-ramoops-0'):
                    data = subprocess.run(ADB + ['exec-out', 'cat', f'/sys/fs/pstore/{name}'],
                                          capture_output=True, timeout=60).stdout
                    if data:
                        (a.out / f'pstore-{previous_failed:02d}-{name}').write_bytes(data)
            precheck(a.boot_b)
            started = time.strftime('%H:%M:%S')
            fastboot_boot(a.image)
            text = fetch_display(time.monotonic() + 240)
            row = {'run': i, 'started': started, 'fetched': text is not None, **summarize(text)}
            row['display_passed'] = display_passed(row)
            (a.out / f'display-{i:02d}.txt').write_text(text or '')
            row['iommu_faults'] = None
            if text is not None:
                try:
                    with HTTP.open('http://10.231.0.1/dmesg.txt', timeout=10) as r:
                        dmesg = r.read()
                    (a.out / f'dmesg-{i:02d}.txt').write_bytes(dmesg)
                    row['iommu_faults'] = iommu_fault_count(dmesg)
                    row['boot_handoff'] = boot_handoff(dmesg)
                except OSError:
                    pass
            if a.require_no_iommu_fault:
                row['display_passed'] &= row['iommu_faults'] == 0
            # Only acknowledge over the NCM link: without it, a host VPN/TUN
            # route may accept the connection and fake a successful ack.
            row['ack'] = False
            if ncm_test.host_interface():
                try:
                    socket.create_connection(('10.231.0.1', 5009), timeout=10).close()
                    row['ack'] = True
                except OSError:
                    pass
            with (a.out / 'runs.jsonl').open('a') as log:
                log.write(json.dumps(row) + '\n')
            print(json.dumps(row), flush=True)
            previous_failed = None if text is not None else i
            if not wait_android(time.monotonic() + 600):
                raise RuntimeError('Android did not come back after the probe')
            precheck(a.boot_b)
            if a.stop_on_display_failure and not row['display_passed']:
                raise RuntimeError(f'display failed in run {i}; logs preserved')
    finally:
        ncm_test.remove_profile()


if __name__ == '__main__':
    main()
