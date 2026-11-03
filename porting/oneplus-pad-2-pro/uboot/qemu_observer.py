#!/usr/bin/env python3
"""Run the exact observer binary at EL1/EL2 with OPD2413's ABL memory layout.

48 GiB of sparse guest address space covers the tablet's high physical banks;
preallocation and swap reservation are disabled. No physical device is used.
"""
import argparse
import json
from pathlib import Path
import socket
import subprocess
import time
from decode_ramconsole import decode

ENTRY = '''.global _start
.text
_start:
    ldr x0, =0xb6947000
    mov x1, #0
    mov x2, #0
    mov x3, #0
'''
DROP_EL1 = '''    mov x5, #0x80000000
    msr hcr_el2, x5
    mov x5, #3
    msr cnthctl_el2, x5
    msr cntvoff_el2, xzr
    mov x5, #0x0800
    movk x5, #0x30d0, lsl #16
    msr sctlr_el1, x5
    mov x5, #0x3c5
    msr spsr_el2, x5
    ldr x5, =0xb9000000
    msr elr_el2, x5
    eret
'''


SENTINEL = r""".global _start
.text
_start:
    b .
    b payload
    .org 0x10
    .quad 0x1000
    .org 0x38
    .ascii "ARM\x64"
    .org 0x40
payload:
    orr x4, x1, x2
    orr x4, x4, x3
    cbnz x4, fail
    cbz x0, fail
    ldr w4, [x0]
    ldr w5, =0xedfe0dd0
    cmp w4, w5
    b.ne fail
    mrs x4, CurrentEL
    cmp x4, #4
    b.eq el1
    mrs x4, sctlr_el2
    b check
el1:
    mrs x4, sctlr_el1
check:
    mov x5, #5
    tst x4, x5
    b.ne fail
    ldr w5, =0x50415353
    b mark
fail:
    ldr w5, =0x4641494c
mark:
    ldr x4, =0xb91ff000
    str w5, [x4]
    dsb sy
1:  wfe
    b 1b
"""

def test(binary, fdt, out, el, handoff=False, ufs_snapshot=None, smmu_snapshot=False, ufs_nop=False, ufs_link=False, ufs_read=False,
         loaders=(), blk_ramdisk=False):
    out.mkdir(parents=True, exist_ok=True)
    (out/'entry.S').write_text(ENTRY + (DROP_EL1 if el==1 else '    ldr x4, =0xb9000000\n    br x4\n'))
    subprocess.run(['aarch64-linux-gnu-gcc','-nostdlib','-static','-Wl,--build-id=none',
                    '-Wl,-Ttext=0x40200000',str(out/'entry.S'),'-o',str(out/'entry.elf')],check=True)
    subprocess.run(['aarch64-linux-gnu-objcopy','-O','binary',str(out/'entry.elf'),str(out/'entry.bin')],check=True)
    if handoff:
        (out/'sentinel.S').write_text(SENTINEL)
        subprocess.run(['aarch64-linux-gnu-gcc','-nostdlib','-static','-Wl,--build-id=none',
                        '-Wl,-Ttext=0xa8000000',str(out/'sentinel.S'),'-o',str(out/'sentinel.elf')],check=True)
        subprocess.run(['aarch64-linux-gnu-objcopy','-O','binary',str(out/'sentinel.elf'),str(out/'sentinel.bin')],check=True)
    sock=out/'qmp.sock';sock.unlink(missing_ok=True)
    command=['qemu-system-aarch64','-M','virt,virtualization=on,memory-backend=ram',
             '-cpu','cortex-a76','-smp','1','-m','48G','-object',
             'memory-backend-ram,id=ram,size=48G,reserve=off,prealloc=off',
             '-display','none','-serial','none','-monitor','none','-S','-no-reboot',
             '-qmp',f'unix:{sock},server=on,wait=off']
    for file,addr,extra in [(binary,0xb9000000,''),(fdt,0xb6947000,''),(out/'entry.bin',0x40200000,',cpu-num=0')]:
        command+=['-device',f'loader,file={file},addr={addr:#x}{extra}']
    if handoff:command+=['-device',f'loader,file={out}/sentinel.bin,addr=0xa8000000']
    for file,addr in loaders:command+=['-device',f'loader,file={file},addr={addr:#x}']
    if ufs_snapshot:
        # QEMU virt ROM backs these low physical addresses. Seed register values,
        # not an emulated UFS device: this checks access/gating and MMU handoff.
        values = {0x177000: 0, 0x177004: 0, 0x19e000: 0,
                  0x177028: 0x80000000, 0x1770bc: 0x80000000}
        if ufs_snapshot in ['on', 'hwcg']:
            values.update({0x177004: 0x80000000, 0x19e000: 0x80000000,
                           0x177028: 1, 0x1770bc: 1, 0x1d84008: 0x400,
                           0x1d840e4: 0x60000000, 0x1d805a8: 1})
        if ufs_snapshot == 'hwcg':
            values.update({0x177028: 0x88000003, 0x1770bc: 0x88000003})
        pages = {}
        for addr, value in values.items():
            base = addr & ~0xfff
            page = pages.setdefault(base, bytearray(4096))
            page[addr-base:addr-base+4] = value.to_bytes(4, 'little')
        for base, page in pages.items():
            path = out/f'registers-{base:x}.bin'
            path.write_bytes(page)
            command += ['-device', f'loader,file={path},addr={base:#x}']
    with (out/'stderr.log').open('wb') as log:
        process=subprocess.Popen(command,stdout=log,stderr=log)
        try:
            deadline=time.monotonic()+(60 if blk_ramdisk else 30)
            client=socket.socket(socket.AF_UNIX);client.settimeout(3)
            while True:
                if process.poll() is not None:raise RuntimeError((out/'stderr.log').read_text())
                try:client.connect(str(sock));break
                except (FileNotFoundError,ConnectionRefusedError):
                    if time.monotonic()>deadline:raise TimeoutError('QMP connect')
                    time.sleep(.05)
            stream=client.makefile('rwb',buffering=0)
            json.loads(stream.readline())
            def qmp(name,args=None):
                stream.write((json.dumps({'execute':name,'arguments':args or {}})+'\n').encode())
                while True:
                    reply=json.loads(stream.readline())
                    if 'return' in reply:return reply['return']
                    if 'error' in reply:raise RuntimeError(reply)
            qmp('qmp_capabilities');qmp('cont')
            while time.monotonic()<deadline:
                path=out/'ramconsole.bin'
                qmp('pmemsave',{'val':0x880000000,'size':0x40000,'filename':str(path)})
                try:text,_=decode(path.read_bytes(),raw=True)
                except ValueError:time.sleep(.05);continue
                if b'OPD2413_OBSERVER_END' not in text:time.sleep(.05);continue
                assert f'READY EL{el} '.encode() in text
                assert b'video probe=0' in text
                # Probe success plus non-solid physical framebuffer below;
                # short probe environments omit the bdinfo device listing.
                assert b'0x0000000a00000000' in text or b'0xa00000000' in text
                if ufs_snapshot:
                    assert b'OPD2413_UFS_RO_END' in text
                    if ufs_snapshot in ['on', 'hwcg']:
                        assert b'VER                  00000400' in text
                        assert b'HW_VERSION           60000000' in text
                        assert b'PCS_READY            00000001' in text
                        assert b'UFS_RO host skipped' not in text and b'UFS_RO PHY skipped' not in text
                    else:
                        assert b'UFS_RO host skipped' in text and b'UFS_RO PHY skipped' in text
                if smmu_snapshot:
                    # This address is an empty PCIe window on virt, not an
                    # SMMUv2. Check the invalid-ID guard here. Host fixtures
                    # exercise valid stream and context bank layouts.
                    assert b'OPD2413_SMMU_RO_END' in text
                    assert b'SMMU_RO invalid or inaccessible identification registers' in text
                if ufs_nop:
                    assert b'UFS_NOP skipped:' in text
                    assert b'UFS_NOP_PASS' not in text
                    assert b'UFS_NOP_FAIL' not in text
                if ufs_link:
                    assert b'UFS_LINK skipped:' in text
                    assert b'UFS_LINK_FAIL' not in text
                if blk_ramdisk:
                    assert b'OPD2413_UFS_BLK ramdisk' in text and b'OPD2413_BLK_TESTS_END' in text
                elif ufs_read:
                    assert b'UFS_READ skipped:' in text
                    assert b'UFS_READ_PASS' not in text
                    assert b'UFS_READ_FAIL' not in text
                    assert b'UFS_READ DMA=' not in text
                if handoff:
                    qmp('pmemsave',{'val':0xb91ff000,'size':4,'filename':str(out/'handoff.bin')})
                    marker=int.from_bytes((out/'handoff.bin').read_bytes(),'little')
                    if not marker:time.sleep(.05);continue
                    assert marker==0x50415353, 'Linux entry register/MMU contract failed'
                    if b'OPD2413_ANDROID_HANDOFF' not in text:time.sleep(.05);continue
                qmp('stop')
                qmp('pmemsave',{'val':0xfc800000,'size':3392*2400*4,'filename':str(out/'framebuffer.bin')})
                pixels=(out/'framebuffer.bin').read_bytes()
                assert 0 < pixels.count(0) < len(pixels)
                (out/'console.log').write_bytes(text)
                qmp('quit')
                print(f'EL{el}: PASS (CRC, high memory, framebuffer, END' + (', Linux entry contract)' if handoff else ')'))
                return
            try:
                text,_=decode((out/'ramconsole.bin').read_bytes(),raw=True)
                (out/'console-timeout.log').write_bytes(text)
            except (ValueError,FileNotFoundError):pass
            raise TimeoutError(f'Observer END not reached; partial log in {out}/console-timeout.log')
        finally:
            if process.poll() is None:process.terminate()
            process.wait(timeout=5)


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary',type=Path,required=True)
    ap.add_argument('--fdt',type=Path,required=True)
    ap.add_argument('--out',type=Path,required=True)
    ap.add_argument('--handoff',action='store_true',help='Also verify original-kernel return with a register/MMU sentinel')
    ap.add_argument('--ufs-snapshot', choices=['on', 'off', 'hwcg'], help='Seed fake clock/power/register state for the snapshot build (no UFS emulation)')
    ap.add_argument('--smmu-snapshot', action='store_true', help='Check safe handling of the absent SMMUv2 (requires --ufs-snapshot)')
    ap.add_argument('--ufs-nop', action='store_true', help='Check NOP preflight refuses the fake/absent hardware (requires --smmu-snapshot)')
    ap.add_argument('--ufs-link-check', action='store_true', help='Check link-state preflight refuses the fake/absent hardware')
    ap.add_argument('--ufs-read', action='store_true', help='Check the read-only survey preflight refuses the fake/absent hardware (requires --smmu-snapshot)')
    ap.add_argument('--loader', action='append', default=[], metavar='FILE@ADDR', help='Extra guest-physical file load')
    ap.add_argument('--blk-ramdisk', action='store_true', help='Expect the RAM-backed opdufs test build (--ufs-blk-ramdisk) to finish its block tests')
    a=ap.parse_args()
    loaders=[(Path(f).resolve(),int(addr,0)) for f,addr in (s.rsplit('@',1) for s in a.loader)]
    if a.smmu_snapshot and not a.ufs_snapshot:ap.error('--smmu-snapshot requires --ufs-snapshot')
    if a.ufs_nop and not a.smmu_snapshot:ap.error('--ufs-nop requires --smmu-snapshot')
    if a.ufs_read and not a.smmu_snapshot:ap.error('--ufs-read requires --smmu-snapshot')
    for el in [1,2]:test(a.binary.resolve(),a.fdt.resolve(),a.out.resolve()/f'el{el}',el,a.handoff,a.ufs_snapshot,a.smmu_snapshot,a.ufs_nop,a.ufs_link_check,a.ufs_read,
                         loaders,a.blk_ramdisk)

if __name__=='__main__':main()
