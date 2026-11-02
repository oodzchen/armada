#!/usr/bin/env python3
"""Apply the RAM-only observer overlay to a dedicated, pinned U-Boot checkout."""
from pathlib import Path
import argparse
import shutil
import subprocess
PIN = 'ee9fc27f2a2a43f253a6fe18d7a6f7c4eb0cda3b'


def replace_once(path, old, new):
    text = path.read_text()
    if new in text:
        return
    if text.count(old) != 1:
        raise ValueError(f'{path}: patch context changed')
    path.write_text(text.replace(old, new))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('source', type=Path)
    root = ap.parse_args().source.resolve()
    overlay = Path(__file__).resolve().parent
    if subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip() != PIN:
        raise ValueError('Observer audited only against ' + PIN)
    mach = root/'arch/arm/mach-snapdragon'
    for source, target in [('observer.c', mach/'opd2413_observer.c'),
                           ('layout.h', mach/'opd2413_layout.h'),
                           ('filter.c', mach/'opd2413_filter.c'),
                           ('serial_ramconsole.c', root/'drivers/serial/serial_opd2413_ramconsole.c'),
                           ('ramconsole_format.h', root/'drivers/serial/opd2413_ramconsole_format.h'),
                           ('observer_defconfig', root/'configs/opd2413_observer_defconfig'),
                           ('observer.env', root/'board/qualcomm/opd2413-observer.env')]:
        shutil.copyfile(overlay/source, target)
    (root/'arch/arm/dts/opd2413-observer.dts').write_text('/dts-v1/; / { model = "OPD2413 observer placeholder"; #address-cells=<2>; #size-cells=<2>; };\n')
    replace_once(mach/'Kconfig', 'if ARCH_SNAPDRAGON\n', 'if ARCH_SNAPDRAGON\n\nconfig OPD2413_OBSERVER\n\tbool "RAM-only OPD2413 observer (no storage)"\n\tdefault n\n')
    replace_once(mach/'Makefile', 'obj-y += dram.o', 'obj-$(CONFIG_OPD2413_OBSERVER) += opd2413_observer.o opd2413_filter.o\nobj-y += dram.o')
    replace_once(root/'drivers/serial/Makefile', 'obj-$(CONFIG_SANDBOX_SERIAL)', 'obj-$(CONFIG_OPD2413_OBSERVER) += serial_opd2413_ramconsole.o\nobj-$(CONFIG_SANDBOX_SERIAL)')
    replace_once(mach/'qcom-priv.h', 'int qcom_parse_memory(', 'int opd2413_observer_fdt(void **out);\nvoid opd2413_observer_map(void);\nint opd2413_observer_late(void);\n\nint qcom_parse_memory(')
    replace_once(mach/'board.c', 'int board_fdt_blob_setup(void **fdtp)\n{', 'int board_fdt_blob_setup(void **fdtp)\n{\n\tif (IS_ENABLED(CONFIG_OPD2413_OBSERVER)) return opd2413_observer_fdt(fdtp);')
    replace_once(mach/'board.c', 'int board_late_init(void)\n{', 'int board_late_init(void)\n{\n\tif (IS_ENABLED(CONFIG_OPD2413_OBSERVER)) return opd2413_observer_late();')
    replace_once(mach/'mem_map.c', '\tbuild_mem_map();', '\tif (IS_ENABLED(CONFIG_OPD2413_OBSERVER)) opd2413_observer_map();\n\telse build_mem_map();')
    print('Prepared OPD2413 observer at', root)

if __name__ == '__main__':
    main()
