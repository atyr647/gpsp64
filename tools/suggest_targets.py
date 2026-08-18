#!/usr/bin/env python3
"""suggest_targets.py -- turn benchmark page profiles into AOT targets.

Reads the `--pages` dump written by native/native_main.c (one line per
executed 4KB page: address, total interpreted instructions, and how many
of those were Thumb) and prints candidate ranges for n64/aot_targets.txt.

Why the Thumb column matters: tools/thumb2c.py only translates Thumb.
A hot page that is mostly ARM cannot be helped by adding it as an AOT
target, so pages below --min-thumb are reported as skipped rather than
silently suggested.

Usage:
    tools/suggest_targets.py bench-results/baseline.pages
    tools/suggest_targets.py bench-results/baseline.pages --top 8 >> n64/aot_targets.txt
"""
import argparse
import sys


def read_pages(path):
    rows = []
    with open(path) as f:
        for line in f:
            line = line.split('#', 1)[0].strip()
            if not line:
                continue
            parts = line.split()
            if len(parts) < 3:
                continue
            rows.append((int(parts[0], 0), int(parts[1]), int(parts[2])))
    return rows


def read_existing(path):
    """Existing (start, end) ranges from an aot_targets.txt, if present."""
    ranges = []
    try:
        with open(path) as f:
            for line in f:
                line = line.split('#', 1)[0].strip()
                if not line:
                    continue
                parts = line.split()
                if len(parts) >= 2:
                    ranges.append((int(parts[0], 0), int(parts[1], 0)))
    except FileNotFoundError:
        pass
    return ranges


def covered(page, ranges):
    return any(start <= page < end for start, end in ranges)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('pages', help='page dump from --pages')
    ap.add_argument('--top', type=int, default=10,
                    help='max number of pages to suggest (default 10)')
    ap.add_argument('--min-thumb', type=float, default=60.0,
                    help='minimum %% Thumb for a page to be AOT-able (default 60)')
    ap.add_argument('--min-share', type=float, default=0.5,
                    help='minimum %% of total interpreted insns (default 0.5)')
    ap.add_argument('--targets', default='n64/aot_targets.txt',
                    help='existing targets file, to avoid re-suggesting')
    args = ap.parse_args()

    rows = read_pages(args.pages)
    if not rows:
        print(f'no page data in {args.pages}', file=sys.stderr)
        return 1
    existing = read_existing(args.targets)
    grand = sum(r[1] for r in rows) or 1

    suggested, skipped = [], []
    for page, total, thumb in rows:
        share = 100.0 * total / grand
        tpct = 100.0 * thumb / total if total else 0.0
        if share < args.min_share:
            continue
        if covered(page, existing):
            skipped.append((page, share, tpct, 'already covered'))
        elif tpct < args.min_thumb:
            skipped.append((page, share, tpct, 'mostly ARM - thumb2c cannot translate'))
        else:
            suggested.append((page, share, tpct))

    for page, share, tpct, why in skipped:
        print(f'# skip 0x{page:08X}  {share:5.1f}% of interp  Thumb {tpct:3.0f}%  ({why})')
    if skipped and suggested:
        print('#')

    total_share = 0.0
    for page, share, tpct in suggested[:args.top]:
        total_share += share
        print(f'0x{page:08X}  0x{page + 0x1000:08X}'
              f'        # {share:5.1f}% of interpreted insns, Thumb {tpct:3.0f}%')
    if suggested:
        print(f'# ^ {min(len(suggested), args.top)} page(s), '
              f'{total_share:.1f}% of interpreted instructions', file=sys.stderr)
    else:
        print('# no AOT-able pages met the thresholds', file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main())
