#!/usr/bin/env python3
"""Check the OPD2413 boot scanout quiesce helper against a register model.

The helper runs in msm_mdss_reset() right before the MDSS core BCR is
asserted.  This checks ordering, scope and error propagation of the actual
kernel code; it is not hardware emulation.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel-source', type=Path, required=True)
    args = parser.parse_args()
    source = (args.kernel_source / 'drivers/gpu/drm/msm/msm_mdss.c').read_text()
    start = source.index('#define MDSS_BOOT_INTF_TIMING_ENGINE_EN')
    end = source.index('\nstatic void __iomem *mdss_boot_map(', start)
    fixture = Path(__file__).with_name('mdss_boot_handoff_test.c').read_text()
    assert fixture.count('/*__KERNEL_HELPERS__*/') == 1
    fixture = fixture.replace('/*__KERNEL_HELPERS__*/', source[start:end])
    with tempfile.TemporaryDirectory(prefix='opd-mdss-test-') as directory:
        directory = Path(directory)
        test_source, binary = directory / 'test.c', directory / 'test'
        test_source.write_text(fixture)
        subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-o', str(binary),
                        str(test_source)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
