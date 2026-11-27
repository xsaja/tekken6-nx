#!/usr/bin/env python3
"""tekken6-nx (2026-11-05): moves Renut's non-state hooks from katamari-nx's addresses to Tekken 6's.

Reads tools/renut_hook_map_tekken.txt (`<katamari addr>  <tekken addr or ->  <evidence>`). In the hook sources each
`BANJO_RENUT_HOOK*(sub_<katamari>` becomes `sub_<tekken>`; a `-` turns the hook into its BANJO_RENUT_UNHOOKED_* form
(compiled, registered nowhere). Every changed line gets a `tekken6-nx:` note. Hooks not in the map are listed; two hooks
at the same Tekken address are refused (they would define the same guest function twice).

  python tools/renut_hook_remap_tekken.py [--dry-run]
"""
import argparse
import collections
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOOKS = os.path.join(ROOT, 'app', 'src', 'renut_engine', 'gpu', 'd3d', 'hooks')
MAP = os.path.join(ROOT, 'tools', 'renut_hook_map_tekken.txt')
HOOK = re.compile(r'\bBANJO_RENUT_(HOOK(?:_RAW)?(?:_SHARED)?)\(sub_([0-9A-F]{8})')


def read_map():
    out = {}
    for line in open(MAP, encoding='utf-8'):
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        kat, tek = line.split()[:2]
        kat = kat.upper()
        if kat in out:
            sys.exit(f'{kat} appears twice in the map')
        out[kat] = None if tek == '-' else tek.upper()
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dry-run', action='store_true')
    a = ap.parse_args()
    mapping = read_map()
    targets = collections.Counter(v for v in mapping.values() if v)
    dup = [t for t, n in targets.items() if n > 1]
    if dup:
        sys.exit('two hooks at the same Tekken address: ' + ', '.join(dup))
    moved = unhooked = 0
    unmapped = []
    for path in sorted(glob.glob(os.path.join(HOOKS, '*.cpp'))):
        raw = open(path, encoding='utf-8', newline='').read()
        nl = '\r\n' if '\r\n' in raw else '\n'
        lines = raw.split(nl)
        for i, line in enumerate(lines):
            m = HOOK.search(line)
            if not m or 'state tables' in line or 'tekken6-nx: hook' in line:
                continue
            kat = m.group(2)
            if kat not in mapping:
                unmapped.append(f'{os.path.basename(path)}:{i + 1}: sub_{kat}')
                continue
            tek = mapping[kat]
            if tek:
                new = line.replace(f'(sub_{kat}', f'(sub_{tek}', 1)
                note = f'tekken6-nx: hook at Tekken sub_{tek} (katamari sub_{kat})'
                moved += 1
            else:
                new = line.replace(f'BANJO_RENUT_{m.group(1)}(', f'BANJO_RENUT_UNHOOKED_{m.group(1)}(', 1)
                note = f'tekken6-nx: not in Tekken, unhooked (katamari sub_{kat})'
                unhooked += 1
            lines[i] = new + ('  // ' if '//' not in new else ' (') + note + ('' if '//' not in new else ')')
        if not a.dry_run:
            open(path, 'w', encoding='utf-8', newline='').write(nl.join(lines))
    print(f'{moved} hooks moved, {unhooked} unhooked, {len(unmapped)} still at katamari addresses')
    for u in unmapped:
        print('  ' + u)


if __name__ == '__main__':
    main()
