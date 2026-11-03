#!/usr/bin/env python3
"""Capture the OPD2413 RAM ACM log; send no commands to the tablet."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import select
import termios
import time
import tty


def log_ports():
    ports = []
    for node in Path('/sys/class/tty').glob('ttyACM*'):
        for parent in (node / 'device').resolve().parents:
            try:
                identity = tuple((parent / key).read_text().strip()
                                 for key in ('idVendor', 'idProduct', 'serial'))
            except OSError:
                continue
            if identity == ('1d6b', '0104', 'armada-opd-smoke'):
                ports.append(Path('/dev') / node.name)
            break
    return ports


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--timeout', type=float, default=240)
    args = ap.parse_args()
    deadline = time.monotonic() + args.timeout
    descriptor = None
    waiting = set()
    with args.out.open('wb') as output:
        while time.monotonic() < deadline:
            ports = log_ports()
            if len(ports) > 1:
                raise RuntimeError('Multiple matching USB log gadgets')
            if ports:
                try:
                    descriptor = os.open(ports[0], os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
                except PermissionError:
                    if ports[0] not in waiting:
                        print(json.dumps({'permission_needed': str(ports[0]), 'uid': os.getuid()}), flush=True)
                        waiting.add(ports[0])
                except OSError:
                    pass
                else:
                    break
            time.sleep(0.2)
        if descriptor is None:
            raise TimeoutError('No accessible OPD2413 RAM log gadget')
        print(json.dumps({'capturing': str(ports[0])}), flush=True)
        previous = termios.tcgetattr(descriptor)
        try:
            tty.setraw(descriptor, termios.TCSANOW)
            while time.monotonic() < deadline:
                if not select.select([descriptor], [], [], 0.5)[0]:
                    continue
                try:
                    data = os.read(descriptor, 65536)
                except BlockingIOError:
                    continue
                except OSError:
                    break
                if not data:
                    break
                output.write(data)
                output.flush()
        finally:
            try:
                termios.tcsetattr(descriptor, termios.TCSANOW, previous)
            except (OSError, termios.error):
                pass
            os.close(descriptor)
    data = args.out.read_bytes()
    print(json.dumps({'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}))
    if not data:
        raise RuntimeError('USB log was empty')


if __name__ == '__main__':
    main()
