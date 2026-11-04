#!/usr/bin/env python3
"""Check the actual OPD2413 KMS handoff helper against a register/CTL model.

This checks ordering, scope and error propagation; it is not hardware emulation.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel-source', type=Path, required=True)
    args = parser.parse_args()
    source = (args.kernel_source /
              'drivers/gpu/drm/msm/disp/dpu1/dpu_kms.c').read_text()
    start = source.index('static bool dpu_kms_needs_firmware_teardown(')
    end = source.index('\nstatic int _dpu_kms_mmu_init(', start)
    fixture = Path(__file__).with_name('kms_handoff_test.c').read_text()
    assert fixture.count('/*__KERNEL_HELPERS__*/') == 1
    fixture = fixture.replace('/*__KERNEL_HELPERS__*/', source[start:end])
    with tempfile.TemporaryDirectory(prefix='opd-kms-test-') as directory:
        directory = Path(directory)
        test_source, binary = directory / 'test.c', directory / 'test'
        test_source.write_text(fixture)
        subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-o', str(binary),
                        str(test_source)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
