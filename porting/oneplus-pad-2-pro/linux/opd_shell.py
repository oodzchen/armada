#!/usr/bin/env python3
"""Run commands on the OPD2413 RAM test system through its debug telnetd.

The tablet's busybox telnetd listens only on the point-to-point NCM address
(10.231.0.1:23) of a RAM-only test boot. This client refuses every telnet
option, sends each command between generated markers and prints only what the
command wrote, followed by its exit status.

    opd_shell.py 'cat /sys/class/backlight/*/brightness' 'dmesg | tail'
"""
import argparse
import re
import socket
import sys
import time
import uuid

IAC, DONT, DO, WONT, WILL, SB, SE = 255, 254, 253, 252, 251, 250, 240


class Telnet:
    def __init__(self, host, port, timeout):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)
        self.buffer = b''

    def _filter(self, data):
        out = bytearray()
        i = 0
        while i < len(data):
            byte = data[i]
            if byte != IAC:
                out.append(byte)
                i += 1
                continue
            if i + 1 >= len(data):
                break
            command = data[i + 1]
            if command in (DO, DONT, WILL, WONT) and i + 2 < len(data):
                option = data[i + 2]
                if command == DO:
                    self.sock.sendall(bytes([IAC, WONT, option]))
                elif command == WILL:
                    self.sock.sendall(bytes([IAC, DONT, option]))
                i += 3
            elif command == SB:
                end = data.find(bytes([IAC, SE]), i)
                i = len(data) if end < 0 else end + 2
            elif command == IAC:
                out.append(IAC)
                i += 2
            else:
                i += 2
        return bytes(out)

    def read_until(self, pattern, deadline):
        while True:
            match = pattern.search(self.buffer)
            if match:
                text = self.buffer[:match.end()]
                self.buffer = self.buffer[match.end():]
                return text, match
            if time.monotonic() > deadline:
                raise TimeoutError('no reply from the tablet shell')
            data = self.sock.recv(65536)
            if not data:
                raise ConnectionError('tablet shell closed the connection')
            self.buffer += self._filter(data)

    def send(self, line):
        self.sock.sendall(line.encode() + b'\r\n')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('commands', nargs='+')
    ap.add_argument('--host', default='10.231.0.1')
    ap.add_argument('--port', type=int, default=23)
    ap.add_argument('--timeout', type=float, default=60, help='seconds per command')
    a = ap.parse_args()
    shell = Telnet(a.host, a.port, a.timeout)
    tag = uuid.uuid4().hex[:12]
    # Quote-split markers so the echoed command line never contains them.
    shell.send(f"stty -echo 2>/dev/null; PS1=; PS2=; bind 'set enable-bracketed-paste off' 2>/dev/null; printf '%s\\n' 'OPD{tag}''READY'")
    shell.read_until(re.compile(f'OPD{tag}READY\r?\n'.encode()), time.monotonic() + a.timeout)
    status = 0
    for command in a.commands:
        shell.send(f"printf '%s\\n' 'OPD{tag}''BEGIN'; {command}\nprintf 'OPD{tag}''END %s\\n' $?")
        shell.read_until(re.compile(f'OPD{tag}BEGIN\r?\n'.encode()), time.monotonic() + a.timeout)
        text, match = shell.read_until(re.compile(f'OPD{tag}END (\\d+)\r?\n'.encode()),
                                       time.monotonic() + a.timeout)
        body = text[:match.start()].replace(b'\r\n', b'\n')
        sys.stdout.buffer.write(body)
        code = int(match.group(1))
        print(f'[exit {code}]', file=sys.stderr)
        status = status or code
    shell.send('exit')
    raise SystemExit(status)


if __name__ == '__main__':
    main()
