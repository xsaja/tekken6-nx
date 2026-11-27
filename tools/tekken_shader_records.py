#!/usr/bin/env python3
"""tekken6-nx (2026-11-27): where Tekken 6's shader containers are in the game's files, for the web installer.

The installer page (renut_nx_ultimate_installer, lib/sources.js) makes deko3d_glsl.bin from the player's own copy. It
finds the containers of default.xex itself; for the ones in the game's files it needs records: the file, where the
stored bytes are, how they are coded, and where each container sits in the decoded bytes, with its SHA-256.

Tekken keeps its shaders in the data banks DATA301..313.BIN, XCTD files (tools/xctd.c): a 16-byte header, a table of
decoded sizes (u32 BE, one per 0x20000-byte raw block), and each raw block an independent run of XMem LZX frames
(window 2^17, 32 KB frames: [u16 BE size] or [FF u16 decoded size, u16 size]). A record here is a run of consecutive
raw blocks (most containers fit in one; one that crosses a block boundary takes the blocks it spans), coded 'xctd':
{ "file", "offset", "stored", "codec": "xctd", "blocks": [decoded size of each block], "containers": [{at, length,
sha256}] }. Its decoded bytes are the blocks' decoded bytes in order.

The tool decodes each bank with out/xctd.exe (tools/build_xctd.bat), so the offsets come from the same decoder the
dump uses. It also reports the containers of the shader dump (out/renut_shader_dump, what the NRO's shader table is
built from) that are neither in default.xex's image nor in a record: the page cannot make those.

  python tools/tekken_shader_records.py [--game ../game] [--dump out/renut_shader_dump] [--out out/release/records.json]
"""
import argparse
import glob
import hashlib
import json
import os
import struct
import subprocess
import sys
import tempfile

import xxhash

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
from renut_shader_dump import find_shader_headers  # noqa: E402

BANKS = range(301, 314)
BLOCK = 0x20000


def dump_name(blob):
    return ('vs' if blob[3] & 1 else 'ps') + '_' + xxhash.xxh3_64(blob).hexdigest() + '.bin'


def containers(data, aligned):
    for o in find_shader_headers(data, aligned=aligned):
        virt, phys = struct.unpack_from('>II', data, o + 4)
        blob = data[o:o + virt + phys]
        if len(blob) == virt + phys:
            yield o, blob


def bank_records(path, exe, work):
    raw = open(path, 'rb').read()
    if struct.unpack_from('>I', raw, 0)[0] != 0x0FF512ED:
        return [], set()
    n = (len(raw) + BLOCK - 1) // BLOCK
    sizes = list(struct.unpack_from('>%dI' % n, raw, 16))
    header = 16 + 4 * n
    starts = [0]
    for s in sizes:
        starts.append(starts[-1] + s)
    dec = os.path.join(work, os.path.basename(path) + '.dec')
    r = subprocess.run([exe, path, dec], capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit(f'{path}: the decoder reported bad blocks: {r.stdout} {r.stderr}')
    data = open(dec, 'rb').read()
    os.remove(dec)
    if len(data) != starts[-1]:
        raise SystemExit(f'{path}: decoded {len(data)} bytes, the table says {starts[-1]}')
    # group the containers by the block range they span
    by_range = {}
    names = set()
    for o, blob in containers(data, aligned=False):
        first = max(i for i in range(n) if starts[i] <= o)
        end = o + len(blob)
        last = first
        while starts[last + 1] < end:
            last += 1
        by_range.setdefault((first, last), []).append((o, blob))
        names.add(dump_name(blob))
    records = []
    rel = os.path.basename(path)
    for (first, last), items in sorted(by_range.items()):
        raw_start = max(first * BLOCK, header)
        raw_end = min((last + 1) * BLOCK, len(raw))
        seen = set()
        cs = []
        for o, blob in items:
            h = hashlib.sha256(blob).hexdigest()
            if h in seen:
                continue
            seen.add(h)
            cs.append({'at': o - starts[first], 'length': len(blob), 'sha256': h})
        records.append({'file': rel, 'offset': raw_start, 'stored': raw_end - raw_start, 'codec': 'xctd',
                        'blocks': sizes[first:last + 1], 'containers': cs})
    return records, names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', default=os.path.join(os.path.dirname(ROOT), 'game'))
    ap.add_argument('--dump', default=os.path.join(ROOT, 'out', 'renut_shader_dump'))
    ap.add_argument('--image', default=os.path.join(ROOT, 'out', 'renut_image.bin'))
    ap.add_argument('--out', default=os.path.join(ROOT, 'out', 'release', 'records.json'))
    ap.add_argument('--xctd', default=os.path.join(ROOT, 'out', 'xctd.exe'))
    a = ap.parse_args()
    os.makedirs(os.path.dirname(a.out), exist_ok=True)

    image_names = {dump_name(b) for _, b in containers(open(a.image, 'rb').read(), aligned=True)}
    records, file_names = [], set()
    with tempfile.TemporaryDirectory(dir=os.path.join(ROOT, 'out')) as work:
        for b in BANKS:
            path = os.path.join(a.game, f'DATA{b}.BIN')
            if not os.path.exists(path):
                continue
            rs, names = bank_records(path, a.xctd, work)
            records += rs
            file_names |= names
            print(f'DATA{b}: {len(rs)} records, {sum(len(r["containers"]) for r in rs)} containers, '
                  f'{len(names)} distinct', flush=True)

    dump = {os.path.basename(p) for p in glob.glob(os.path.join(a.dump, '*.bin'))}
    covered = dump & (image_names | file_names)
    missing = sorted(dump - covered)
    json.dump({'records': records}, open(a.out, 'w'), indent=1)
    stored = sum(r['stored'] for r in records)
    print(f'records: {len(records)} ({stored / 2**20:.1f} MB of stored blocks read by the page) -> {a.out}')
    print(f'dump {len(dump)}: image {len(dump & image_names)}, files {len(dump & file_names)}, '
          f'neither {len(missing)} ({sum(m.startswith("vs_") for m in missing)} vs, '
          f'{sum(m.startswith("ps_") for m in missing)} ps)')
    with open(os.path.splitext(a.out)[0] + '_missing.txt', 'w') as f:
        f.write('\n'.join(missing) + '\n')


if __name__ == '__main__':
    main()
