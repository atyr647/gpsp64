#!/usr/bin/env python3
"""pccyc.py -- attribute time-weighted VR4300 samples to code regions.

Reads `PCCYC <pc> <count>` lines (ares, GPSP_PCCYC=<cycles per sample>)
plus the ROM's ELF, and reports where emulated *time* goes.

Two things this does that tools/pcprof.py does not.

Weighting.  pcprof.py consumes a sampler that fires every N executed
instructions.  On a VR4300 that is the wrong axis: a PC that stalls forty
cycles on a D-cache miss counts exactly the same as one that retires in
one, so an instruction-weighted profile hides the stalls that dominate a
frame.  This one consumes a sampler driven from CPU::step(), which is
handed the real cycle cost of everything.

Regions.  Most of the interesting code is not in the symbol table at all:
the dynarec's output lives in rom_translation_cache / ram_translation_cache
and the hand-written stubs sit below the watermark inside them, so nm
attributes every sample there to whatever BSS object happens to precede it
-- which is how `ewram_raw +0x70d6c` ended up in a debugging session as if
it meant something.  Samples inside those objects are labelled by region
instead, and the stub area is split out from translated code.
"""
import bisect
import os
import subprocess
import sys

# Objects that are code at runtime but data to the symbol table.  Order
# matters only for readability; lookup is by containment.
CODE_OBJECTS = (
    'rom_translation_cache',
    'ram_translation_cache',
    'ibcache_arm', 'ibcache_thumb', 'ibcache_dual',
)


def nm(elf):
    cands = [os.environ.get('NM', 'mips64-elf-nm'),
             os.path.join(os.path.dirname(__file__), '..',
                          'toolchain/opt/libdragon/bin/mips64-elf-nm')]
    for c in cands:
        try:
            return subprocess.run([c, '-C', '--defined-only', '-S', elf],
                                  capture_output=True, text=True,
                                  check=True).stdout
        except (FileNotFoundError, subprocess.CalledProcessError):
            continue
    sys.exit('no mips64-elf-nm found')


def symbols(elf):
    """Return (text_syms, objects) where objects maps name -> (addr, size)."""
    text, objs = [], {}
    for line in nm(elf).splitlines():
        p = line.split(None, 4)
        if len(p) < 3:
            continue
        try:
            addr = int(p[0], 16)
        except ValueError:
            continue
        if len(p) >= 4 and len(p[1]) <= 16 and all(c in '0123456789abcdefABCDEF' for c in p[1]):
            size, kind, name = int(p[1], 16), p[2], p[3]
        else:
            size, kind, name = 0, p[1], p[2]
        if kind.lower() in 'tw':
            text.append((addr, size, name))
        elif kind.lower() in 'bd':
            objs[name] = (addr, size)
    text.sort()
    return text, objs


def main():
    if len(sys.argv) < 3:
        sys.exit('usage: pccyc.py <elf> <log-with-PCCYC-lines> '
                 '[stub-watermark-bytes]')
    text, objs = symbols(sys.argv[1])
    starts = [s[0] for s in text]

    # The stub area is emitted at the bottom of rom_translation_cache before
    # any block is translated; everything above the watermark is generated
    # code.  Pass the watermark (the JITSTUB line prints it) to separate
    # them, otherwise they are reported together.
    watermark = int(sys.argv[3]) if len(sys.argv) > 3 else None

    # These objects are declared with .space in a nobits section and carry
    # no ELF size, so nm reports them at zero bytes.  Take each one's extent
    # as running to the next symbol -- which is why every sample inside them
    # otherwise lands in whatever symbol happens to precede them, and how
    # 21.8% of a profile first showed up filed under `__text_end`.
    alladdr = sorted(set([a for a, _, _ in text] + [a for a, _ in objs.values()]))
    def extent(name):
        a = objs[name][0]
        i = bisect.bisect_right(alladdr, a)
        end = alladdr[i] if i < len(alladdr) else 0x80800000
        return a, end - a

    regions = []
    for name in CODE_OBJECTS:
        if name in objs:
            a, sz = extent(name)
            if name == 'rom_translation_cache' and watermark:
                regions.append((a, a + watermark, 'dynarec stubs (mips_stub.S)'))
                regions.append((a + watermark, a + sz,
                                'translated code (ROM blocks)'))
            else:
                label = {'rom_translation_cache': 'translated code (ROM blocks)',
                         'ram_translation_cache': 'translated code (RAM blocks)'}.get(
                            name, 'inline cache: ' + name)
                regions.append((a, a + sz, label))

    # -DN64_STUBMAP makes the ROM print the address of every generated
    # memory handler.  With it, a sample in the stub area is attributed to
    # the operation and region that own it rather than to "the stubs".
    stubmap = []
    for line in open(sys.argv[2], errors='ignore'):
        if line.startswith('STUBMAP '):
            f = line.split()
            if len(f) >= 3:
                stubmap.append(((int(f[1], 16) & 0x1FFFFFFF) | 0x80000000, f[2]))
    stubmap.sort()
    stubstarts = [a for a, _ in stubmap]

    counts, total = {}, 0
    for line in open(sys.argv[2], errors='ignore'):
        if not line.startswith('PCCYC '):
            continue
        f = line.split()
        if len(f) < 3:
            continue
        try:
            pc, n = int(f[1], 16), int(f[2])
        except ValueError:
            continue
        pc = (pc & 0x1FFFFFFF) | 0x80000000   # KSEG0/1 -> KSEG0
        total += n
        for lo, hi, label in regions:
            if lo <= pc < hi:
                if label.startswith('dynarec stubs') and stubmap:
                    j = bisect.bisect_right(stubstarts, pc) - 1
                    if j >= 0:
                        label = 'stub ' + stubmap[j][1]
                counts[label] = counts.get(label, 0) + n
                break
        else:
            i = bisect.bisect_right(starts, pc) - 1
            if i < 0:
                counts['<below .text>'] = counts.get('<below .text>', 0) + n
                continue
            addr, size, name = text[i]
            if size and pc >= addr + size:
                name = '<gap after %s>' % name
            counts[name] = counts.get(name, 0) + n

    if not total:
        sys.exit('no PCCYC samples found -- run ares with GPSP_PCCYC=<n>')
    print('%d weighted samples\n' % total)
    print('%6s  %10s  %s' % ('share', 'samples', 'where'))
    for name, c in sorted(counts.items(), key=lambda kv: -kv[1])[:35]:
        print('%5.1f%%  %10d  %s' % (c * 100.0 / total, c, name))


if __name__ == '__main__':
    main()
