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

def test(binary, fdt, out, el, handoff=False):
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
    with (out/'stderr.log').open('wb') as log:
        process=subprocess.Popen(command,stdout=log,stderr=log)
        try:
            deadline=time.monotonic()+15
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
                assert b'framebuffer@fc800000 active' in text
                assert b'0x0000000a00000000' in text
                if handoff:
                    qmp('pmemsave',{'val':0xb91ff000,'size':4,'filename':str(out/'handoff.bin')})
                    marker=int.from_bytes((out/'handoff.bin').read_bytes(),'little')
                    if not marker:time.sleep(.05);continue
                    assert marker==0x50415353, 'Linux entry register/MMU contract failed'
                    assert b'OPD2413_ANDROID_HANDOFF' in text
                qmp('stop')
                qmp('pmemsave',{'val':0xfc800000,'size':3392*2400*4,'filename':str(out/'framebuffer.bin')})
                pixels=(out/'framebuffer.bin').read_bytes()
                assert 0 < pixels.count(0) < len(pixels)
                (out/'console.log').write_bytes(text)
                qmp('quit')
                print(f'EL{el}: PASS (CRC, high memory, framebuffer, END' + (', Linux entry contract)' if handoff else ')'))
                return
            raise TimeoutError('Observer END not reached')
        finally:
            if process.poll() is None:process.terminate()
            process.wait(timeout=5)


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary',type=Path,required=True)
    ap.add_argument('--fdt',type=Path,required=True)
    ap.add_argument('--out',type=Path,required=True)
    ap.add_argument('--handoff',action='store_true',help='Also verify original-kernel return with a register/MMU sentinel')
    a=ap.parse_args()
    for el in [1,2]:test(a.binary.resolve(),a.fdt.resolve(),a.out.resolve()/f'el{el}',el,a.handoff)

if __name__=='__main__':main()
