#!/usr/bin/env python3
"""jitdis.py -- disassemble and analyse the dynarec's own output.

Consumes the JITDUMP lines a `-DN64_JIT_DUMP=<n>` build writes to stderr
(see n64_jit_dump_block in cpu_threaded.c) and turns them into readable
MIPS plus aggregate statistics about what the emitter actually produces.

    tools/jitdis.py <ares.log> [--dis N]   # --dis: disassemble first N blocks

The point is to rank patterns by how often they are *emitted*, which is a
proxy for how often they run only insofar as blocks are entered equally
often -- they are not.  Treat the counts as "what the code generator
spends its output on", not as a time profile; this project has already
learned the hard way (see docs/DYNAREC.md) that instruction counts and
cycles disagree.
"""
import collections
import subprocess
import sys
import tempfile
import os

OBJDUMP = "/home/user/gpsp64/toolchain/opt/libdragon/bin/mips64-elf-objdump"

# Register names as the emitter assigns them (mips/mips_emit.h).
ROLE = {
    0: "zero", 1: "temp/at", 2: "rv/v0", 3: "Ar0", 4: "a0", 5: "a1", 6: "a2",
    7: "Ar1", 8: "Ar2", 9: "Ar3", 10: "Ar4", 11: "Ar5", 12: "Ar6", 13: "Ar7",
    14: "Ar8", 15: "Ar9", 16: "base", 17: "cycles", 18: "Ar10", 19: "pc",
    20: "Nflag", 21: "Zflag", 22: "Cflag", 23: "Vflag", 24: "Ar11", 25: "Ar12",
    26: "k0", 27: "k1", 28: "Ar13", 29: "sp", 30: "Ar14", 31: "ra",
}


def parse(path):
    blocks, cur = [], None
    with open(path, errors="replace") as f:
        for line in f:
            if not line.startswith("JITDUMP"):
                continue
            parts = line.split()
            if len(parts) >= 5 and parts[1] == "BEGIN":
                cur = {"type": parts[2], "pc": int(parts[3], 16),
                       "nwords": int(parts[4]), "words": []}
            elif len(parts) >= 2 and parts[1] == "END":
                if cur:
                    blocks.append(cur)
                cur = None
            elif cur is not None:
                for w in parts[1:]:
                    try:
                        cur["words"].append(int(w, 16))
                    except ValueError:
                        pass
    return [b for b in blocks if b["words"]]


def dis(words):
    """Disassemble a word list via objdump on a raw binary."""
    data = b"".join(w.to_bytes(4, "big") for w in words)
    fd, tmp = tempfile.mkstemp(suffix=".bin")
    try:
        os.write(fd, data)
        os.close(fd)
        out = subprocess.run(
            [OBJDUMP, "-D", "-b", "binary", "-m", "mips:4300", "-EB", tmp],
            capture_output=True, text=True).stdout
    finally:
        os.unlink(tmp)
    lines = []
    for ln in out.splitlines():
        s = ln.strip()
        if not s or ":" not in s:
            continue
        head = s.split(":")[0]
        try:
            int(head, 16)
        except ValueError:
            continue
        lines.append(s)
    return lines


def classify(w):
    """Bucket one emitted word into an emitter-meaningful category."""
    if w == 0:
        return "nop"
    op = w >> 26
    rs, rt, rd = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
    fn = w & 0x3F
    imm = w & 0xFFFF
    if op == 0:
        if fn == 0x21:                       # addu
            if rt == 0:
                return "move (addu rd,rs,zero)"
            if rs == 0:
                return "move (addu rd,zero,rt)"
            return "addu"
        if fn == 0x08:
            return "jr"
        if fn == 0x09:
            return "jalr"
        return {0x00: "sll", 0x02: "srl", 0x03: "sra", 0x04: "sllv",
                0x06: "srlv", 0x07: "srav", 0x23: "subu", 0x24: "and",
                0x25: "or", 0x26: "xor", 0x27: "nor", 0x2A: "slt",
                0x2B: "sltu"}.get(fn, "special/other")
    if op == 0x09:                           # addiu
        if rs == 17 and rt == 17:
            return "cycle update (addiu cycles)"
        if rs == 19:
            return "pc materialise (addiu rt,pc,imm)"
        if rs == 0:
            return "load imm (addiu rt,zero)"
        return "addiu"
    if op == 0x02:
        return "j"
    if op == 0x03:
        return "jal"
    if op in (0x04, 0x05, 0x06, 0x07):
        return "branch (beq/bne/blez/bgtz)"
    if op == 0x01:
        return "branch (bltz/bgez/bltzal)"
    if op == 0x0F:
        return "lui"
    if op in (0x0C, 0x0D, 0x0E):
        return {0x0C: "andi", 0x0D: "ori", 0x0E: "xori"}[op]
    if op in (0x0A, 0x0B):
        return "slti/sltiu"
    if op in (0x20, 0x21, 0x23, 0x24, 0x25):
        return "load (lb/lh/lw/lbu/lhu)"
    if op in (0x28, 0x29, 0x2B):
        return "store (sb/sh/sw)"
    return "other"


def main():
    path = sys.argv[1]
    ndis = 0
    if "--dis" in sys.argv:
        ndis = int(sys.argv[sys.argv.index("--dis") + 1])

    blocks = parse(path)
    if not blocks:
        print("no JITDUMP blocks found")
        return

    total = sum(len(b["words"]) for b in blocks)
    cat = collections.Counter()
    for b in blocks:
        for w in b["words"]:
            cat[classify(w)] += 1

    print(f"blocks dumped : {len(blocks)}")
    print(f"MIPS words    : {total}")
    print(f"mean per block: {total/len(blocks):.1f}")
    print()
    print(f"{'category':34s} {'count':>7s}  {'share':>6s}")
    print("-" * 52)
    for k, v in cat.most_common():
        print(f"{k:34s} {v:7d}  {100.0*v/total:5.1f}%")

    # Delay-slot waste: a nop immediately after a jump/branch is a slot the
    # emitter could not fill.  Count those separately from nops elsewhere.
    ds_nop = other_nop = 0
    for b in blocks:
        ws = b["words"]
        for i, w in enumerate(ws):
            if w != 0:
                continue
            if i > 0:
                p = ws[i - 1]
                pop = p >> 26
                pfn = p & 0x3F
                isjump = pop in (0x02, 0x03, 0x01, 0x04, 0x05, 0x06, 0x07) or \
                         (pop == 0 and pfn in (0x08, 0x09))
                if isjump:
                    ds_nop += 1
                    continue
            other_nop += 1
    print()
    print(f"nops in a delay slot (unfilled) : {ds_nop} "
          f"({100.0*ds_nop/total:.1f}% of all emitted words)")
    print(f"nops elsewhere                  : {other_nop}")

    # Where do the delay-slot nops come from?  Bucket by the jump that owns
    # the slot -- that names the emitter path that failed to fill it.
    ds_by = collections.Counter()
    for b in blocks:
        ws = b["words"]
        for i, w in enumerate(ws):
            if w != 0 or i == 0:
                continue
            p = ws[i - 1]
            pop, pfn = p >> 26, p & 0x3F
            if pop == 0x03:
                ds_by["after jal (helper/stub call)"] += 1
            elif pop == 0x02:
                ds_by["after j (block link / indirect)"] += 1
            elif pop == 0x01:
                ds_by["after bltz/bgez/bltzal"] += 1
            elif pop in (0x04, 0x05, 0x06, 0x07):
                ds_by["after beq/bne/blez/bgtz"] += 1
            elif pop == 0 and pfn in (0x08, 0x09):
                ds_by["after jr/jalr"] += 1
    print()
    print("unfilled delay slots, by owning jump:")
    for k, v in ds_by.most_common():
        print(f"  {k:36s} {v:5d}  ({100.0*v/total:4.1f}% of all words)")

    # Split the biggest category -- register moves -- by what role the move
    # actually plays, since only some of them are structurally required.
    mv = collections.Counter()
    for b in blocks:
        for w in b["words"]:
            if (w >> 26) != 0 or (w & 0x3F) != 0x21:
                continue
            rs, rt, rd = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
            if rt != 0:
                continue
            if rd == rs:
                mv["rd == rs (pure no-op move)"] += 1
            elif rd in (4, 5, 6):
                mv["into a0/a1/a2 (shared-stub argument)"] += 1
            elif rs == 2:
                mv["from v0 (stub return value)"] += 1
            elif rd == 1 or rs == 1:
                mv["via at (scratch staging)"] += 1
            else:
                mv["ARM reg -> ARM reg"] += 1
    print()
    print("register moves (addu rd,rs,zero), by role:")
    for k, v in mv.most_common():
        print(f"  {k:36s} {v:5d}  ({100.0*v/total:4.1f}% of all words)")

    print()
    print("block sizes (words):",
          ", ".join(str(len(b["words"])) for b in blocks[:24]),
          "..." if len(blocks) > 24 else "")

    for b in blocks[:ndis]:
        print()
        print(f"=== {b['type']} block at GBA pc {b['pc']:08x} "
              f"({len(b['words'])} words) ===")
        for ln in dis(b["words"]):
            print("  " + ln)


if __name__ == "__main__":
    main()
