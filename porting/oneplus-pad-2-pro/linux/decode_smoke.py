#!/usr/bin/env python3
"""Recover hash-verified smoke snapshots from a possibly damaged pmsg log."""
import argparse
import base64
from collections import Counter, defaultdict
import hashlib
from pathlib import Path
import re

ALPHABET = b'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/='


def distance(a, b):
    return sum((x ^ y).bit_count() for x, y in zip(a, b))


def verified(encoded, length, digest):
    try:
        value = base64.b64decode(encoded, validate=True)
    except ValueError:
        return None
    if len(value) == length and hashlib.sha256(value).hexdigest().encode() == digest:
        return value
    return None


def recover_pmsg(data):
    """Recover fixed-length writes even when markers/newlines have bit errors.

    The raw pmsg ring may start within a record. Read through that boundary
    once, but count each physical header only once. Nothing is returned
    without matching the recorded length and full SHA-256 digest.
    """
    marker = b'OPD_STATE_V1 BEGIN '
    ring = data + data
    headers = defaultdict(list)
    for pos in range(len(data)):
        if distance(ring[pos:pos + 4], marker[:4]) > 4:
            continue
        if distance(ring[pos:pos + len(marker)], marker) > 12:
            continue
        candidates = []
        for stage in ('initial', 'settled', 'final'):
            for copy in range(5):
                prefix = f'OPD_STATE_V1 BEGIN {stage} {copy} '.encode()
                candidates.append((distance(ring[pos:pos + len(prefix)], prefix),
                                   stage, len(prefix)))
        score, stage, offset = min(candidates)
        if score > 16:
            continue
        candidates = []
        for width in range(1, 6):
            start = pos + offset
            body = ring[start:start + width + 65]
            if start + width + 65 >= len(ring):
                continue
            # Width is inferred from separators and the decimal/hex alphabets;
            # the actual length and digest are voted across separate writes.
            score = sum(min((c ^ a).bit_count() for a in b'0123456789')
                        for c in body[:width])
            score += (body[width] ^ 32).bit_count()
            score += sum(min((c ^ a).bit_count() for a in b'0123456789abcdef')
                         for c in body[width + 1:])
            score += (ring[start + width + 65] ^ 10).bit_count()
            candidates.append((score, width, body))
        if candidates:
            _, width, body = min(candidates)
            headers[stage].append((pos + offset + width + 66, body))
    states = {}
    for stage, copies in headers.items():
        try:
            metadata = majority([body for _, body in copies])
            length, digest = metadata.split(b' ', 1)
            length = int(length)
            if not 0 < length <= 65536 or not re.fullmatch(rb'[0-9a-f]{64}', digest):
                continue
            size = 4 * ((length + 2) // 3)
            count = (size + 75) // 76
            replicas = []
            for offset, _ in copies:
                encoded = b''
                start = offset
                for part in range(count):
                    prefix = f'OPD_STATE_V1 DATA {stage} 0 {part} '.encode()
                    width = min(76, size - part * 76)
                    offset += len(prefix)
                    encoded += ring[offset:offset + width]
                    offset += width + 1
                if offset - start <= len(data) and len(encoded) == size:
                    replicas.append(encoded)
            if len(replicas) < 3:
                continue
            encoded = bytearray(min(ALPHABET, key=lambda c:
                sum((r[i] ^ c).bit_count() for r in replicas)) for i in range(size))
            value = verified(encoded, length, digest)
            # If majority still misses one character, try only alternatives
            # present in a physical replica. SHA-256 decides, not expected text.
            if value is None and length <= 4096:
                for i in range(size):
                    saved = encoded[i]
                    for alternative in {r[i] for r in replicas} & set(ALPHABET) - {saved}:
                        encoded[i] = alternative
                        value = verified(encoded, length, digest)
                        if value is not None:
                            break
                    encoded[i] = saved
                    if value is not None:
                        break
            if value is not None:
                states[stage] = value
        except (ValueError, IndexError):
            continue
    return states


def majority(values):
    size = Counter(map(len, values)).most_common(1)[0][0]
    values = [v for v in values if len(v) == size]
    if len(values) < 3:
        raise ValueError('Fewer than three usable replicas')
    return bytes(sum(1 << bit for bit in range(8)
                     if sum((v >> bit) & 1 for v in group) > len(values) // 2)
                 for group in zip(*values))


def decode(data):
    headers = defaultdict(list)
    parts = defaultdict(lambda: defaultdict(dict))
    for line in data.splitlines():
        h = re.search(rb'OPD_STATE_V1 BEGIN (\w+) ([0-4]) (\d+) ([0-9a-f]{64})', line)
        if h:
            stage, copy, length, digest = h.groups()
            headers[stage].append((int(length), digest))
        d = re.search(rb'OPD_STATE_V1 DATA (\w+) ([0-4]) (\d+) (\S+)', line)
        if d:
            stage, copy, part, value = d.groups()
            parts[stage][int(part)][int(copy)] = value
    result = {}
    for stage, metadata in headers.items():
        try:
            length = Counter(x[0] for x in metadata).most_common(1)[0][0]
            if length <= 0 or length > 65536:
                raise ValueError('Invalid snapshot length')
            expected = majority([x[1] for x in metadata])
            encoded_size = 4 * ((length + 2) // 3)
            count = (encoded_size + 75) // 76
            encoded = b''.join(majority(list(parts[stage][i].values())) for i in range(count))
            snapshot = base64.b64decode(encoded, validate=True)
            if len(snapshot) != length or hashlib.sha256(snapshot).hexdigest().encode() != expected:
                raise ValueError('Snapshot SHA-256 mismatch')
            result[stage.decode()] = snapshot
        except (ValueError, KeyError, IndexError):
            continue
    for stage, value in recover_pmsg(data).items():
        result.setdefault(stage, value)
    if not result:
        raise ValueError('No complete hash-verified snapshot')
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('input', type=Path)
    ap.add_argument('--out', type=Path, required=True)
    a = ap.parse_args()
    states = decode(a.input.read_bytes())
    a.out.mkdir(parents=True, exist_ok=True)
    for stage, data in states.items():
        (a.out / (stage + '.txt')).write_bytes(data)
        print(stage + ': SHA-256 verified')


if __name__ == '__main__':
    main()
