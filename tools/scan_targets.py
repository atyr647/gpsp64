#!/usr/bin/env python3
"""scan_targets.py — read aot_targets.txt and emit Thumb-function PCs.

Reads a file of "start_hex end_hex" ranges (one per line, # for comments),
scans the ROM for PUSH {..., lr} prologues in each range, and writes the
discovered PCs to stdout (one hex PC per line).

Used by the Makefile to feed thumb2c.py without bash-quoting headaches.
"""
import sys
import os

# Allow importing find_thumb_funcs from the same dir.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from find_thumb_funcs import scan, filter_aligned


def parse_targets(path):
    """Parse aot_targets.txt. Yields (start, end) integer pairs."""
    with open(path) as f:
        for raw in f:
            line = raw.split('#', 1)[0].strip()
            if not line:
                continue
            parts = line.split()
            if len(parts) < 2:
                print(f'warning: bad target line: {raw!r}', file=sys.stderr)
                continue
            yield int(parts[0], 0), int(parts[1], 0)


def main():
    if len(sys.argv) != 3:
        print(f'usage: {sys.argv[0]} ROM TARGETS_FILE', file=sys.stderr)
        sys.exit(1)
    rom_path, targets_path = sys.argv[1], sys.argv[2]
    with open(rom_path, 'rb') as f:
        rom = f.read()
    seen = set()
    for start, end in parse_targets(targets_path):
        for pc in scan(rom, start, end):
            if pc in seen:
                continue
            seen.add(pc)
            print(f'0x{pc:08X}')


if __name__ == '__main__':
    main()
