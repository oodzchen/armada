#!/usr/bin/env python3
"""Derive the OPD2413 smoke-test DTB from the erhai sm8750-erhai.dtb.

- UFS host and PHY disabled: the smoke test never touches storage.
- ramoops moved to Android's zone (0x880000000, 0x240000, console 0x40000,
  pmsg 0x200000, no records, ecc 0) so that Android shows the Linux console
  as /sys/fs/pstore/console-ramoops-0 after a PSCI reset.
- --no-mdss additionally disables the display subsystem and the KTZ8868
  backlight/bias chips (diagnostic only; with MDSS on, msm DRM takes over the
  ABL-lit panel and works).
"""
import argparse
import shutil
import subprocess
from pathlib import Path

RAMOOPS = '/reserved-memory/ramoops@bfff00000'
UFS = ['/soc@0/ufs@1d84000', '/soc@0/phy@1d80000']
DISPLAY = ['/soc@0/display-subsystem@ae00000',
           '/soc@0/geniqup@8c0000/i2c@88c000/backlight@11',
           '/soc@0/geniqup@9c0000/i2c@980000/backlight@11',
           '/backlight']


def fdtput(dtb, *args):
    subprocess.run(['fdtput', str(dtb), *args], check=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--input', type=Path, required=True, help='sm8750-erhai.dtb')
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--no-mdss', action='store_true')
    a = ap.parse_args()
    shutil.copyfile(a.input, a.out)
    for node in UFS + (DISPLAY if a.no_mdss else []):
        fdtput(a.out, '-t', 's', node, 'status', 'disabled')
    fdtput(a.out, '-t', 'x', RAMOOPS, 'reg', '8', '80000000', '0', '240000')
    for prop, value in [('record-size', '0'), ('console-size', '40000'),
                        ('pmsg-size', '200000'), ('ftrace-size', '0')]:
        fdtput(a.out, '-t', 'x', RAMOOPS, prop, value)
    print(a.out)


if __name__ == '__main__':
    main()
