#!/usr/bin/env python3
"""tekken6-nx (2026-11-05): puts Renut's Direct3D device offsets back to N&B title update 3's layout.

katamari-nx rewrote them for XDK 2.0.5632 (tools/renut_device_offsets_katamari.py in katamari-nx), tagging every line
`katamari-nx: <N&B>-><Katamari>, ...`. Tekken 6's XDK 2.0.8276 device uses N&B's layout (docs/tekken6-notes.md,
"Renut's device layout"): each tagged device offset goes back to the N&B value. Guest data addresses (0x82xxxxxx) are
not touched here: they need Tekken's own addresses, and are listed instead. Lines already handled carry `tekken6-nx`.

  python tools/renut_device_offsets_tekken.py [--dry-run]
"""
import argparse
import glob
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GPU = os.path.join(ROOT, 'app', 'src', 'renut_engine', 'gpu')
TAG = re.compile(r'katamari-nx: ((?:(?:0x[0-9A-Fa-f]+|\d+)->(?:0x[0-9A-Fa-f]+|\d+)(?:, )?)+)')
PAIR = re.compile(r'(0x[0-9A-Fa-f]+|\d+)->(0x[0-9A-Fa-f]+|\d+)')
NUM = r'(?<![0-9A-Za-z_.]){}(?![0-9A-Za-z_.])'


def is_data(s):
    return s.lower().startswith('0x8') and len(s) == 10


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dry-run', action='store_true')
    a = ap.parse_args()
    changed_lines, data = 0, []
    for path in sorted(glob.glob(os.path.join(GPU, '**', '*.*'), recursive=True)):
        raw = open(path, encoding='utf-8', newline='').read()
        nl = '\r\n' if '\r\n' in raw else '\n'
        lines = raw.split(nl)
        changed = False
        for i, line in enumerate(lines):
            m = TAG.search(line)
            if not m or 'tekken6-nx' in line:
                continue
            cut = line.find('//')
            head, tail = (line, '') if cut < 0 else (line[:cut], line[cut:])
            new_head, undone = head, []
            for nb, kat in PAIR.findall(m.group(1)):
                if is_data(nb):
                    data.append(f'{os.path.relpath(path, GPU)}:{i + 1}: {nb}->{kat}')
                    continue
                pattern = re.compile(NUM.format(re.escape(kat)))
                if pattern.search(new_head):
                    new_head = pattern.sub(nb, new_head, count=1)
                    undone.append(f'{kat}->{nb}')
            if undone:
                lines[i] = new_head + tail + ' (tekken6-nx: N&B layout, ' + ', '.join(undone) + ')'
                changed = True
                changed_lines += 1
        if changed and not a.dry_run:
            open(path, 'w', encoding='utf-8', newline='').write(nl.join(lines))
    print(f'{changed_lines} lines back to N&B offsets; {len(data)} guest data sites still at Katamari addresses:')
    for d in data:
        print('  ' + d)


if __name__ == '__main__':
    main()
