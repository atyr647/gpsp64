#!/usr/bin/env python3
"""parse_prof.py -- summarise the ROM's PROF output from an ares run.

PROF lines are emitted every 60 emulated frames and derive from the
VR4300 COUNT register, so `ms/f` is genuine N64 frame time.

Emulation does a fixed amount of work per frame regardless of host speed,
so window N holds the same game state in every build; comparing
like-indexed windows across builds is a fair A/B.  --skip drops the boot
windows (BIOS decompression, intro) which are not representative.
"""
import argparse
import re
import statistics
import sys

# PROF: CPU91% PPU9% Blt4% 101ms/f | 4383K insns 723KIPS ~130 cyc/i | ARM99%/Thm0% idle 0 rt 0
# PROF:  aot-pages: 0x82e1000:15% 0x806f000:14% ...
PAGES_RE = re.compile(r'^PROF:\s+aot-pages:\s+(.*)$')
PAGE_ENTRY = re.compile(r'(0x[0-9a-fA-F]+):(\d+)%')

PROF_RE = re.compile(
    r'^PROF: CPU(?P<cpu>\d+)% PPU(?P<ppu>\d+)% Blt(?P<blt>\d+)% (?P<ms>\d+)ms/f'
    r' \| (?P<insns>\d+)K insns (?P<kips>\d+)KIPS ~(?P<cyc>\d+) cyc/i'
    r' \| ARM(?P<arm>\d+)%/Thm(?P<thm>\d+)% idle (?P<idle>\d+) rt (?P<rt>\d+)')


def parse(path):
    """Return (windows, page_samples).  page_samples[i] is the hot-page
    list reported for window i, as [(addr, pct), ...] measured on the N64
    itself -- the right basis for choosing new AOT targets."""
    rows, pages, pending = [], [], None
    with open(path, errors='replace') as f:
        for line in f:
            line = line.rstrip()
            m = PROF_RE.match(line.strip())
            if m:
                rows.append({k: int(v) for k, v in m.groupdict().items()})
                pending = len(rows) - 1
                continue
            mp = PAGES_RE.match(line)
            if mp and pending is not None:
                pages.append((pending,
                              [(a, int(p)) for a, p in PAGE_ENTRY.findall(mp.group(1))]))
                pending = None
    return rows, pages


def report_pages(pages, skip):
    """Aggregate hot pages over the steady-state windows."""
    agg = {}
    n = 0
    for idx, entries in pages:
        if idx < skip:
            continue
        n += 1
        for addr, pct in entries:
            agg[int(addr, 16)] = agg.get(int(addr, 16), 0) + pct
    if not agg or not n:
        return
    print('\n  hot pages on N64 (share of interpreted insns, averaged):')
    for addr, tot in sorted(agg.items(), key=lambda kv: -kv[1])[:10]:
        if tot / n < 0.5:
            continue
        print(f'    0x{addr:08X}  {tot / n:5.1f}%')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log')
    ap.add_argument('--label', default='run')
    ap.add_argument('--skip', type=int, default=8,
                    help='boot windows to discard (default 8)')
    ap.add_argument('--text', type=int, default=0)
    ap.add_argument('--timeline', action='store_true',
                    help='print every window, to locate phase boundaries')
    ap.add_argument('--range', default=None,
                    help='measure only windows A:B (0-based, python slice)')
    args = ap.parse_args()

    rows, pages = parse(args.log)
    if not rows:
        print(f'{args.label}: no PROF output found in {args.log}', file=sys.stderr)
        return 1

    if args.timeline:
        print(f'{"win":>4} {"ms/f":>6} {"FPS":>6} {"CPU":>4} {"PPU":>4} '
              f'{"Blt":>4} {"cyc/i":>6} {"insnsK":>7} {"ARM":>4} {"Thm":>4} {"idle":>7}')
        for i, r in enumerate(rows):
            fps_i = 1000.0 / r['ms'] if r['ms'] else 0
            print(f'{i:>4} {r["ms"]:>6} {fps_i:>6.1f} {r["cpu"]:>4} {r["ppu"]:>4} '
                  f'{r["blt"]:>4} {r["cyc"]:>6} {r["insns"]:>7} {r["arm"]:>4} '
                  f'{r["thm"]:>4} {r["idle"]:>7}')
        print()

    if args.range:
        a, _, b = args.range.partition(':')
        steady = rows[int(a) if a else None:int(b) if b else None]
    else:
        steady = rows[args.skip:] or rows
    def avg(k): return statistics.mean(r[k] for r in steady)
    def med(k): return statistics.median(r[k] for r in steady)

    ms = med('ms')
    fps = 1000.0 / ms if ms else 0.0

    print(f'\n=== ares: {args.label} ===')
    print(f'  windows            {len(rows)} captured, {len(steady)} steady '
          f'(first {args.skip} discarded as boot)')
    print(f'  frame time         {ms:.1f} ms/f  (median)  ->  {fps:.1f} FPS')
    print(f'  frame time         {avg("ms"):.1f} ms/f  (mean)')
    print(f'  time split         CPU {avg("cpu"):.0f}%  PPU {avg("ppu"):.0f}%  Blt {avg("blt"):.0f}%')
    print(f'  interpreter cost   {avg("cyc"):.0f} VR4300 cycles per GBA instruction')
    print(f'  emulated work      {avg("insns"):.0f}K insns/window  ({avg("kips"):.0f} KIPS)')
    print(f'  instruction mix    ARM {avg("arm"):.0f}%  Thumb {avg("thm"):.0f}%')
    print(f'  idle-loop skips    {avg("idle"):.0f}/window   runtime-detected {avg("rt"):.0f}')
    if args.text:
        print(f'  .text              {args.text} bytes')
    report_pages(pages, args.skip)
    print(f'\nARESSUM {args.label} fps={fps:.2f} ms={ms:.1f} cpu={avg("cpu"):.0f} '
          f'ppu={avg("ppu"):.0f} blt={avg("blt"):.0f} cyc_per_insn={avg("cyc"):.0f} '
          f'insns_k={avg("insns"):.0f} text={args.text}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
