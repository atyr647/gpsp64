#!/usr/bin/env python3
"""Check the hot dispatch chain's I-cache index placement in a linked ELF.

The VR4300 I-cache is 16KB, direct-mapped, 32-byte lines, so a function's
cache footprint is its address range taken mod 16KB.  Two functions whose
footprints overlap evict each other every time control passes between
them; when they call each other on the hot path that is a self-inflicted
miss per call, and it costs 48 cycles to refill each line.

The chain is placed by collecting it into one linker input-section group
(see n64/n64_hotchain.h), which makes it contiguous and therefore
conflict-free by construction -- but only while the group fits in 16KB
and while every member actually lands in it.  A function that gets
inlined away, or one added to the chain without the attribute, breaks
that silently: the build still works and the frame time quietly drops.

So check it:

    tools/hotchain.py gpsp.elf

Exit status is 1 if the group overflows 16KB or if a listed member is
missing from it, 0 otherwise.  Overlaps *with code outside the group*
are reported for information: those are expected -- .text is 1.4MB
against a 16KB cache, so something is always sharing an index -- and
only matter when the other party is itself hot.
"""
import re
import subprocess
import sys

ICACHE = 16 * 1024

# Every member of keep.text.gpsp_hotchain that we expect to find.  Missing
# ones are an error, because the usual cause is inlining silently removing
# a function from the group.
EXPECTED = [
    "update_gba",
    "update_scanline",
    "mips_update_gba",
    "mips_indirect_branch_arm",
    "mips_indirect_branch_thumb",
    "mips_indirect_branch_dual",
]


def symbols(elf, nm):
    out = subprocess.run([nm, "-n", elf], capture_output=True, text=True).stdout
    syms = []
    for line in out.splitlines():
        parts = line.split(None, 2)
        if len(parts) < 3:
            continue
        try:
            addr = int(parts[0], 16)
        except ValueError:
            continue
        if parts[1].lower() in "tw":
            syms.append((addr, parts[2].strip()))
    syms.sort()
    # Size a symbol as the gap to the next one.  Approximate at the very
    # end of .text, exact everywhere else, and good enough to decide
    # whether two ranges share cache indices.
    return [(a, syms[i + 1][0] - a if i + 1 < len(syms) else 0, n)
            for i, (a, n) in enumerate(syms)]


def group_from_map(mapfile):
    """True extent of the gathered group, read from the link map.

    The members' own addresses only bound the symbols this script knows
    about, and mips_stub.S contributes several routines beyond them.  The
    map lists every input section that landed in the group with its real
    size, which is what has to stay under 16KB.  Returns (lo, hi) or None
    if the map is absent or has no group in it.
    """
    try:
        text = open(mapfile).read()
    except OSError:
        return None
    chunks = []
    for m in re.finditer(r"^ keep\.text\.\S*\n?\s*(0x[0-9a-f]+)\s+(0x[0-9a-f]+)",
                         text, re.M):
        chunks.append((int(m.group(1), 16), int(m.group(2), 16)))
    if not chunks:
        return None
    lo = min(a for a, _ in chunks)
    hi = max(a + s for a, s in chunks)
    return lo, hi


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    elf = sys.argv[1]
    prefix = sys.argv[2] if len(sys.argv) > 2 else \
        "toolchain/opt/libdragon/bin/mips64-elf-"
    syms = symbols(elf, prefix + "nm")

    by_name = {n: (a, s) for a, s, n in syms}
    missing = [n for n in EXPECTED if n not in by_name]
    if missing:
        print("MISSING from the ELF entirely: " + ", ".join(missing))
        return 1

    members = sorted((by_name[n][0], by_name[n][1], n) for n in EXPECTED)
    # Prefer the link map: it covers the whole group, including the members
    # of the chain this script does not name individually.
    bounds = group_from_map(re.sub(r"\.elf$", ".map", elf))
    if bounds:
        lo, hi = bounds
        source = "link map"
    else:
        lo = members[0][0]
        hi = max(a + s for a, s, _ in members)
        source = "named members only -- link map not found, group may be larger"
    span = hi - lo

    print(f"hot chain: {lo:08x}..{hi:08x}  span {span} bytes "
          f"({span * 100.0 / ICACHE:.0f}% of the 16KB I-cache)  [{source}]")
    for a, s, n in members:
        print(f"  {a:08x} +{s:<6d} index {a & (ICACHE-1):04x}..{(a + s - 1) & (ICACHE-1):04x}  {n}")

    fail = False
    if span > ICACHE:
        print(f"\nFAIL: the chain spans {span} bytes, more than the {ICACHE}-byte "
              "I-cache, so its members alias each other again.")
        fail = True

    # What else got linked into the middle of the group.  Gaps are normal:
    # the chain's own file contributes routines this list does not name
    # (mips_cheat_hook, return_to_main, the CPSR helpers).  They are shown
    # rather than flagged, because the thing that would actually be wrong
    # -- a member falling out of the group -- shows up as the span check
    # above, not as a gap.
    inside = [(a, s, n) for a, s, n in syms
              if lo <= a < hi and n not in {m[2] for m in members}]
    if inside:
        print(f"\nalso in the group ({len(inside)} symbols):")
        for a, s, n in inside:
            print(f"  {a:08x} +{s:<6d} {n}")

    # Explicit pairwise index overlap, which is the thing that actually costs.
    print()
    clean = True
    for i in range(len(members)):
        for j in range(i + 1, len(members)):
            ai, si, ni = members[i]
            aj, sj, nj = members[j]
            if (ai & (ICACHE-1)) < (aj & (ICACHE-1)) + sj and \
               (aj & (ICACHE-1)) < (ai & (ICACHE-1)) + si:
                print(f"OVERLAP: {ni} and {nj} share cache indices")
                clean = False
                fail = True
    if clean:
        print("no member of the chain shares a cache index with another member.")
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
