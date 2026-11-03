#!/usr/bin/env python3
"""Host side of the OPD2413 RAM USB NCM transfer test.

The tablet (10.231.0.1) runs the init from smoke_initramfs.py --usb-ncm. This
script binds an in-memory NetworkManager profile to the tablet's fixed host-side
MAC, then opens every connection itself: it sends a small and a large stream of
random data to port 5001, compares the tablet's reported lengths and SHA-256
digests (whole stream and per 64 MiB chunk), downloads the tablet's random
tx.bin over HTTP and checks it the same way, and finally sends the verdict to
port 5009, which ends the tablet's wait. Nothing is written on the tablet
except its RAM.
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

DEVICE = '10.231.0.1'
HOST_ADDRESS = '10.231.0.2/24'
HOST_MAC = '02:4f:50:44:00:02'
PROFILE = 'opd2413-ncm'
CHUNK = 64 << 20
BLOCK = 1 << 20


class Digests:
    def __init__(self):
        self.total = hashlib.sha256()
        self.chunk = hashlib.sha256()
        self.chunk_fill = 0
        self.chunks = []
        self.size = 0

    def update(self, data):
        self.total.update(data)
        self.size += len(data)
        view = memoryview(data)
        while view:
            take = min(len(view), CHUNK - self.chunk_fill)
            self.chunk.update(view[:take])
            self.chunk_fill += take
            view = view[take:]
            if self.chunk_fill == CHUNK:
                self.chunks.append(self.chunk.hexdigest())
                self.chunk = hashlib.sha256()
                self.chunk_fill = 0

    def result(self):
        chunks = list(self.chunks)
        if self.chunk_fill:
            chunks.append(self.chunk.hexdigest())
        return {'bytes': self.size, 'sha256': self.total.hexdigest(), 'chunks': chunks}


def ensure_profile():
    names = subprocess.run(['nmcli', '-t', '-f', 'NAME', 'connection', 'show'],
                           capture_output=True, text=True, check=True).stdout.split()
    if PROFILE not in names:
        # save no: the profile lives only in NetworkManager's memory.
        subprocess.run(['nmcli', 'connection', 'add', 'save', 'no', 'type', 'ethernet',
                        'con-name', PROFILE, 'ifname', '*',
                        'ethernet.mac-address', HOST_MAC,
                        'ipv4.method', 'manual', 'ipv4.addresses', HOST_ADDRESS,
                        'ipv4.never-default', 'yes', 'ipv6.method', 'disabled',
                        'connection.autoconnect', 'yes',
                        'connection.autoconnect-priority', '100'],
                       capture_output=True, text=True, check=True)


def remove_profile():
    subprocess.run(['nmcli', 'connection', 'delete', PROFILE], capture_output=True)


def host_interface():
    for node in Path('/sys/class/net').iterdir():
        try:
            if (node / 'address').read_text().strip() == HOST_MAC:
                return node.name
        except OSError:
            pass
    return None


def connect(port, deadline):
    while True:
        try:
            return socket.create_connection((DEVICE, port), timeout=5)
        except OSError:
            if time.monotonic() > deadline:
                raise TimeoutError(f'no listener on {DEVICE}:{port}')
            time.sleep(0.2)


def send_random(port, size, deadline):
    digests = Digests()
    sock = connect(port, deadline)
    sock.settimeout(60)
    start = time.monotonic()
    with sock:
        left = size
        while left:
            data = os.urandom(min(BLOCK, left))
            sock.sendall(data)
            digests.update(data)
            left -= len(data)
        sock.shutdown(socket.SHUT_WR)
        # The tablet's nc exits after reading EOF and closes its side.
        while sock.recv(4096):
            pass
    elapsed = time.monotonic() - start
    result = digests.result()
    result.update(seconds=round(elapsed, 3), mib_per_s=round(size / elapsed / BLOCK, 2))
    return result


def fetch(path, timeout=30):
    with urllib.request.urlopen(f'http://{DEVICE}{path}', timeout=timeout) as response:
        return response.read()


def parse_results(text):
    reported = {}
    for line in text.splitlines():
        fields = line.split()
        if len(fields) == 3 and fields[1].startswith('bytes='):
            entry = reported.setdefault(fields[0], {'chunks': []})
            entry['bytes'] = int(fields[1][6:])
            entry['sha256'] = fields[2].removeprefix('sha256=')
        elif len(fields) == 3 and fields[1].startswith('chunk='):
            entry = reported.setdefault(fields[0], {'chunks': []})
            if int(fields[1][6:]) != len(entry['chunks']):
                raise ValueError('out-of-order chunk line: ' + line)
            entry['chunks'].append(fields[2].removeprefix('sha256='))
        elif len(fields) == 2 and fields[1] == 'done':
            reported.setdefault(fields[0], {'chunks': []})['done'] = True
    return reported


def wait_results(label, deadline):
    while time.monotonic() < deadline:
        try:
            reported = parse_results(fetch('/results.txt').decode())
        except OSError:
            reported = {}
        if reported.get(label, {}).get('done'):
            return reported
        time.sleep(0.5)
    raise TimeoutError(f'tablet did not report {label}')


def same(local, remote):
    return (remote is not None and local['bytes'] == remote.get('bytes')
            and local['sha256'] == remote.get('sha256') and local['chunks'] == remote.get('chunks'))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, required=True, help='JSON report')
    ap.add_argument('--small-kib', type=int, default=1024)
    ap.add_argument('--large-mib', type=int, default=1024)
    ap.add_argument('--wait-link', type=float, default=600,
                    help='How long to wait for the tablet to boot and enumerate')
    ap.add_argument('--timeout', type=float, default=240,
                    help='Test limit once the link is up; keep it below the tablet --wait')
    ap.add_argument('--no-nm', action='store_true',
                    help='Address already configured (off-device tests)')
    args = ap.parse_args()
    deadline = time.monotonic() + args.wait_link
    report = {'device': DEVICE, 'host_mac': HOST_MAC}
    if not args.no_nm:
        ensure_profile()
    try:
        if not args.no_nm:
            while not (name := host_interface()):
                if time.monotonic() > deadline:
                    raise TimeoutError('NCM interface did not appear')
                time.sleep(0.2)
            report['interface'] = name
            print(json.dumps({'interface': name}), flush=True)
            time.sleep(5)
            address = subprocess.run(['ip', '-4', '-o', 'addr', 'show', 'dev', name],
                                     capture_output=True, text=True).stdout
            if HOST_ADDRESS.split('/')[0] not in address:
                # NetworkManager did not auto-activate the MAC-bound profile.
                activation = subprocess.run(['nmcli', 'connection', 'up', PROFILE, 'ifname', name],
                                            capture_output=True, text=True)
                report['manual_activation'] = activation.returncode
        deadline = time.monotonic() + args.timeout
        connect(80, deadline).close()
        print(json.dumps({'link': 'up'}), flush=True)
        report['small'] = send_random(5001, args.small_kib << 10, deadline)
        report['large'] = send_random(5001, args.large_mib << 20, deadline)
        print(json.dumps({'sent_large_mib_per_s': report['large']['mib_per_s']}), flush=True)
        reported = wait_results('large', deadline)
        report['small']['match'] = same(report['small'], reported.get('small'))
        report['large']['match'] = same(report['large'], reported.get('large'))
        digests = Digests()
        start = time.monotonic()
        with urllib.request.urlopen(f'http://{DEVICE}/tx.bin', timeout=60) as response:
            while data := response.read(BLOCK):
                digests.update(data)
        elapsed = time.monotonic() - start
        report['tx'] = digests.result()
        report['tx'].update(seconds=round(elapsed, 3),
                            mib_per_s=round(report['tx']['bytes'] / elapsed / BLOCK, 2),
                            match=same(report['tx'], reported.get('tx')))
        ok = all(report[k]['match'] for k in ('small', 'large', 'tx'))
        report['verdict'] = 'PASS' if ok else 'FAIL'
    except Exception as error:
        report['verdict'] = 'FAIL'
        report['error'] = f'{type(error).__name__}: {error}'
    finally:
        if not args.no_nm:
            report['interface_stats'] = {
                key: int((Path('/sys/class/net') / report.get('interface', '-') / 'statistics' / key).read_text())
                for key in ('rx_bytes', 'tx_bytes', 'rx_errors', 'tx_errors', 'rx_dropped', 'tx_dropped')
                if (Path('/sys/class/net') / report.get('interface', '-') / 'statistics' / key).exists()}
        summary = ' '.join(f'{k}={report[k]["match"]}:{report[k]["mib_per_s"]}MiB/s'
                           for k in ('small', 'large', 'tx') if 'match' in report.get(k, {}))
        verdict = f'{report["verdict"]} {summary} {report.get("error", "")}'.strip()
        try:
            with connect(5009, time.monotonic() + 10) as sock:
                sock.sendall(verdict.encode() + b'\n')
                sock.shutdown(socket.SHUT_WR)
                while sock.recv(4096):
                    pass
            report['verdict_sent'] = True
        except OSError:
            report['verdict_sent'] = False
        if not args.no_nm:
            remove_profile()
        args.out.write_text(json.dumps(report, indent=2) + '\n')
        print(verdict, flush=True)
    raise SystemExit(0 if report['verdict'] == 'PASS' else 1)


if __name__ == '__main__':
    main()
