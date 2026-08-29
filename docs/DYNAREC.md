# The dynarec: where it stands

> **Correction.** An earlier version of this document claimed the
> Makefile's "2.6x slower than the interpreter" figure was not a real
> measurement, on the evidence that the dynarec translated six blocks and
> hung. That was true only when starting from a **savestate**. From a
> cold boot the dynarec runs, translates 80+ blocks of real game code and
> completes frames, and its steady-state cost is **22.0 N64 cycles per
> GBA cycle** — which confirms the documented ~20.5 rather than refuting
> it. The hang is a separate, real bug in the savestate path. Both
> findings are kept below because the distinction is the whole point.

# Why it is slow, and what the numbers mean

`Makefile.n64` records the dynarec as **2.6× slower than the interpreter**
— 20.5 N64 cycles per GBA cycle against 7.9, with 5.56 being real time —
and that figure has been the reason not to invest in it. It is not a
measurement of the dynarec running the game.

## The numbers, measured from CP0 COUNT against cpu_ticks

Cold boot, per 50 `update_gba` calls, on the Emerald title/intro:

| | N64 cycles per GBA cycle |
| --- | --- |
| interpreter | 7.9 |
| dynarec, steady state | **22.0** |
| dynarec, intervals containing translation | 42 – 257 |
| real time | 5.56 |

So the dynarec is **2.8x slower than the interpreter** on game code, and
needs to get 4x faster to reach real time. The translation bursts are
the striking part: an interval that translates is 2 to 12 times more
expensive than one that does not, and the cost tracks the `icache calls`
counter exactly — every emit calls `inst_cache_invalidate_all()`, which
discards the whole 16 KB I-cache and makes everything afterwards run
cold.

## What it actually does from a savestate

Instrumenting block translation (`-DN64_JIT_TRACE`) shows the dynarec
translates exactly **six blocks** and then never translates another, for
as long as you leave it running:

```
JITX arm   rom 00000008   BIOS SWI vector
JITX arm   rom 00000064   BIOS
JITX thumb rom 080008ce   <- idle_loop_target_pc
JITX thumb rom 080008c6   <- idle loop body
JITX arm   rom 00000018   BIOS IRQ vector
JITX arm   rom 00000020   BIOS
```

Those are the BIOS vectors and **Emerald's idle loop — the exact PC the
interpreter special-cases to skip**. Not one line of game code is ever
translated.

## It is hung, not slow

The distinction matters and is easy to get wrong, because in ares the
build is also genuinely slow, so "it never finishes a frame" looks like
slowness. Three facts separate them:

- The guest freezes at a byte-identical state every run: `upd=400
  vcount=133 ticks=2193895120`. Tripling the wall clock (200 s → 620 s)
  produced more *emulated* VI frames (29 → 40) and moved the guest not at
  all.
- `frame_counter` never advances. `vcount` climbs to 133 and stops.
- Block lookups stop entirely — translation, hash-hit and hash-walk
  counters all freeze. Execution enters translated code and never returns
  to `mips_update_gba`.

That is an infinite loop inside a translated block with no cycle-counter
check: the block spins, `reg_cycles` never goes negative, so the exit
back to the event engine never fires.

## So the 2.6× figure measures a spin

Every performance number ever recorded for this dynarec — 20.5 N64 cycles
per GBA cycle, "1.9× worse with the memory patcher", "a frame takes
seconds" — was taken while it was stuck in a four-block idle loop. They
say nothing about how fast it executes game code, because it has never
executed any.

Measured properly, from CP0 `COUNT` against `cpu_ticks` over the healthy
phase before the hang (COUNT verified not to wrap: 0.48 s of emulated
time against a 91.6 s wrap period), the dynarec runs at **3.71 N64 cycles
per GBA cycle**. That is faster than the interpreter's 7.9 and faster
than the 5.56 needed for full speed — but it is measured on a tight
idle loop that fits entirely in cache, which is the best case
imaginable. Treat it as an upper bound, not a projection.

## One bug found and fixed on the way, insufficient by itself

`generate_branch_no_cycle_update` in `mips/mips_emit.h` emits the
idle-skip fast path when `pc == idle_loop_target_pc`. `pc` there is the
address of the *branch being translated*; `idle_loop_target_pc` is the
loop *head* — the interpreter tests `reg[REG_PC]` after an instruction
completes, which for a taken backward branch is the destination. So the
test only fires when a loop branches to itself. Emerald's idle loop spans
0x080008C6..0x080008CE and does not, so the fast path was never emitted.

Both operands are now tested. It did not fix the hang — the loop must
exit through a different emit path — but the comparison was wrong and is
now right.

## The savestate hang: what it is not

The hang blocks measuring the dynarec with this project's harness, which
boots from the overworld savestate. It has been narrowed but not fixed.
Eliminated, each by direct measurement:

| suspect | verdict |
| --- | --- |
| translation caches not flushed on load | `gba_load_state` calls `flush_dynarec_caches()` after restoring memory, and both flush functions reset `last_*_translation_ptr` correctly |
| RAM block tags left stale | `flush_dynarec_caches` sets `iwram_code_min/max` to the full 0..0x8000 before flushing, so the whole tag area is cleared |
| the m4a HLE's patched SWIs in IWRAM | identical hang with `N64_M4A=`: 6 blocks, frozen at `vcount=133` |
| a cyclic hash chain | chain-walk counter never exceeds 0 |
| the region switch falling through to its `(u8*)(~0)` sentinel | counter stays 0 |
| ares recompiler thrashing | 766 emits total, zero during a run, no allocator flushes |
| idle-skip testing the wrong operand | real bug, fixed, hang persists |

What is left, and where the next session should start: the last two
blocks translated before the hang are `arm rom 00000018` and `arm rom
00000020` — the BIOS **IRQ vector**. So the interrupt fires and is
translated, and the *next* block would be the game's IRQ handler in
IWRAM, which is the **RAM** translation path rather than the ROM one.
Cold boot spends its early life in ROM and does not exercise that path
the same way. That is the difference between the two, and it is where to
look.

## Where to look next

The hang is in translated code, not in the lookup or translation
machinery (all three counters freeze). So: which emit path does Emerald's
Thumb idle loop at 0x080008C6..0x080008CE take, and why does that path
not emit a `reg_cycles` check? `generate_branch_cycle_update` and
`generate_indirect_branch_cycle_update` are the candidates.

## The 2.6x gap does not survive an apples-to-apples measurement

`Makefile.n64` records the dynarec at 20.5 N64 cycles per GBA cycle
against the interpreter's 7.9. Those two numbers were taken on different
workloads, at different points in a run, with a metric the interpreter
flatters itself on: the interpreter skips the idle loop, advancing
emulated cycles at almost no host cost, so any interval containing idle
time reads far better for it than for a dynarec that executes the loop.

Measured the same way — CP0 `COUNT` against `cpu_ticks`, same ROM, same
ares build, both from a cold boot:

| | N64 cycles per GBA cycle |
| --- | --- |
| **identical emulated interval** (464,469 GBA cycles, the JIT's whole reach) | |
| &nbsp;&nbsp;dynarec | **50.9** |
| &nbsp;&nbsp;interpreter | **53.9** |
| interpreter over a full 620 s run (3.47 G cycles, ~12,400 frames) | 22.7 |
| dynarec, steady-state intervals (no translation in them) | 21.5 |
| real time | 5.56 |

**Over the only interval where both have been measured on the same work,
the dynarec is ahead**, and its steady state matches the interpreter's
long-run average. The 2.6x deficit is an artifact of comparing a
translation-heavy early boot against a settled interpreter average.

That does not make the dynarec faster — it makes the two roughly equal,
about 4x short of real time, on the early-boot workload. What it does is
retire the reason for not working on it.

### What this does not show

Both figures are from the boot and intro. Neither has been measured on
gameplay, because the dynarec still hangs when started from the
savestate, and that is the only route this project has to gameplay. An
attempt to measure the interpreter from the savestate for comparison
produced a byte-identical log to the cold-boot run — it had silently
booted cold — and was discarded rather than reported.

## What is left to make this competitive

In priority order, with what is known about each:

**1. The savestate hang. Everything else is downstream of this.** It is
not merely a benchmarking inconvenience: it is the reason no one has ever
seen the dynarec run Emerald's actual game code, and therefore the reason
every performance claim about it — including the ones above — is about
the boot sequence. Fix it and the question "is the dynarec worth it"
becomes answerable in one run.

The lead is in the elimination table above: the last blocks translated
before the hang are the BIOS IRQ vector, so the next block would be the
game's handler in IWRAM — the RAM translation path, which a cold boot
barely exercises. Instrument `block_lookup_translate_*`'s RAM branch
(the `pcregion` 0x2/0x3 case) and watch what it does with the tag on the
first IWRAM block after a state load.

**2. Block linking — already implemented, and not the problem.**
`cpu_threaded.c:3343-3383` patches internal branches to recorded block
offsets and resolves external ones through `block_lookup_translate_*` at
translation time, patching the jump to go straight at the target block.
The 22.0 figure was always *with* linking.

What linking cannot patch is an **indirect** branch — `BX`, `POP{pc}`, a
computed jump — because it has no fixed target. Each one spilled every
ARM register, called into C to walk a hash table, and reloaded them. That
now has a one-entry direct-mapped inline cache per site
(`mips/mips_stub.S`, `-DN64_JIT_IBCACHE`, on by default for JIT builds):
a repeat visit is six instructions and no register traffic.

It works and it is nearly worthless. `block_lookup` falls from 21.6% of
sampled instructions to **0%**, translated code rises from 67.7% to
95.0%, and the frame cost moves **22.0 -> 21.5** — 2%. Emulation is
bit-identical, same `cpu_ticks` at every checkpoint.

**3. Memory-access inlining — not worth building.** With the inline cache
in, every stub in `mips_stub.S` *combined* — memory access, `update_gba`,
the indirect-branch trampolines — is 4.7% of sampled instructions. By the
same instruction-to-time ratio that made a 21.6% instruction saving worth
2% of time, that is worth roughly 1%. gpSP's own attempt at this, the JAL
patcher, was restored with correct cache maintenance and measured **1.9x
worse** (`mips/mips_emit.h:2779`).

## Where the time actually is

With 95% of executed instructions now inside translated code, the
dynarec's 21.5 N64 cycles per GBA cycle **is the cost of the generated
code itself** — not dispatch, not block lookup, not the stubs. Beating
the interpreter's 7.9 needs the generated code to get about 3x better,
which is a code-quality problem: register allocation, redundant flag
computation, and the instruction sequence emitted per ARM opcode. It is
not reachable by removing dispatch overhead, because there is no longer
meaningful dispatch overhead to remove.

**A methodology note that cost real time to learn here.** `GPSP_PCPROF`
samples every N *executed instructions*, so it is an instruction profile,
not a time profile. It overstates code that runs many cheap, predictable
instructions and understates code that stalls. A 21.6% share of samples
turned out to be 2% of cycles. Use it to find *what runs*, never to
rank *what costs*.

**4. `$gp`.** The register allocator keeps ARM r13 in `$gp`, which
collides with gp-relative code and libdragon's `inthandler`; the
workaround defers interrupts across the whole translated window.
Fixing it (`-G0`, or freeing `$gp`) removes the workaround and returns a
register. Modest direct payoff.

**Not worth doing:** reducing cache maintenance. Removing the
full-I-cache invalidate after each emit halves the time spent in
`translate_icache_sync` (21K -> 11K ticks) and changes the frame cost not
at all — 256.0 against 257.0 on translating intervals, 22.0 either way on
steady ones. The expensive intervals are the translation work itself.

## Measuring the dynarec in ares at all

ares runs the JIT build at roughly 0.2% of real time — 620 s of wall
clock bought 0.67 s of emulated N64 time. A guest that writes code into
RDRAM and executes it is a recompiling host emulator's worst case: ares's
`invalidateRange` marks the 4 KB section dirty and sets
`Pipeline::EndBlock` whenever the guest writes into the section it is
executing from, which a dynarec does constantly. Its own recompiler is
not thrashing (`ARESEMIT` reports 766 emits total and zero during a run,
no allocator flushes), so this is block-ending overhead, not
recompilation.

The consequence is a methodology rule: **wall-clock fps is meaningless
for the JIT build.** Use CP0 `COUNT` against `cpu_ticks` — both are
emulated-side and hardware-faithful — and check that COUNT has not
wrapped (32-bit at 46.875 MHz, so 91.6 s).

## The savestate "hang": indirect branches jumped to the block they were in

Restored from `bench-results/states/overworld.sav`, the dynarec translated
four blocks, pinned the ARM PC at 0x080008C6, completed zero frames, and
sat there indefinitely (verified over 900 s — only PC samples accumulated,
no further `update_gba` calls). The interpreter, from the same savestate,
ran 3968 frames.

The cause was in `mips_indirect_branch_arm/thumb`. An indirect branch — BX,
POP{pc}, `ldr pc, [...]`, LDM with r15 in the list — passes its destination
in `$4`, because `arm_to_mips_reg[]` maps ARM r15 to `reg_a0`; that is what
mips_emit.h's "a0 holds the destination" means. Commit 139d743 changed both
stubs to `addu $4, $19, $0`, replacing the destination with `$19`, the
block's own start address, so every indirect branch resolved to the block
it was already in.

Nothing goes wrong until the first interrupt. gpSP's open BIOS dispatches
IRQs through

    00000018: b 0x20
    00000020: stmdb sp!, {r0-r3, r12, lr}
    00000024: mov   r0, #0x04000000
    00000028: mov   lr, pc
    0000002c: ldr   pc, [r0, #-4]      ; [0x03FFFFFC] = handler pointer

so the first IRQ runs `ldr pc` and the block at 0x20 jumps to itself for
ever. From a savestate that is the first VBlank, 448 hardware events in.
A cold boot survives only because 180 s of ares is not enough to reach one.

### What ruled everything else out

Worth recording, because most of these look plausible and cost a run each:

- The emulated timeline is **correct**. JIT and interpreter agree
  tick-for-tick across all 448 `update_gba` calls from the same savestate.
  Whatever is wrong is not an emulation-accuracy bug.
- `update_gba`'s last return value is `0x400003C0` — bit 30 (`changed_pc`)
  with `reg[REG_PC] = 0x18`. The IRQ really was raised and delivered.
- Blocks at 0x18 and 0x20 *are* translated (`JITX arm rom 00000018/20`).
- IWRAM 0x7FFC really holds the handler pointer (raw `50 27 00 03` =
  0x03002750), so neither the savestate nor the load path is at fault.
- With `N64_JIT_IBCACHE=` the log is an unbroken run of
  `LOOKUP arm 00000020`, which is what named the bug.
- Re-enabling N64 interrupts around `update_gba` changes nothing: the run
  is byte-identical. See `-DN64_JIT_NO_IRQ_WINDOW`.

`JITX` only prints when a block is actually *translated*, so a lookup that
keeps returning the same block leaves no trace. `-DN64_JIT_TRACE` now also
prints `LOOKUP <mode> <pc>` on every inline-cache miss, which is what makes
this shape of bug visible at all.

### Where it stops now

Past the fix: 151 blocks translated, 154 lookup hits, the game's IWRAM
handler running (`JITX thumb ram 03001AA8`), frame 23100 reaching vcount
167 — past VBlank. Then, ~46 events later, an indirect branch resolves to
0x114230B6 and ares freezes the CPU on an unmapped RCP access, with
`$sp = 0xa4fffed0` and GBA addresses ORed into KSEG1 elsewhere in the
register file (`a0 = a5000130`, `at = a5000350`). That is the
address-computation bug the `JITK1` and `n64_jit_scan_sp` probes in
cpu_threaded.c were written for — pre-existing, and now reachable.

### Where the dynarec actually stands

Over the interval both engines can run from the same savestate — 205,808
GBA ticks, the game's VBlank poll loop:

| engine | N64 cycles per GBA cycle |
| --- | --- |
| dynarec | 86.81 |
| interpreter | 69.45 |
| real time | 5.56 |

The dynarec costs 1.25x the interpreter there. Both are ~10x their
steady-state cost because a poll loop is the worst case for per-event
overhead — the interpreter's figure over 3968 frames of the same scene is
7.02. There is still no measurement of the dynarec over gameplay, because
it does not yet survive gameplay.

## The harness bug that invalidated a day of JIT numbers

`tools/jit-savestate-run.sh` used to run ares as

    ( cd "$SCRATCH" && ... ares ... ) >ares.log 2>&1
    cp "$SCRATCH/ares.log" "$REPO/bench-results/$LABEL.jit.txt"

The redirection is set up by the parent shell, so it landed in the repo,
not in `$SCRATCH` — while the tar that stages the scratch tree copied the
repo's own `ares.log` into `$SCRATCH`. Every published log was therefore
written by an *earlier* run of a *different* build. Two nominally identical
builds gave opposite results; a freshly added diagnostic print never
appeared. It was caught by comparing mtimes: the published log was five
minutes older than the `gpsp.elf` it supposedly came from.

The script now writes to an absolute path, excludes `ares.log`/`run.log`
from the tar, and refuses to publish a log that is not newer than the ROM.
If you write a benchmark wrapper, make it prove its output is fresh.
