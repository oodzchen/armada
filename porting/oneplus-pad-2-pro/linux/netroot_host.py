#!/usr/bin/env python3
"""Host side of the OPD2413 RAM boot of the full Armada root image.

Pairs with smoke_initramfs.py --netroot. Waits for the NCM link (the same
in-memory NetworkManager profile as ncm_test.py), checks that the tablet
expects exactly this image, sends it to port 5002, waits for the tablet's
verification, then polls the read-only status pages that the booted system
serves and saves them. At the end it downloads the journal and connects to
port 5009, which makes the tablet record its final state and reboot back to
Android. Every connection is opened by the host.
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


def connect(host, port, deadline):
    while True:
        try:
            return socket.create_connection((host, port), timeout=5)
        except OSError:
            if time.monotonic() > deadline:
                raise TimeoutError(f'no listener on {host}:{port}')
            time.sleep(0.5)


def fetch(base, path, timeout=20):
    with urllib.request.urlopen(base + path, timeout=timeout) as response:
        return response.read()


def poll_text(base, path):
    try:
        return fetch(base, path).decode(errors='replace')
    except OSError:
        return None


def image_digest(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as image:
        while block := image.read(1 << 24):
            digest.update(block)
    return digest.hexdigest()


def wait_link(args, report, deadline):
    if args.no_nm:
        return
    while not (name := ncm_test.host_interface()):
        if time.monotonic() > deadline:
            raise TimeoutError('NCM interface did not appear')
        time.sleep(0.2)
    report['interface'] = name
    print(json.dumps({'interface': name}), flush=True)
    time.sleep(5)
    address = subprocess.run(['ip', '-4', '-o', 'addr', 'show', 'dev', name],
                             capture_output=True, text=True).stdout
    if ncm_test.HOST_ADDRESS.split('/')[0] not in address:
        result = subprocess.run(['nmcli', 'connection', 'up', ncm_test.PROFILE, 'ifname', name],
                                capture_output=True, text=True)
        report['manual_activation'] = result.returncode


def send_image(args, report, deadline):
    size = args.image.stat().st_size
    sock = connect(args.device, args.root_port, deadline)
    sock.settimeout(120)
    start = time.monotonic()
    sent = 0
    step = max(size // 10, 1)
    with sock, open(args.image, 'rb') as image:
        while block := image.read(1 << 20):
            sock.sendall(block)
            previous, sent = sent, sent + len(block)
            if previous // step != sent // step:
                print(json.dumps({'sent_mib': sent >> 20, 'of_mib': size >> 20,
                                  'mib_per_s': round(sent / (time.monotonic() - start) / (1 << 20), 2)}), flush=True)
        sock.shutdown(socket.SHUT_WR)
        while sock.recv(4096):
            pass
    elapsed = time.monotonic() - start
    report['send'] = {'bytes': sent, 'seconds': round(elapsed, 1), 'mib_per_s': round(sent / elapsed / (1 << 20), 2)}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--image', type=Path, required=True, help='root squashfs built into the initramfs')
    ap.add_argument('--out', type=Path, required=True, help='new directory for the report and pages')
    ap.add_argument('--observe', type=float, default=420, help='seconds to watch after switch_root')
    ap.add_argument('--wait-link', type=float, default=600)
    ap.add_argument('--check-timeout', type=float, default=900,
                    help='seconds for the tablet to hash the received image')
    ap.add_argument('--no-end', action='store_true', help='leave the return to the tablet timer')
    ap.add_argument('--no-nm', action='store_true', help='address already configured (rehearsal)')
    ap.add_argument('--device', default=ncm_test.DEVICE)
    ap.add_argument('--http-port', type=int, default=80)
    ap.add_argument('--root-port', type=int, default=5002)
    ap.add_argument('--control-port', type=int, default=5009)
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    base = f'http://{args.device}:{args.http_port}'
    size, digest = args.image.stat().st_size, image_digest(args.image)
    report = {'image': str(args.image), 'bytes': size, 'sha256': digest}
    print(json.dumps({'image_sha256': digest, 'bytes': size}), flush=True)
    if not args.no_nm:
        ncm_test.ensure_profile()
    try:
        wait_link(args, report, time.monotonic() + args.wait_link)
        deadline = time.monotonic() + 120
        while not (results := poll_text(base, '/results.txt')):
            if time.monotonic() > deadline:
                raise TimeoutError('initramfs status page not reachable')
            time.sleep(0.5)
        if f'expect bytes={size} sha256={digest}' not in results:
            raise RuntimeError('tablet expects a different image: ' + results.strip())
        print(json.dumps({'link': 'up', 'tablet_expects_this_image': True}), flush=True)
        send_image(args, report, time.monotonic() + 60)
        deadline = time.monotonic() + args.check_timeout
        while True:
            results = poll_text(base, '/results.txt') or ''
            if 'root verified' in results or 'root mismatch' in results:
                break
            # The initramfs page disappears at switch_root; the booted system's
            # page means the tablet accepted the image (it boots nothing else).
            booted = poll_text(base, '/status.txt') or ''
            if booted.startswith('stage='):
                results = 'root verified (inferred: booted system status page)'
                break
            if time.monotonic() > deadline:
                raise TimeoutError('tablet did not report the image check')
            time.sleep(1)
        report['tablet_check'] = results.strip().splitlines()
        if 'root mismatch' in results:
            raise RuntimeError('tablet rejected the image: ' + results.strip())
        print(json.dumps({'tablet_verified': True}), flush=True)
        switched = time.monotonic()
        last = None
        count = 0
        while time.monotonic() - switched < args.observe:
            status = poll_text(base, '/status.txt')
            if status and status != last:
                last = status
                count += 1
                (args.out / f'status-{count:03d}.txt').write_text(status)
                if count % 4 == 1:  # keep a recent journal in case the end is abrupt
                    journal = poll_text(base, '/journal.txt')
                    if journal:
                        (args.out / 'journal.txt').write_text(journal)
                keys = dict(line.split('=', 1) for line in status.splitlines()
                            if '=' in line and line.split('=', 1)[0] in
                            ('stage', 'system', 'graphical.target', 'sddm.service', 'ID', 'IMAGE_VERSION'))
                failed = status.split('failed:\n', 1)[-1].split('jobs:', 1)[0].strip()
                keys['failed_units'] = len(failed.splitlines()) if failed else 0
                keys['t'] = round(time.monotonic() - switched)
                print(json.dumps(keys), flush=True)
            time.sleep(5)
        report['status_pages'] = count
        for page in ('status.txt', 'journal.txt', 'warnings.txt'):
            text = poll_text(base, '/' + page)
            if text:  # never replace a saved copy with an empty reply
                (args.out / page).write_text(text)
        report['verdict'] = 'BOOTED' if count else 'NO_STATUS'
    except Exception as error:
        report['verdict'] = 'FAIL'
        report['error'] = f'{type(error).__name__}: {error}'
    finally:
        if not args.no_end:
            try:
                with connect(args.device, args.control_port, time.monotonic() + 10) as sock:
                    sock.sendall(b'end\n')
                report['end_sent'] = True
            except OSError:
                report['end_sent'] = False
        if not args.no_nm:
            ncm_test.remove_profile()
        (args.out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps({k: report.get(k) for k in ('verdict', 'error', 'send', 'end_sent')}), flush=True)
    raise SystemExit(0 if report['verdict'] == 'BOOTED' else 1)


if __name__ == '__main__':
    main()
