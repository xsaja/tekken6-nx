#!/usr/bin/env python3
"""tekken6-nx (2026-11-24): compare guest memory snapshots of the PC build (the P key, app/src/tekken_memsnap.cpp).

  python tools/memsnap_diff.py counter snap_01.bin snap_02.bin [snap_03.bin ...]
      Big-endian u8 / u16 / u32 values that go up by exactly 1 from each snapshot to the next (the Arcade stage
      number: take one snapshot in each of two or three consecutive stages).
  python tools/memsnap_diff.py toggle snap_01.bin snap_02.bin snap_03.bin [...]
      Bytes that alternate between two values snapshot after snapshot (an option switched off / on / off ...).

Snapshots are records of (u32 little-endian guest address, u32 size, bytes), pages that were all zeros left out
(read as zeros). Candidates in the game's image (0x82000000-0x83000000) are listed first: global state lives there.
"""
import struct
import sys


def load(path):
    pages = {}
    with open(path, 'rb') as f:
        data = f.read()
    at = 0
    while at + 8 <= len(data):
        address, size = struct.unpack_from('<II', data, at)
        at += 8
        pages[address] = data[at:at + size]
        at += size
    return pages


ZERO = bytes(4096)


def page(snap, address):
    return snap.get(address, ZERO)


def counters(snaps, limit=60):
    keys = set()
    for s in snaps:
        keys.update(s)
    found = []
    for address in sorted(keys):
        pages = [page(s, address) for s in snaps]
        if all(p == pages[0] for p in pages[1:]):
            continue
        for width, fmt in ((1, '>B'), (2, '>H'), (4, '>I')):
            for off in range(0, 4096 - width + 1, width):
                values = [struct.unpack_from(fmt, p, off)[0] for p in pages]
                if all(values[i + 1] == values[i] + 1 for i in range(len(values) - 1)) and values[0] < 64:
                    found.append((address + off, width, values))
    found.sort(key=lambda x: (not (0x82000000 <= x[0] < 0x83000000), x[0]))
    print(f'{len(found)} candidates (value +1 each snapshot, first value under 64)')
    for address, width, values in found[:limit]:
        print(f'  {address:08X} u{width * 8:<2} {values}')


def toggles(snaps, limit=60):
    keys = set()
    for s in snaps:
        keys.update(s)
    found = []
    for address in sorted(keys):
        pages = [page(s, address) for s in snaps]
        if all(p == pages[0] for p in pages[1:]):
            continue
        for off in range(4096):
            values = [p[off] for p in pages]
            a, b = values[0], values[1]
            if a == b:
                continue
            if all(v == (a if i % 2 == 0 else b) for i, v in enumerate(values)):
                found.append((address + off, values))
    found.sort(key=lambda x: (not (0x82000000 <= x[0] < 0x83000000), x[0]))
    print(f'{len(found)} candidates (byte alternating between two values)')
    for address, values in found[:limit]:
        print(f'  {address:08X} {values}')


def main():
    if len(sys.argv) < 4 or sys.argv[1] not in ('counter', 'toggle'):
        sys.exit(__doc__)
    snaps = [load(p) for p in sys.argv[2:]]
    (counters if sys.argv[1] == 'counter' else toggles)(snaps)


if __name__ == '__main__':
    main()
