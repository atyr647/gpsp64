#!/usr/bin/env python3
"""pcprof.py -- attribute sampled VR4300 PCs to functions.

Reads the ELF symbol table (via mips64-elf-nm) and a list of sampled
program counters, and prints the functions the CPU was actually in.

Every other profile in this project is derived -- a counter here, a timer
there, a residual by subtraction -- and those stop being trustworthy the
moment two of them disagree.  This one asks the emulator where the CPU is
and counts.
"""
import bisect
import os
import subprocess
import sys


def symbols(elf):
    nm = os.environ.get('NM', 'mips64-elf-nm')
    for cand in (nm, os.path.join(os.path.dirname(__file__), '..',
                                  'toolchain/opt/libdragon/bin/mips64-elf-nm')):
        try:
            out = subprocess.run([cand, '-C', '--defined-only', '-S', elf],
                                 capture_output=True, text=True, check=True).stdout
            break
        except (FileNotFoundError, subprocess.CalledProcessError):
            continue
    else:
        sys.exit('no mips64-elf-nm found')

    syms = []
    for line in out.splitlines():
        parts = line.split(None, 4)
        if len(parts) < 4:
            continue
        try:
            addr = int(parts[0], 16)
        except ValueError:
            continue
        if len(parts) >= 4 and parts[2].lower() in 'tw':
            size = int(parts[1], 16) if len(parts[1]) <= 16 else 0
            syms.append((addr, size, parts[3]))
        elif parts[1].lower() in 'tw':
            syms.append((addr, 0, parts[2]))
    syms.sort()
    return syms


def main():
    if len(sys.argv) < 3:
        sys.exit('usage: pcprof.py <elf> <pcs.txt>')
    syms = symbols(sys.argv[1])
    starts = [s[0] for s in syms]

    counts = {}
    total = 0
    with open(sys.argv[2]) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            pc = int(line, 16)
            # PCs come out of ares in KSEG0/KSEG1; symbols are KSEG0.
            pc = (pc & 0x1FFFFFFF) | 0x80000000
            total += 1
            i = bisect.bisect_right(starts, pc) - 1
            if i < 0:
                counts['<below .text>'] = counts.get('<below .text>', 0) + 1
                continue
            addr, size, name = syms[i]
            if size and pc >= addr + size:
                name = '<gap after %s>' % name
            counts[name] = counts.get(name, 0) + 1

    if not total:
        sys.exit('no samples')
    print('%d samples' % total)
    print('%6s  %8s  %s' % ('share', 'samples', 'function'))
    for name, c in sorted(counts.items(), key=lambda kv: -kv[1])[:30]:
        print('%5.1f%%  %8d  %s' % (c * 100.0 / total, c, name))


if __name__ == '__main__':
    main()
