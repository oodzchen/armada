#!/usr/bin/env python3
"""Regenerate the tested topology from a verified upstream source archive."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile


def main():
    here = Path(__file__).resolve().parent
    metadata = json.loads((here / 'source.json').read_text())
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', type=Path, required=True,
                        help=metadata['url'])
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if hashlib.sha256(args.archive.read_bytes()).hexdigest() != metadata['sha256']:
        parser.error('upstream archive SHA-256 mismatch')
    with tempfile.TemporaryDirectory(prefix='armada-topology-') as temporary:
        work = Path(temporary)
        with tarfile.open(args.archive) as archive:
            archive.extractall(work, filter='data')
        upstream = work / metadata['directory']
        conf = work / 'topology.conf'
        with conf.open('wb') as output:
            subprocess.run(['m4', '-I', str(here), '-I', str(upstream),
                            str(here / 'OnePlus-Pad-2-Pro.m4')],
                           stdout=output, check=True)
        blob = work / 'topology.bin'
        subprocess.run(['alsatplg', '-c', str(conf), '-o', str(blob)], check=True)
        data = blob.read_bytes()
        if hashlib.sha256(data).hexdigest() != metadata['topology_sha256']:
            raise SystemExit('generated topology differs from the hardware-tested blob')
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(data)
        print(f'{metadata["topology_sha256"]}  {args.output}')


if __name__ == '__main__':
    main()
