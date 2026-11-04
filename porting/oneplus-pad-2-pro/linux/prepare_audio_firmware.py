#!/usr/bin/env python3
"""Stage verified private firmware for a local OnePlus image build."""
import argparse
import hashlib
from pathlib import Path
import shutil


REPO = Path(__file__).resolve().parents[3]
MANIFEST = REPO / 'system_files/usr/share/armada/firmware/oneplus-pad-2-pro.sha256'


def prepare(source, acf, destination):
    entries = []
    for line in MANIFEST.read_text().splitlines():
        if not line or line.startswith('#'):
            continue
        digest, relative = line.split()
        path = acf if relative == 'awinic/erhai/aw88261_acf.bin' else source / relative
        if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            raise ValueError(f'firmware SHA-256 mismatch: {relative}')
        entries.append((path, relative))
    # Validate the entire input before writing anything to the build context.
    for path, relative in entries:
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, target)
        target.chmod(0o644)
    return len(entries)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--firmware-dir', type=Path, required=True,
                        help='extracted firmware root containing qcom/sm8750/erhai/')
    parser.add_argument('--aw88261-acf', type=Path, required=True,
                        help='stock ODM aw882xx_acf.bin')
    parser.add_argument('--dest', type=Path,
                        default=REPO / 'system_files/usr/lib/firmware')
    args = parser.parse_args()
    try:
        count = prepare(args.firmware_dir, args.aw88261_acf, args.dest)
    except (OSError, ValueError) as error:
        parser.exit(1, f'{error}\n')
    print(f'Staged {count} verified firmware files in {args.dest}')


if __name__ == '__main__':
    main()
