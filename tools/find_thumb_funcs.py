#!/usr/bin/env python3
"""find_thumb_funcs.py — scan a GBA ROM for Thumb function prologues.

A Thumb function typically starts with PUSH {..., lr} which encodes
as 0xB5xx (the low byte's high bit = 1 means LR is in the reg list).
The most common forms:
  B500   push {lr}
  B5xx   push {regs, lr}     (xx = bitmap of r0..r7)
  B5F0   push {r4-r7, lr}    (very common — saves callee-saved + lr)

Some functions also start with PUSH {regs} (no lr), but those usually
correspond to inline-helper-style code that returns via BX or BL.
We focus on PUSH {..., lr} for high precision.

Usage:
  tools/find_thumb_funcs.py ROM.gba [--start 0x08006000] [--end 0x08007000]
                                    [--filter-tail-call] [--max N]

Prints a sorted list of candidate function entry PCs (one per line).
"""
import sys
import argparse
import struct

ROM_BASE = 0x08000000


def is_push_lr(halfword):
    """Returns True if the 16-bit halfword decodes to PUSH {..., lr}."""
    # Thumb-1 PUSH encoding: 1011 010R RRRR RRRR
    #   top byte must be 0xB4 or 0xB5
    #   bit R (bit 8) = 1 means LR included
    return (halfword & 0xFF00) == 0xB500


def scan(rom, start, end):
    """Walk through [start, end) looking for halfwords that are PUSH {lr}.

    Returns a list of candidate entry PCs (sorted, deduped).
    Both 16-bit halfwords in each 32-bit word are checked, but only
    those at 2-byte alignment (which all Thumb insns are).
    """
    if start < ROM_BASE:
        start = ROM_BASE
    if end > ROM_BASE + len(rom):
        end = ROM_BASE + len(rom)
    candidates = []
    for pc in range(start, end, 2):
        off = pc - ROM_BASE
        hw = rom[off] | (rom[off + 1] << 8)
        if is_push_lr(hw):
            candidates.append(pc)
    return candidates


def filter_aligned(candidates, alignment=4):
    """Keep only candidates aligned to `alignment`. ARM ABI typically
    aligns Thumb function entries to 4 bytes (so the linker can use the
    low bit as the Thumb mode marker)."""
    return [pc for pc in candidates if pc % alignment == 0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('rom')
    ap.add_argument('--start', type=lambda x: int(x, 0), default=0x08000000)
    ap.add_argument('--end',   type=lambda x: int(x, 0), default=0x09000000)
    ap.add_argument('--align', type=int, default=2,
                    help='only keep PCs aligned to this many bytes (default 2)')
    ap.add_argument('--max', type=int, default=0,
                    help='cap output to N candidates (0 = unlimited)')
    args = ap.parse_args()

    with open(args.rom, 'rb') as f:
        rom = f.read()

    candidates = scan(rom, args.start, args.end)
    if args.align > 2:
        candidates = filter_aligned(candidates, args.align)
    if args.max:
        candidates = candidates[:args.max]

    for pc in candidates:
        print(f'0x{pc:08X}')


if __name__ == '__main__':
    main()
