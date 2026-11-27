#!/usr/bin/env python3
"""tekken6-nx (2026-11-05): every Xbox 360 shader container of Tekken 6's files, for reNut's build-time shader cache.

reNut translates shaders when the app is built (app/src/renut_engine/gpu/gpu.cmake: the dump folder + run-time
captures). Tekken keeps one pixel-shader set per stage in its data banks (DATA302 NSXR packages, a few in DATA303 /
DATA304), so a character or stage never played draws nothing until its shaders are captured. This tool writes them
all ahead of time (docs/tekken6-notes.md, "Shader dump plan"):

  - DATA301..313.BIN (XCTD: LZX blocks) are decoded one at a time with out/xctd.exe (tools/build_xctd.bat builds it
    on first use), scanned for containers, and the decoded file deleted;
  - the game image out/renut_image.bin (written by the code generator) is scanned too;
  - --vs-dump files (decoded NSXR packages saved from guest memory, e.g. the vertex shaders of DATA301 entry 0, whose
    inner compression is not decoded here) are scanned too.

Each container goes to <out>/{vs|ps}_<xxh3_64>.bin, the name reNut's captures use. Resumable: a bank whose
<work>/DATA3xx.done exists is skipped, files are written through a temporary name, so a power cut only costs the bank
in progress. Offline: reads ../game, writes under out/ (never the game data into the repository).

  python tools/dump_tekken_shaders.py [--game ../game] [--out out/renut_shader_dump] [--vs-dump file ...]
"""
import argparse
import glob
import os
import struct
import subprocess
import sys
import time

import xxhash

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
from renut_shader_dump import find_shader_headers  # noqa: E402

BANKS = range(301, 314)  # XCTD banks; 314 is a raw PAC, 315..320 videos


def write_containers(data, out_dir, seen):
    """Writes every container found in data; returns (found, new)."""
    found = new = 0
    for o in find_shader_headers(data, aligned=False):
        virt, phys = struct.unpack_from('>II', data, o + 4)
        blob = data[o:o + virt + phys]
        if len(blob) != virt + phys:
            continue
        name = ('vs' if blob[3] & 1 else 'ps') + '_' + xxhash.xxh3_64(blob).hexdigest() + '.bin'
        found += 1
        if name in seen:
            continue
        seen.add(name)
        path = os.path.join(out_dir, name)
        if not os.path.exists(path):
            tmp = path + '.tmp'
            with open(tmp, 'wb') as f:
                f.write(blob)
            os.replace(tmp, path)
            new += 1
    return found, new


def xctd_exe():
    exe = os.path.join(ROOT, 'out', 'xctd.exe')
    if not os.path.exists(exe):
        print('building out/xctd.exe ...', flush=True)
        subprocess.run(['cmd', '/c', os.path.join(ROOT, 'tools', 'build_xctd.bat')], check=True)
    return exe


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', default=os.path.join(os.path.dirname(ROOT), 'game'))
    ap.add_argument('--out', default=os.path.join(ROOT, 'out', 'renut_shader_dump'))
    ap.add_argument('--work', default=os.path.join(ROOT, 'out', 'shader_dump_work'))
    ap.add_argument('--vs-dump', nargs='*', default=[], help='decoded NSXR files saved from guest memory')
    ap.add_argument('--captures', default=os.path.join(ROOT, 'app', 'out', 'pc', 'shader_capture'))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    os.makedirs(a.work, exist_ok=True)
    seen = {os.path.basename(p) for p in glob.glob(os.path.join(a.out, '*.bin'))}
    print(f'{len(seen)} containers already in {a.out}', flush=True)

    exe = None
    for b in BANKS:
        done = os.path.join(a.work, f'DATA{b}.done')
        src = os.path.join(a.game, f'DATA{b}.BIN')
        if os.path.exists(done):
            print(f'DATA{b}: done before, skipped', flush=True)
            continue
        if not os.path.exists(src):
            print(f'DATA{b}: not in {a.game}, skipped', flush=True)
            continue
        exe = exe or xctd_exe()
        dec = os.path.join(a.work, f'DATA{b}.dec')
        t = time.time()
        r = subprocess.run([exe, src, dec])
        if r.returncode != 0:
            print(f'DATA{b}: decoder reported bad blocks (exit {r.returncode}); scanning what decoded', flush=True)
        with open(dec, 'rb') as f:
            data = f.read()
        found, new = write_containers(data, a.out, seen)
        del data
        os.remove(dec)
        open(done, 'w').write(f'{found} containers, {new} new\n')
        print(f'DATA{b}: {found} containers, {new} new ({time.time() - t:.0f} s)', flush=True)

    image = os.path.join(ROOT, 'out', 'renut_image.bin')
    for path in ([image] if os.path.exists(image) else []) + a.vs_dump:
        with open(path, 'rb') as f:
            found, new = write_containers(f.read(), a.out, seen)
        print(f'{os.path.basename(path)}: {found} containers, {new} new', flush=True)

    dumped = {os.path.basename(p) for p in glob.glob(os.path.join(a.out, '*.bin'))}
    caps = {os.path.basename(p) for p in glob.glob(os.path.join(a.captures, '*.bin'))}
    missing = sorted(caps - dumped)
    vs = sum(n.startswith('vs_') for n in dumped)
    print(f'total {len(dumped)} containers ({vs} vs, {len(dumped) - vs} ps); captures covered '
          f'{len(caps) - len(missing)} of {len(caps)}; missing: {sum(m.startswith("vs_") for m in missing)} vs, '
          f'{sum(m.startswith("ps_") for m in missing)} ps', flush=True)
    print('next: rebuild the app (tools\\build_pc.bat) so reNut compiles them', flush=True)


if __name__ == '__main__':
    main()
