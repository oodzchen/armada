#!/usr/bin/env python3
"""Exercise recovery and rejection of damaged retained-RAM evidence."""
import base64
import hashlib
import random
import unittest
from decode_smoke import decode


STATE = b'kernel=7.2.6\ncpus=0-7\nfb=0 msm-drmdrmfb\n' + bytes(range(256)).hex().encode() + b'\n'


def records(damage=False, common_damage=False, drop_parts=False):
    encoded = base64.b64encode(STATE)
    digest = hashlib.sha256(STATE).hexdigest()
    lines = []
    for copy in range(5):
        value = bytearray(encoded)
        if damage:
            value[copy + 7] ^= 1
        if common_damage:
            value[7] ^= 1
        lines.append(f'OPD_STATE_V1 BEGIN final {copy} {len(STATE)} {digest}'.encode())
        for part, start in enumerate(range(0, len(value), 76)):
            if drop_parts and part == 0 and copy < 3:
                continue
            lines.append(f'OPD_STATE_V1 DATA final {copy} {part} '.encode() + value[start:start + 76])
        lines.append(f'OPD_STATE_V1 END final {copy}'.encode())
    return b'\n'.join(lines)


class Recovery(unittest.TestCase):
    def test_intact(self):
        self.assertEqual(decode(records())['final'], STATE)

    def test_distinct_bit_flips(self):
        self.assertEqual(decode(records(damage=True))['final'], STATE)

    def test_console_prefixes(self):
        log = b'\n'.join(b'[    3.123456] ' + line for line in records(damage=True).splitlines())
        self.assertEqual(decode(log)['final'], STATE)

    def test_markers_newlines_and_ring_boundary(self):
        log = bytearray(records(damage=True))
        start = 0
        for copy in range(5):
            start = log.index(f'OPD_STATE_V1 BEGIN final {copy}'.encode(), start)
            log[start + copy] ^= 1
            log[log.index(b'\n', start)] ^= 1
            start += 1
        log = log[123:] + log[:123]
        self.assertEqual(decode(log)['final'], STATE)

    def test_minority_alternative_requires_matching_digest(self):
        log = records()
        for copy in range(3):
            prefix = f'OPD_STATE_V1 DATA final {copy} 0 '.encode()
            offset = log.index(prefix) + len(prefix) + 7
            log = log[:offset] + bytes([log[offset] ^ 1]) + log[offset + 1:]
        self.assertEqual(decode(log)['final'], STATE)

    def test_twenty_five_copies_with_noise(self):
        log = bytearray((records() + b'\n') * 5)
        rng = random.Random(2413)
        for i in range(len(log)):
            if rng.random() < 0.12:
                log[i] ^= 1 << rng.randrange(8)
        self.assertEqual(decode(log)['final'], STATE)

    def test_unrecoverable_corruption_rejected(self):
        with self.assertRaises(ValueError):
            decode(records(common_damage=True))

    def test_missing_replicas_rejected(self):
        with self.assertRaises(ValueError):
            decode(records(drop_parts=True))

    def test_truncation_rejected(self):
        with self.assertRaises(ValueError):
            decode(records()[:100])


if __name__ == '__main__':
    unittest.main()
