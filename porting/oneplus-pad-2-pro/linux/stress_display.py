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


def adb(*args, timeout=40):
    return subprocess.run(ADB + list(args), capture_output=True, timeout=timeout).stdout.decode().strip()


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
    adb('reboot', 'bootloader')
    while SERIAL not in subprocess.run(['fastboot', 'devices'], capture_output=True, text=True).stdout:
        if time.monotonic() - start > 90:
            raise TimeoutError('fastboot did not appear')
        time.sleep(0.1)
    slot = subprocess.run(['fastboot', '-s', SERIAL, 'getvar', 'current-slot'],
                          capture_output=True, text=True, timeout=20)
    if 'current-slot: b' not in slot.stdout + slot.stderr:
        raise RuntimeError('fastboot slot is not b')
    subprocess.run(['fastboot', '-s', SERIAL, 'boot', str(image)], check=True,
                   capture_output=True, timeout=120)


def fetch_display(deadline):
    activated = False
    while time.monotonic() < deadline:
        name = ncm_test.host_interface()
        if name and not activated:
            time.sleep(3)
            address = subprocess.run(['ip', '-4', '-o', 'addr', 'show', 'dev', name],
                                     capture_output=True, text=True).stdout
            if ncm_test.HOST_ADDRESS.split('/')[0] not in address:
                # NetworkManager does not always auto-activate the MAC-bound profile.
                subprocess.run(['nmcli', 'connection', 'up', ncm_test.PROFILE, 'ifname', name],
                               capture_output=True, text=True)
            activated = True
        try:
            with urllib.request.urlopen('http://10.231.0.1/display.txt', timeout=3) as r:
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


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--image', type=Path, required=True)
    ap.add_argument('--sha256', required=True)
    ap.add_argument('--boot-b', required=True, help='expected boot_b SHA-256')
    ap.add_argument('--count', type=int, default=10)
    ap.add_argument('--out', type=Path, required=True)
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
            (a.out / f'display-{i:02d}.txt').write_text(text or '')
            if text is not None:
                try:
                    with urllib.request.urlopen('http://10.231.0.1/dmesg.txt', timeout=10) as r:
                        (a.out / f'dmesg-{i:02d}.txt').write_bytes(r.read())
                except OSError:
                    pass
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
    finally:
        ncm_test.remove_profile()


if __name__ == '__main__':
    main()
