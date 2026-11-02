#!/usr/bin/env python3
"""Validate recovery and CRC rejection using the C writer's output fixture."""
from pathlib import Path
import sys
from decode_ramconsole import decode, STRIDE
raw=bytearray(Path(sys.argv[1]).read_bytes())
assert decode(raw, True)==(b'shim\nU', 0)
size=int.from_bytes(raw[8:12], 'little')
assert decode(raw[12:12+size])[0]==b'shim\nU'
# Different damaged bits in three copies still allow bitwise majority recovery.
for i in range(14): raw[12+i]^=0xff
raw[12+STRIDE+9]^=4
raw[12+2*STRIDE+11]^=8
assert decode(raw,True)[0]==b'shim\nU'
raw=bytearray(Path(sys.argv[1]).read_bytes())
# A majority with the same damaged data must be rejected by the CRC.
for copy in range(3): raw[12+copy*STRIDE+8]^=1
try: decode(raw,True)
except ValueError as error: assert 'CRC' in str(error)
else: raise AssertionError('Corrupted majority was accepted')
print('PASS: raw/pstore decoding, metadata/text recovery and CRC rejection')
