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

### The second bug: the dynarec had no BIOS-call hook

SWIs 0xF0-0xFF are unused by the GBA BIOS, so this port patches the game
with them: `n64/m4a_hle.c` replaces the sound driver's four hot mixer loops
with `swi 0xFn0000` markers and runs them natively. `cpu.cc` intercepts
those in both the ARM and Thumb decoders — `bios_hle_swi()`, then step over
the instruction. `arm_swi()` in the dynarec had no such hook: it ran
`execute_swi` and branched to 0x00000008 unconditionally, feeding the
marker to the open BIOS's dispatcher as a real SWI 0xF0. Hence the garbage
branch to 0x114230B6.

A savestate carries the patched IWRAM with it, so this fires whether or not
the HLE is compiled in — which is why building with `N64_M4A=` changed
nothing, and why that test was misleading the first time it was run.

Two things about the fix are worth keeping in mind:

- The SWI number is known at translate time, so only the 0xF0 block needs
  the new path; everything below it keeps the original code.
- The call must go through an asm shim (`n64_jit_swi_hook`), not a bare
  `jal`. The first attempt called the C function directly and still
  crashed: the handler reads and writes `reg[]`, while the ARM registers
  live in caller-saved MIPS registers that a C call destroys, and `$gp`
  holds ARM r13 rather than the real gp. Every C call out of translated
  code has to spill first and let `cfncall` restore `$gp` — the SWI hook is
  no exception.

### Where the dynarec actually stands

It runs gameplay. From the overworld savestate, across the two fixes:

| | before | after indirect-branch fix | after SWI hook |
| --- | --- | --- | --- |
| frames completed | 0 | 0 | 3098 |
| blocks translated | 4 | 151 | 1477 |
| block-lookup hits | 0 | 154 | 1514519 |
| outcome | spins | freeze | runs |

Measured against the interpreter on the same scene, from the same
savestate, both with `-DN64_TIME_TRACE` only:

| engine | N64 cycles per GBA cycle |
| --- | --- |
| dynarec | 7.47 |
| interpreter | 7.02 |
| real time | 5.56 |

Run-to-run spread is around 8% (an earlier dynarec run of the same scene
gave 6.93), so the two engines are at **parity** — the dynarec is not
faster than the interpreter on this workload. That is now a performance
question with a number attached rather than a crash, which is the point of
these two fixes. Note that `ares` wall-clock frame counts still cannot be
compared between the two builds (see above); only the cycle ratio is
meaningful.

### The crash that stood between the two fixes

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

## Where a frame goes, and the two levers that follow from it

With the dynarec running, the first useful measurement is what a frame is
made of. From the overworld savestate:

| | share of frame |
| --- | --- |
| CPU emulation | 77% |
| blit | 17% |
| PPU | 4% |

So the CPU side is the right target — but not for the reason the label
suggests. Per frame the emulator makes **675 `update_gba` calls while the
game executes about 7,400 GBA instructions**. Under `-DPROFILE_CYCLES`
that CPU time splits 51% inside `execute_arm_translate`, 41% inside the
`update_gba` body, 6% PPU, 9% in `sound_timer`. At 670 yields per frame
that is roughly 1,200 N64 cycles of dynarec entry/exit per yield and 1,000
in the event machinery. **Almost none of it is executing translated code**,
which is exactly why the dynarec started out no faster than the
interpreter: neither engine was spending its time on instructions.

`-DN64_EVENT_PROF` breaks down what shortens the CPU window. Baseline:
66% video (two events per scanline), 32% timer 0, serial and DMA zero.

Both levers therefore attack the *number of yields*, not the cost of
translated code:

- **`-DN64_TIMER_BATCH`** — timer 0 feeds the direct-sound FIFO, which is
  where the emulator's audio actually comes from, so its events cannot be
  dropped. They can be batched: an overflow only needs precise timing if
  something observes it at that instant (an IRQ, or a cascade into a timer
  that may raise one). `update_timers` now services however many overflows
  fall inside the window.
- **`-DN64_MERGE_HBLANK`** — the second video event of each scanline exists
  only so an HBlank IRQ or HBlank DMA can fire on the right cycle. Measured
  on the overworld, neither is ever armed. When both are disarmed the two
  transitions are done together and the CPU gets the whole 1232-cycle
  scanline. This flag predates the dynarec and was measured and rejected
  once — back when the renderer dominated the frame and the CPU side did
  not. That is no longer true.

Measured over an identical 2384-frame window from the same savestate:

| dynarec | N64 cycles per GBA cycle | |
| --- | --- | --- |
| baseline | 7.52 | |
| + timer batch | 6.78 | −9.8% |
| + hblank merge | 6.68 | −11.1% |
| both | **5.75** | **−23.5%** |
| real time | 5.56 | |

Events per frame fall 676 → 458 → 230, and the frame from 44 ms to 33 ms.
The two levers are very nearly independent.

Both help the interpreter less, because its yield is cheaper to begin
with:

| | baseline | both |
| --- | --- | --- |
| dynarec | 7.52 | **5.75** |
| interpreter | 7.07 | 6.50 |

So the ordering flips: the dynarec starts 6% behind the interpreter and
ends 12% ahead of it, at 97% of real GBA speed.

### What these cost

Neither is free accuracy-wise, which is why both are opt-in.

`N64_TIMER_BATCH` makes sound DMA fire in bursts, and that visibly changes
what the game does — the interpreter's idle-loop detector fires 17,626
times per window instead of 25,849, and 381K instructions run instead of
442K. It also *slows the interpreter down* by 4% on its own while speeding
the dynarec up by 10%, so it is not a general win.

`N64_MERGE_HBLANK` means the CPU never runs with the HBlank flag set, so
code polling DISPSTAT bit 1 would not see it, and a VRAM write made
"during HBlank" lands before the line it precedes rather than after. The
condition is re-checked every scanline, so a scene that arms an HBlank IRQ
or DMA drops straight back to the two-phase schedule.

### What is left

At 230 yields per frame the remaining event traffic is one video event per
scanline, which is hard to reduce further without giving up scanline
rendering. The other half of the cost is the ~1,200 N64 cycles of dynarec
entry/exit per yield: `mips_update_gba` is only about 50 instructions, so
that figure is cache misses, not work — `save_registers`/`restore_registers`
touch `reg[0..31]`, eight D-cache lines, and the event machinery in between
evicts them. Reducing the spill, or keeping `reg[]` resident against the
rest of the working set, is the next thing to try.

## Two things that looked like levers and were not

Both are recorded because each cost a build-and-measure cycle and each
looked compelling from the profile.

**Skipping sound mixing that nothing reads.** `-DPROFILE_CYCLES` attributed
15% of emulation time to `sound_timer`, and the default build has no
`N64_AUDIO_OUT`, so `n64_audio_render_frame` — the only consumer of
`SNDBUF` — is compiled out. Rendering samples into a buffer nobody reads
is obviously wasted work. Skipping the four rendering loops while leaving
the FIFO pop, the state writeback and the DMA refill untouched measured
**+0.4%**: no change. The 15% was largely the cost of *measuring* it —
`PROFILE_CYCLES` puts two `PROF_TICK()` reads around every one of the ~224
`sound_timer` calls per frame. Treat any small bucket in that profile as
suspect for the same reason.

**Collapsing the VBlank period.** After the HBlank merge, 68 of the 228
remaining events per frame are VBlank scanlines that render nothing —
their event only advances VCOUNT and tests the VCount match. Collapsing
them into one event took events per frame from 230 to 163, a 29%
reduction, and bought **0.8%**.

The first attempt gated the skip on the VCount IRQ being disarmed and
never fired at all: Emerald arms it every frame (DISPSTAT `0x9629`) but
matches on line 150, before VBlank starts. The guard that matters is where
the match *line* sits, not whether the IRQ is armed. Worth remembering for
any similar gate.

The 0.8% is the useful part. **Event count is not itself the cost.** The
two levers that did work removed events that were *doing work* — a
sound-FIFO overflow that renders samples, an HBlank transition that scans
four DMA channels — not merely events. Removing 67 cheap events per frame
changes almost nothing, so there is no more speed hiding in the event
schedule, and the VCOUNT-visibility trade it asks for is not worth 0.8%.

## What is actually left, measured

Default build, overworld savestate, median of ~60 PROF windows:

| | per frame | share |
| --- | --- | --- |
| total | 28.7 ms | |
| emulation | 20.9 ms | 73% |
| blit | 7.8 ms | 27% |
| — of which PPU | 1.8 ms | 6% |

Inside blit, the renderer reports `sort 0.89 ms, emit 4.16 ms,
wait-for-RDP 0.25 ms`. **Building RDP command words on the CPU, 4.16 ms a
frame, is the largest single identifiable block of work left** — larger
than anything remaining on the dynarec side, and it is not a dynarec
problem.

On the emulation side the honest statement is that the split is not yet
resolved: `-DPROFILE_CYCLES` puts 63% of emulation in the
`execute_arm_translate` bucket, but that bucket is dynarec entry/exit plus
translated code together, the JIT build does not maintain the instruction
counter the interpreter uses, and the VBlank-collapse result says the
per-yield component is smaller than it first appeared. Splitting that
bucket — a cycle counter around the spill/reload in `mips_update_gba`, and
an instruction count for the JIT — is the next measurement, not the next
optimisation.

## The AOT static recompiler is unreachable in the default build

`tools/thumb2c.py` statically recompiles hot Thumb functions to C, producing
95,000 lines in `n64/aot_generated.c`. It is dispatched from `cpu.cc:3362`,
inside the **Thumb interpreter loop**. `cpu_threaded.c` contains no
reference to it at all.

So making the dynarec the default switched the whole thing off. Nothing
warns about this; the build simply stops using it.

Priced, in an interpreter build with `-DPROFILE_CYCLES`:

    interp 51%  aot 18%  event 21%  ppu 8%  snd 13%
    388 AOT calls/frame, ~14 of 8192 4 KB pages covered

18% of emulation time, and still not enough: interpreter+AOT is 35 ms/frame
against the dynarec's 26.5. The AOT does not compensate for interpreting
everything it does not cover.

The combination nobody has built is AOT **plus** dynarec — at translate
time, if a block's PC has AOT coverage, emit a call to the native function
instead of translating it. The two paths have never run together.

## Hybrid AOT + dynarec: the selection criterion is memory density, not hotness

The two engines have opposite strengths, and the asymmetry is in the memory
path rather than anywhere in the code generation.

**Dynarec memory op.** A hand-written asm stub: region decode, mask, add to
a direct base, byte-swap, SMC tag check, store. IWRAM and EWRAM never touch
a page table.

**AOT read** (`aot_read32`): a C call into `aot_map()`, which indexes
`memory_map_read[addr >> 15]` — an 8192-entry pointer table, **32 KB, four
times the D-cache** — then loads, eswaps and rotates. It pays that lookup
for *every* region, including IWRAM and EWRAM where the dynarec uses a
direct base.

**AOT write** (`aot_write32`): a C call into `write_memory32`, the full C
write path with its region switch, SMC handling and IO dispatch.

So an AOT memory op is strictly more expensive than a dynarec one. What AOT
buys instead is everything *between* the memory ops: GCC allocates registers
across the whole function, does CSE and strength reduction, and never spills
the flag cache at a block boundary.

That gives the selection rule:

> **AOT should take compute-dense functions. The dynarec should keep
> memory-dense ones.**

which is not the same as "AOT should take the hot ones", and explains why an
earlier session measured AOT coverage expansion as a *loss*: expanding by
hotness drags in memory-dense functions, which is exactly what AOT is worst
at.

Measured over the 690 generated functions (memory helper calls per 100 lines
of generated C):

    p0   1.6   p10  3.4   p25  3.4   p50  6.6   p75  9.1   p90 11.5   p100 23.5

A 15x spread, so the criterion discriminates. The extremes:

    worst AOT candidates            best AOT candidates
    0x0806F694  23.5                0x0808904C   1.6
    0x08001228  20.0                0x080894D4   1.7
    0x082E0398  19.3                0x080898D4   2.0

### The move that may dissolve the choice

AOT's disadvantage is not inherent — it is that `aot_generated.c` calls C
helpers instead of the dynarec's stubs. Those stubs already take the address
in `$4` and return in `$2`, which is the C ABI for `u32 f(u32)`; what they
additionally need is `$16` holding the register base. Pointing the AOT at
them would give AOT the fast memory path *and* GCC's optimisation, and the
hybrid question largely goes away.

Worth testing before spending effort on per-function selection.

### Mechanism, already proven

`n64_jit_swi_hook` in mips/mips_stub.S is the exact shape required: spill the
ARM registers, let `cfncall` restore `$gp`, call into C, reload. The dynarec
would check `aot_page_bits[pc >> 12]` at translate time and, on a hit at the
block's entry PC, emit that call instead of translating the block.

## The memory-density hypothesis, tested to completion, and what actually explained the gap

The hybrid was built (`-DN64_JIT_AOT`), and it deadlocked, and the deadlock
was fixed (m4a's code-patching bypassed the dynarec's store stubs, so a
stale translated block kept running the unpatched SWI marker forever --
see the git history for `n64/m4a_hle.c`). With that fixed, the hybrid ran
cleanly end to end for the first time: **30.24 ms/frame against the
dynarec alone's 27.65 ms** -- genuinely ~9% slower, not deadlocked.

The "AOT's memory path is strictly worse" theory from the previous section
was then tested directly, in two independent ways, and **both came back
negative**:

- Routing AOT's IWRAM/EWRAM loads through the dynarec's own asm stubs
  (`aot_stub_ld` in mips/mips_stub.S) measured **worse**: 32.38 ms/frame.
  The trampoline needed to call an asm stub from C -- push, save `$16` and
  `$ra`, materialise the register base, indirect call, pop -- cost more
  than the `memory_map_read` lookup it replaced.
- Skipping that trampoline and reading IWRAM/EWRAM directly in C
  (`N64_AOT_DIRECTMEM`, a `static inline` doing exactly what the
  interpreter's `readaddress32(iwram_raw, ...)` does, zero call-boundary
  crossing at all) measured **flat**: 30.34 ms/frame, statistically the
  same as going through `aot_map()`'s 32 KB table.

So the 32 KB page table was never the expense. Both experiments that were
supposed to remove it did nothing.

### What it actually was

`AOT_OPT=-Os` (see Makefile.n64) applies only to `aot_generated.c` -- the
file holding the 690 translated function bodies -- and it was chosen and
measured for the *interpreter*+AOT combination, where it is the right
choice for a real reason: AOT there competes with interpreted dispatch for
the same 16 KB I-cache, and interpretation is universally slow (150-175
cyc/insn), so keeping AOT's footprint small to avoid mutual eviction
matters more than how fast any individual AOT body runs.

Rebuilding just that file at `-O2` (`AOT_OPT=-O2`), with the hybrid,
dropped the frame to **28.34 ms** -- essentially the whole gap -- with the
memory path completely unchanged (still `aot_map()`, no direct-mem, no
stub routing). Adding `N64_AOT_DIRECTMEM` on top brought it to **28.02
ms**, so the direct-memory idea was real but small (~0.3 ms), not the
story. `AOT_OPT=-O3` made it worse again (29.56 ms) -- consistent with
this project's other findings about `-O3` on N64 code: more aggressive
inlining and unrolling grows the AOT blob past what the 16 KB I-cache can
hold resident.

Reproduced exactly on a second independent build (30.06/30.24 vs 28.02 ms,
twice), and the block-routing count (3433 of 12265 blocks emitted as AOT
thunks) was verified identical across every one of these runs, so none of
it is a difference in which code ran -- only how well it was compiled.

**The tempting explanation -- that `-Os` was blocking cross-TU inlining of
the tiny `aot_read32`/`aot_write32` helpers into their thousands of call
sites, and `-O2` fixed that -- is wrong**, and was ruled out by
disassembling the hottest AOT function in both builds: the call count and
call targets to `aot_read32`/`aot_read16`/`aot_read8` are *identical*
between `-Os` and `-O2`, and the function itself only grows ~7% at `-O2`.
No inlining of the memory helpers happens at either level. The `-Os`/`-O2`
difference is ordinary codegen quality -- register allocation, instruction
scheduling on the VR4300's in-order pipeline -- applied to code that was
being deliberately compiled for size, in a context (the hybrid) where size
no longer buys anything, because there is no slow interpreter fallback
left to discount against.

### Where the hybrid actually stands

    dynarec alone                       27.6-27.7 ms/frame
    hybrid, AOT_OPT=-Os (as committed)  30.1-30.2 ms/frame   -9% vs dynarec
    hybrid, AOT_OPT=-O2                 28.3 ms/frame        -2% vs dynarec
    hybrid, AOT_OPT=-O2 + DIRECTMEM     28.0 ms/frame         within the
                                                               ~2.2% layout
                                                               noise floor

So with both fixes, the hybrid is no longer clearly worse than the
dynarec -- but it is not clearly better either. `-DN64_JIT_AOT` stays off
by default. If it is revisited, the AOT_OPT override is not optional: the
existing `-Os` default is correct for the interpreter path and actively
wrong for this one, and shipping the hybrid without changing it would
regress by 9% for no reason anyone would notice was fixable.

One process note: an earlier attempt to characterise this used
`-DN64_JIT_AOT_HOOKPROF`/`-DN64_JIT_AOT_BODYPROF` (COUNT-wrapped timers
around the hook and the AOT call) and got a result in the *opposite*
direction from the clean frame-time comparison -- more hook/body time at
`-O2` than at `-Os`, which would have implied `-O2` made things worse.
That instrumentation is real and kept in the tree, but it was not trusted
here: adding it is itself a nontrivial code-size change to files outside
`aot_generated.c`, and this branch has already demonstrated (the
`N64_STUBPAD` result) that layout alone is worth ~2.2% of a frame. An
instrumented A/B comparing two *differently-sized* binaries is exactly the
condition that number was measured under. The uninstrumented frame-time
numbers, and the static disassembly, are what the conclusion above rests
on -- not the hook/body timers.

## RSP offload for RDP command generation: proof of concept, verified correct

Followed up on the per-tile emit loop residual (~2.44 ms/frame, 95% of it
unexplained by any of the three instrumented ranges -- see above) with a
genuinely different approach: move the computation to the RSP, which sits
nearly idle once the RDP renderer absorbed the blit, rather than continue
trying to explain why it is slow on the VR4300.

The obstacle this project had already hit once: the old `rsp_gbascan.S`
(a full CPU-rasterization-era compositing engine -- palette lookup,
blending, 582 lines, now dead code with the RDP doing that job in
hardware) uses the raw `rsp_init`/`rsp_load` API, which loads its own
ucode over whatever is resident and cannot coexist with `rdpq`, which
also drives the RSP. That is a real architectural constraint, not
incidental -- see the Makefile.n64 comment next to `N64_RSP_BLIT`.

The actual fix is libdragon's `rspq` overlay mechanism: `rspq_overlay_register()`
lets a *second* ucode's command handlers coexist with rdpq's in one
combined RSP binary, dispatched by (overlay ID, command index) from a
lockless CPU/RSP queue. This is precisely how rdpq itself is implemented
-- one overlay among however many are registered -- and it was previously
undocumented in this codebase (the earlier "AOT+dynarec" work concluded
"the RSP is nearly idle" without ever testing whether a second overlay
could actually share it).

### What was built

`rsp_rdpbg.S` -- a new rspq overlay, scalar RSP code (no vector unit; the
per-tile work is a handful of branchy integer compares and shifts, not
lane-parallel). One command, `RDPBGCmd_Batch`: DMA in a contiguous batch
of `rdpbg_draw_t` records (the CPU has already sorted these into one
(slice,palette) run), reproduce `RDPBG_RECT` exactly -- clip against the
0..240 screen width, flip handling, the 10.2/10.5/s5.10 fixed-point
encoding of the RDP's `TEXTURE_RECTANGLE` (0xE4) command -- and DMA the
resulting words back out, dropping tiles that clip away entirely, same as
the CPU path.

`n64/n64_rsp2.c` -- registers the overlay and runs two checks at boot,
behind `-DN64_RSP_RDPBG_TEST` (off by default; nothing live depends on
this yet):

  1. **Correctness**: 9 synthetic tiles covering every branch combination
     (unflipped / h-flip / v-flip / both, on-screen / left-clip /
     right-clip / fully-clipped-away, and left-clip crossed with h-flip,
     the one case where the clip math takes the "s0 += x0" branch instead
     of "s0 -= x0") are run through the RSP command and compared word for
     word against a from-scratch C reference of the same computation.
     **Result: 9 tiles, 8 kept, PASS, 0 mismatches.** The 9th (fully
     off-screen) tile is correctly dropped with no output, matching the
     CPU's silent `continue`.

  2. **Throughput**: a 64-tile synthetic batch (closer to the real
     ~45-tile average per (slice,palette) group than the 9-tile
     correctness set, whose fixed per-call overhead -- DMA setup, rspq
     dispatch -- would dominate and say nothing about steady state).
     **Result: 9392 PClock total, 146 PClock/tile**, fully synchronous
     (the CPU is blocked on `rspq_syncpoint_wait` for the entire
     measurement -- this is an upper bound on RSP cost, not a hidden-
     latency number). For comparison, the CPU's own per-tile cost for
     this same work is ~190 PClock (the ~95 COUNT/tile figure from the
     chassis measurement above, `x2` for PClock). The RSP is not
     dramatically slower doing this scalar -- it is in the same order of
     magnitude as the CPU, synchronously, with none of the overlap
     benefit realised yet.

Verified end to end: default build compiles and runs identically
(27.66 ms/frame, canary green, no freeze) with these two new files
unconditionally in the object list -- the whole thing costs nothing when
`-DN64_RSP_RDPBG_TEST` is not set. With it set, the emulator boots,
selftest passes, and 38 further PROF windows run clean (canary green,
1216-1217 tiles, no freeze) -- the RSP command coexists with rdpq's own
real per-frame rendering commands in the same running session.

### What this does NOT yet prove

This is a synchronous correctness-and-throughput probe, not a live
feature, and the gap between the two is real work:

  - **No overlap.** The whole point of RSP offload is hiding this cost
    behind CPU work happening at the same time; nothing here runs
    asynchronously yet. Realising it needs double-buffering the draw
    list across frames (CPU builds frame N+1's list while the RSP still
    emits frame N's commands) and a real synchronisation point in the
    frame loop, not `rspq_syncpoint_wait` called inline.
  - **No live per-frame wiring.** `n64_rdp_bg.c`'s actual flush loop is
    untouched; this runs a synthetic self-test alongside the real
    pipeline, not in place of it.
  - **Only synthetic input verified.** The 9-case set is a real
    correctness gate (every branch, checked byte-for-byte), but it is not
    the actual game's draw lists across menus, battles, or other scenes.
  - **The rdpq_tex_upload / rdpq_set_tile calls at group boundaries** (21
    slice changes, 27 key changes a frame) still have to happen on the
    CPU, or be reimplemented on the RSP too -- neither attempted here.

So: the two biggest unknowns -- can a second overlay actually coexist
with rdpq, and can hand-written RSP assembly get this specific
computation right -- are now answered, with evidence, both yes. What is
not yet known is whether the live, asynchronous version delivers on the
frame-time promise; that is the next thing to build and measure, not
something this proves by itself.

## RSP offload, wired live: initially a real but small win (superseded below)

Built the live path, behind `-DN64_RSP_RDPBG_LIVE` (off by default,
requires `N64_RDP_EXEC`). `n64_rdpbg_flush()`'s per-tile loop no longer
does any clip/flip/encode arithmetic on the CPU when this is on: runs of
same-(slice,palette) draws (already contiguous in `rdpbg_order` from the
counting sort) are copied -- a plain struct copy, not the branchy
`RDPBG_RECT` math -- into batches of up to 128 and hidden to the RSP.

Two things had to change from the proof-of-concept to make this safe to
run asynchronously rather than just as a synchronous probe:

  - **Fixed-size output.** The RSP kernel now emits exactly 16 bytes per
    tile *always* -- a dropped tile becomes a degenerate zero-width
    `TEXTURE_RECTANGLE` instead of no output. Without this the CPU cannot
    know where a batch's output ends until the RSP reports back how many
    tiles it kept, which would mean waiting on every batch before placing
    the next one -- exactly the serialisation this is supposed to avoid.
    With it, the CPU reserves each batch's slot in `rdpbg_cmds` the
    instant it decides to queue the batch.
  - **One-batch-deep pipeline**, not a synchronous call per batch: at
    every tile-state boundary, the batch queued at the *previous*
    boundary is waited on and handed to `rdpq_exec` -- which is also the
    earliest point it's safe to change tile state, since the RDP command
    stream only cares about the order the CPU calls
    `rdpq_exec`/`rdpq_tex_upload`/`rdpq_set_tile` in, not the order the
    RSP actually computed each batch's contents -- and the batch just
    finished gathering is queued (not waited on) as the new pending one.
    Gathering alternates between two scratch buffers, safe because by the
    time gathering reaches a given slot again, that slot's previous
    occupant was drained one boundary ago.
  - The pending batch is deliberately **not** drained at the end of every
    `n64_rdpbg_flush()` call (there are ~30 of those a frame, one per BG
    layer entry and one per sprite-priority group). It is left in flight
    across the call boundary so it can overlap with the CPU work that
    happens *between* flush() calls -- `rdpbg_emit_bg`/`rdpbg_emit_objs`
    walking the next layer's tilemap in video.cc. `n64_rdpbg_end()` drains
    whatever is left exactly once, at the true end of frame.

### Correctness

Ran the existing 9-case selftest plus a full boot-to-gameplay session on
ares from the overworld savestate. Selftest: PASS, 0 mismatches, same as
the synchronous proof-of-concept. Canary across every window: 100% of
screen, 21 TMEM slices, 27 palette groups -- unchanged. No freeze across
boot or sustained gameplay.

One canary number does change, deliberately and for a documented reason:
tile count reads ~1257 instead of ~1216-1223. The CPU path's
`n64_rdpbg_tiles` counts only *kept* tiles (it increments after the
`x0 >= x1` clip-and-continue check); the RSP path counts tiles
*submitted* to a batch, because the fixed-size-output design that makes
the async pipeline possible means the CPU never learns which ones the
RSP silently turned into degenerate rects. The ~3% difference is exactly
the fully-clipped-away tiles at layer edges, rendering nothing either
way -- it is not corruption, and the screen-coverage figure next to it
(the actual correctness canary) is computed independently in video.cc and
is unaffected.

### Performance

Measured on ares against the dynarec-only baseline, both from the same
overworld savestate, same PROF window count:

| build                    | ms/frame (median) | ms/frame (mean) | FPS  |
|---------------------------|-------------------|------------------|------|
| baseline (CPU path)        | 27.0               | 27.0             | 37.0 |
| RSP live (8-window sample) | 26.0               | 27.6             | 38.5 |
| RSP live (22-window sample)| 26.0               | 26.0             | 38.5 |

A real, reproducible win -- about 1 ms/frame, +1.5 FPS, +4% -- and it
holds up going from 8 to 22 steady windows (median and mean converge to
the same 26.0, unlike the 8-window sample's noisier 27.6 mean). Small,
though, and a new `rdpbg-rsp` PROF line explains why: it reports time
blocked in `rspq_syncpoint_wait`, and on this workload that is **2.4 ms of
the loop's 4.68 ms own wall time** -- over half of it. The one-batch-deep
pipeline is not hiding most of the RSP's cost; it is mostly still waiting
for it.

The reason follows directly from this renderer's shape: the average
(slice,palette) group is only ~40 tiles, and the CPU-side "overlap work"
available while a batch is in flight is just gathering the *next* group's
~40 tiles into the other scratch slot -- a handful of struct copies, a
few hundred VR4300 cycles. The RSP's own round trip for a ~40-tile batch
(DMA in, compute, DMA out, at the ~150 PClock/tile measured earlier) is
several times longer than that. A one-group-deep pipeline can only hide
as much latency as one group's worth of CPU-side gathering takes, and for
groups this small, that is not much. The net win is coming from the RSP
genuinely being a second execution unit doing this arithmetic in
parallel with *something* (a partial hide of the ~40 group's compute
behind the next group's gather, plus the CPU no longer running the
branchy clip/flip/encode math itself at all), not from a deep pipeline
successfully hiding the whole cost.

Deepening the pipeline (queue two or three groups ahead before draining
the oldest) would let more CPU-side gather work accumulate against a
given batch's RSP time and should recover more of that 2.4 ms -- untried
here, and the natural next step if this is worth pursuing further.

### What is and is not covered

Tested on one savestate, one scene (the overworld). Not tested: menus,
battles, or any scene with a substantially different draw-list shape --
this renderer's whole design assumes small (slice,palette) groups, so a
scene with larger, fewer groups (fewer distinct tiles on screen at once)
would change the CPU-gather-vs-RSP-batch-time ratio the discussion above
depends on, in either direction. Default build (flag off) is unaffected:
same 27.0 ms/frame, same object files, same everything -- verified by a
byte-identical `.text`/`.data`/`.bss` size to a build from before this
change.

## Deepening the RSP pipeline: the real win was behind a missing flush, not just depth

Follow-up to the "small win" above, doing exactly what its last section
proposed: queue more than one RSP batch ahead of the CPU instead of
waiting on each one at the very next boundary.

### What changed

`n64_rdp_bg.c`'s single `rsp_pending_valid`/`sp`/`off`/`words` scalars
became a small ring, `rsp_pending[RDPBG_RSP_DEPTH]`, with a head index and
an outstanding count. `rdpbg_rsp_close_batch()` now only drains the
*oldest* outstanding batch once `RDPBG_RSP_DEPTH` are already queued,
instead of draining the previous one every single time; `n64_rdpbg_end()`
drains whatever is left in a loop rather than a single call. Gathering
cycles through `RDPBG_RSP_DEPTH + 1` scratch buffers (one more than the
number that can be outstanding, for the one the CPU is actively filling).
None of this touches the RSP kernel itself -- rspq already serialises
queued commands on the RSP side one at a time, reusing the same static
DMEM buffers safely between them; deepening the pipeline is pure CPU-side
bookkeeping about *when to wait*, not a ucode change.

`RDPBG_RSP_DEPTH` is a plain `#ifndef`-guarded macro, overridable via
`EXTRA_CFLAGS=-DRDPBG_RSP_DEPTH=N` for A/B testing without editing the
file.

### DEPTH=8 hung, and why

Measured DEPTH=1, 4 and 8 on ares (overworld savestate). DEPTH=4 worked
and helped (see numbers below); DEPTH=8 hung solid -- ares's own PC
sampler stuck reporting the same PC and `ra` indefinitely, no further PROF
output, for the entire run.

Cause: `n64/n64_rsp2.c`'s `n64_rsp_rdpbg_queue()` called `rspq_write()` to
enqueue a batch but never `rspq_flush()`. Only `rspq_syncpoint_wait()` is
documented to imply a flush. At DEPTH=1 and DEPTH=4, this code happens to
call `rspq_syncpoint_wait` often enough (draining the previous/oldest
batch at nearly every boundary) that the RSP was kept fed as a side
effect. At DEPTH=8, enough batches queue up between waits that this
stopped being true, and the RSP was apparently never told the new
commands existed. Fixed by adding an explicit, non-blocking
`rspq_flush()` right after `rspq_write()` in `n64_rsp_rdpbg_queue()` --
cheap at any depth, since it does not block, and it is what let DEPTH=8
actually run.

### Results

All measured on ares, overworld savestate, 17-25 steady PROF windows
(60 frames each) per configuration, with the flush fix in place for every
row:

| DEPTH | rspwait/frame | emit/frame | frame time (median / mean) |
|---|---|---|---|
| 1 | 1.39 ms | 4.00 ms | 26.0 / 26.0 ms |
| 4 | 1.10 ms | 3.40 ms | 26.0 / 26.0 ms |
| **8** | **0.06 ms** | **2.83 ms** | **25.0 / 24.6 ms** |

(DEPTH=1 here is *with* the flush fix -- 1.39 ms rspwait, down from the
2.40 ms measured for the original one-deep design before the fix. That
0.9-1.0 ms of reclaimed emit time did not move frame time at all: a
smaller, unrelated shift in GBA-CPU-emulation time -- this project's
established direct-mapped-I-cache layout sensitivity, see
`docs/CACHE_PROFILING.md` -- happened to land in the opposite direction
that run and cancel it out. DEPTH=8 reclaimed enough that it showed
through regardless of that noise: a real, reproducible **~2 ms/frame,
+3 FPS (37.0 -> 40.0) win over the original DEPTH=1 baseline**.)

DEPTH=8 is confirmed correct the same way every step of this feature has
been: the 9-case selftest still passes with 0 mismatches, and the canary
is unchanged -- 100% of screen, 21 TMEM slices, 27 palette groups, same
~1257 submitted-tile count as every other live configuration. Shipped as
the new default (`RDPBG_RSP_DEPTH` defaults to 8). DEPTH=16 was not
tried: rspwait at 8 is already down to 0.06 ms, i.e. already almost
entirely hidden, so there is very little left for a deeper pipeline to
reclaim.

### What this does not cover

Same caveat as before: measured on one savestate, one scene (the
overworld). A scene with a different draw-list shape -- larger or fewer
(slice,palette) groups -- would change how much CPU-side gather work is
available to overlap against a given depth, in either direction. The
default (flag off) build is unaffected, verified the same way as always:
byte-identical `.text`/`.data`/`.bss` to a build from before this change.

## What's left in `emit`, and the two ideas that followed from it

With DEPTH=8 shipped and `rspwait` down to noise, `emit` (2.83 ms/frame)
broke down roughly as: ~0.6 ms in `rdpq_tex_upload` (21 calls/frame),
~0.17 ms in exec+sync+rspwait combined, ~0.83 ms in a separate `sort`
bucket (the counting sort building `rdpbg_order`), and the rest (~2.1 ms)
in the loop itself -- boundary checks, gather copies, RSP dispatch.
Looked into pushing on both the `sort` cost and the `tex_upload` cost.

### The sort step: left alone

It is already a tight two-pass counting sort over a tiny key space (at
most 64 distinct keys a call). There is no obvious algorithmic fat, and
this project already has a directly relevant data point:
`docs/CACHE_PROFILING.md` records an attempt to write draw records out in
sorted order (to turn the emit loop's gather into a sequential walk)
that measured *worse* -- the scatter cost just moved from one pass to
another, it didn't disappear. RSP offload doesn't fit this the way the
render loop did, either: a counting sort has a genuine data dependency
(nothing can be placed until the whole count/prefix-sum is known), unlike
per-tile rendering, which is independent per item and freely batchable.
Concluded this isn't worth pursuing for a ~0.83 ms ceiling.

### `rdpq_tex_upload`: real, avoidable overhead, fixed without touching the RSP

Read rdpq's actual source (`rdpq_tex.c`) instead of guessing. Our upload
is always the same fixed shape: an 8-texel-wide, 256-tall `FMT_CI4` slice
of `vram_swapped`, `tmem_addr` 0. Two things about `rdpq_tex_upload` cost
real time on every one of those 21 calls a frame that don't need to:

  - It builds a fresh `tex_loader_t` from scratch every call (assertions,
    geometry recompute -- `texload_set_rect`'s TMEM-pitch and
    LOAD_BLOCK-eligibility calculations), even though the shape never
    changes call to call.
  - Because an 8-texel-wide CI4 row is 4 bytes (not 8-byte aligned), it
    always takes the generic `texload_tile_4bpp` path rather than the
    cheaper `LOAD_BLOCK` one, emitting `SET_TEXTURE_IMAGE`, *two*
    `SET_TILE` calls (one for an internal helper tile, one for `TILE0`
    with palette 0), `LOAD_TILE`, and `SET_TILE_SIZE`. This file's own
    key-change handling, right below every call site, *immediately*
    overwrites that `TILE0` `SET_TILE` (wrong palette) and `SET_TILE_SIZE`
    the instant it runs -- a slice change always forces an immediate key
    change (`cur_key` resets to `0xFFFF`) -- so those two commands were
    pure waste every single time.

Fixed by calling the already-thin, single-call primitives
(`rdpq_set_texture_image_raw`, `rdpq_load_tile`) directly, skipping
`rdpq_tex_upload`'s generic wrapper entirely -- see `rdpbg_load_slice()`
in `n64_rdp_bg.c`. Two further simplifications fell out of the same
analysis:

  - The internal helper tile (`RDPBG_TILE_LOAD`, `TILE1` -- the same
    choice `tex_loader_t` itself makes) needs its own descriptor
    (format/pitch/address) configured only once, since it never changes
    call to call; it is now set up once a frame in `n64_rdpbg_flush()`
    instead of 21 times. Confirmed free to use: rdpq's own TLUT path uses
    a *different* internal tile (`RDPQ_TILE_INTERNAL` = `TILE7`), and the
    only other `TILE1` user in this tree is `n64_rdp_bench.c`'s one-shot
    boot selftest, never concurrent with a real frame.
  - `FMT_I8` is the same "lie about the format to address by byte instead
    of by 4-bit texel" trick `rdpq_tex_upload` itself already uses for
    CI4 -- not a raw/undocumented shortcut. What matters for correctness
    is that `TILE0`, the tile actually used to draw, is configured as
    CI4, which this file's key-change handling still does right
    afterward, unchanged.

This is a CPU-side fix, not an RSP one -- there was nothing here to
parallelise, just unnecessary genericity to cut through, the same way
`RDPBG_RECT` already hand-encodes `TEXTURE_RECTANGLE` instead of calling
rdpq's own version of that.

Applies to every build, not just `-DN64_RSP_RDPBG_LIVE` (`rdpq_tex_upload`
was called the same way from both the CPU path and the live path).
Verified correct in both: selftest still PASS 0 mismatches, and the
canary is unchanged in each -- CPU path 100% of screen / 1223 kept tiles
/ 21 slices / 27 groups, live path 100% of screen / 1257 submitted tiles
/ 21 slices / 27 groups, both exactly as before this change.

Measured on ares, overworld savestate, 17-25 steady windows:

| build | before | after |
|---|---|---|
| CPU path (no RSP) | 27.0 ms/f, 37.0 FPS | 26.0 ms/f, 38.5 FPS |
| RSP live (DEPTH=8) | 25.0 ms/f, 40.0 FPS | 24.0 ms/f, 41.7 FPS |

`emit` dropped from 2.83 to ~2.13 ms/frame on the live path -- almost
exactly the ~0.6 ms `rdpq_tex_upload` was measured costing, all of it
recovered. Same caveat as every number in this document: one savestate,
one scene.

## The SMC tag-check skip: closed as not worth building

Went looking for the next-cheapest lever. `docs/CACHE_PROFILING.md` had
an "SMC tag aliasing" section reading as two open ideas, "neither
started." Checked the actual code first rather than trusting the doc:
the first idea (move the tag block off the aliasing distance) was
already shipped as `GBA_SMC_SKEW`, just never written up. The second
(skip the tag-load entirely for stores that provably cannot be over
code, via the existing `iwram_code_max` bound) turned out to already have
its ceiling measured, in a code comment, via the *strictly larger* win of
removing the check altogether (`-DN64_NOSMC`, layout-controlled with
`-DN64_STUBPAD`): the stub code got 0.85 ms/frame faster and an unrelated
fixed function (`update_scanline`) got 0.77 ms slower from the resulting
I-cache re-alignment, netting to ~0. A conservative partial skip cannot
beat the ceiling set by removing the check outright, so it was closed
without ever being built. See `docs/CACHE_PROFILING.md`'s corrected
section for the full account.

Net result of this pass: no code change, but a real dead end closed
cheaply (reading existing comments and one already-run measurement,
instead of writing risky assembly-level dynarec changes to find out).

## Parked items, revisited: three closed, one still open

The four non-dynarec-codegen items below were all investigated (research
only, no code written for three of them) in the pass after the ARM
dead-flag work. Register allocation and flag computation (the first two
thirds of "dynarec code-generation quality") are covered by the ARM
dead-flag section below; "the instruction sequence emitted per ARM
opcode" -- whether the MIPS sequences the emitter generates for common
patterns could be shorter -- remains the one genuinely unexplored, still-
biggest lever, and is now a harder sell than it looked: the ARM flag work
shows a real, correct, well-targeted reduction in generated code can
still net negative once I-cache layout shifts are accounted for, so any
future attempt here needs the same controlled-A/B discipline, not just a
smaller-instruction-count argument.

- **Hand-encoding `SET_TILE`/`LOAD_TILE`/`SET_TILE_SIZE`** directly into
  `rdpbg_cmds`, bypassing rdpq's dispatch the same way `RDPBG_RECT`
  already bypasses it for `TEXTURE_RECTANGLE`. **Investigated, not
  worth it.** Read the actual libdragon implementations
  (`rdpq.c`): `rdpq_set_tile`/`rdpq_load_tile`/`rdpq_set_tile_size` all
  route through `__rdpq_write8_syncchange`-family functions marked
  `__attribute__((noinline))`, so each of the ~48-53 calls/frame this
  renderer makes does pay a real, uninlined function-call plus an
  autosync-bookkeeping cost on top of the thin ring-buffer write --
  but at that call *volume*, the whole addressable cost is on the order
  of tens of cycles times ~50, not the thousands-of-calls scale that made
  hand-encoding `rdpq_tex_upload` worth it. Rough estimate: well under
  0.1 ms/frame, likely inside this project's own established ~2.2%
  layout-noise floor -- not enough to justify taking over TMEM
  tile-descriptor state ourselves (the correctness-risk class this
  project has been most careful about all along) for a saving that might
  not even be distinguishable from noise in a controlled A/B.
- **`$gp` liberation.** **Investigated, not the "modest, quick" lever it
  looked like.** Read the actual mechanism (`mips/mips_emit.h`,
  `mips/mips_stub.S`): the dynarec keeps ARM r13 in `$gp` because the
  register allocator is a fixed, fully-saturated 1:1 map (all 15 usable
  MIPS registers already spoken for, confirmed during the ARM dead-flag
  work above) with no spare register to give r13 instead. Since `$gp` is
  also what libdragon's own gp-relative code (including its interrupt
  handler) uses, this port disables CPU interrupts for the *entire*
  `execute_arm_translate_internal` call -- which, per its own comment,
  "returns once per emulated frame" -- and re-enables them once back in
  C, rather than per translated block as the docs previously implied.
  That call pair itself is negligible (a COP0 STATUS toggle, twice a
  frame). The two real fixes both cost more than they look worth: (a)
  rebuild libdragon itself with `-G0` to stop it using `$gp` for small-data
  at all -- a whole-SDK build-configuration change with unclear ripple
  effects on every other libdragon subsystem, well outside "quick lever"
  territory; or (b) evict some *other* already-assigned MIPS register to
  memory to free `$gp` for r13 alone -- which doesn't remove register
  pressure, just relocates it, with no obvious reason the result would be
  faster. And the thing the workaround costs -- a whole frame's interrupts
  deferred -- has shown no symptoms in any of this session's testing
  (audio, input, and display have all worked throughout), so there isn't
  a hidden functional payoff to chase either. Not pursued further.
- **Reusing the last rendered frame when nothing changed** (static
  dialogue/menu screens) instead of re-running the BG renderer.
  **Investigated, unpromising for this specific game.** The premise needs
  a "did anything change" check to be both cheap and to actually fire
  often; Pokemon Emerald's overworld and most menus keep near-constant
  idle animation running underneath (grass sway, water shimmer, blinking
  cursors), so a real "nothing changed this frame" condition would be
  rare even on screens that look static to a player, undermining the
  premise before getting to the standing "displayed fps is what matters"
  question at all. A correct change-detector would also need to track
  writes across both PPU registers and the relevant VRAM/OAM ranges, real
  added complexity for a case that rarely pays off. Not pursued further.
- **Repo hygiene, not performance**: 14 old bisection-experiment `.z64`
  files (`gpsp_add1-8.z64`, `gpsp_bisect_*.z64`, `gpsp_cyc20_*.z64`) are
  checked into the repo root, unreferenced by anything else in the tree.
  Safe to remove; not done without asking first, since it's a repo-history
  change rather than a code change.

## ARM dead-flag elimination: built correctly, measured as a regression, reverted

Picked up the first "parked for later" item: dynarec code-generation
quality. Of the three things named as the real remaining lever (register
allocation, redundant flag computation, per-opcode instruction
sequences), register allocation turned out to be a non-issue on
inspection -- `arm_to_mips_reg[]` is a fixed 1:1 mapping, ARM r0-r14 each
permanently own a MIPS register, and there is no spill/reload traffic to
optimise. Flag computation was the real target: Thumb already has a
working dead-flag elimination pass (`thumb_dead_flag_eliminate()`,
`cpu_threaded.c`), but ARM's equivalent has been a stub since this port's
original x86 codebase --

    // For now this just sets a variable that says flags should always be
    // computed.
    #define arm_dead_flag_eliminate()  flag_status = 0xF

-- meaning every `S`-suffixed ARM data-processing instruction
(`ADDS`/`SUBS`/`CMP`/`ANDS`/...) has always paid for computing all four
condition flags, whether or not anything downstream ever reads them.

### What was built

A full classifier, `arm_flag_status()`, populating the same
`block_data[].flag_data` triple (may-modify / must-modify / uses) the
existing, working Thumb liveness pass already consumes -- so the backward
liveness algorithm itself needed no changes, just real per-instruction
input for ARM instead of a constant. Built via two research passes over
`cpu_threaded.c`'s actual `translate_arm_instruction()` dispatch (not the
ARM ISA reference in the abstract, since what matters is matching what
the existing emitter *actually* generates) covering every one of its
`(opcode>>20)&0xFF` cases, cross-checked line by line against:

  - the logical/arithmetic split (`AND,EOR,TST,TEQ,ORR,MOV,BIC,MVN` vs
    `SUB,RSB,ADD,ADC,SBC,RSC,CMP,CMN`), since only the logical group's C
    flag comes from the barrel shifter (and thus can be "maybe" rather
    than "always" set) while the arithmetic group's N/Z/C/V all come
    unconditionally from the ALU adder;
  - `ADC`/`SBC`/`RSC` additionally needing C as an input, not just an
    output;
  - the shift-carry determinism rules (register-specified shift amount:
    always "maybe"; `LSL#0`: a true no-op, C untouched; any other
    immediate shift, including the `LSR#32`/`ASR#32`/`RRX`
    encoded-zero-means-32 special cases: deterministic);
  - a real, pre-existing gap found along the way: `TST`/`TEQ` with an
    immediate operand never actually get C from the rotation in this
    codebase at all (`generate_op_tst_imm`/`teq_imm` route through the
    plain `_ands`/`_eors` imm helpers, which have no carry-out code path)
    -- not something this change introduced, and harmless to mark
    conservatively regardless;
  - every instruction class capable of writing PC (data processing with
    `Rd==15`, `LDR`/halfword loads, `LDM` with PC in the register list,
    `B`/`BL`/`BX`, `SWI`), which needs every flag conservatively marked
    "needed" per the same rule the liveness algorithm's own comment
    states: "for any instruction that changes PC ... it is unknown what
    flags will be needed after it arrives at its destination";
  - ARM's own biggest wrinkle Thumb never had to deal with: almost any
    instruction can be conditionally executed based on the *current*
    flags, so anything with `condition != AL` conservatively needs all
    four flags valid on entry, independent of what the instruction itself
    computes.

Caught and fixed one real bug in my own derivation before it ever reached
a build: an early draft's logical-vs-arithmetic split used a numeric
range check (`op_alu<8 || op_alu>=0xC`) that silently misrouted `TST`/`TEQ`
(ALU-op values 8 and 9) into the arithmetic group. Replaced with an
explicit bitmask membership test (`(0xF303 >> op_alu) & 1`) verified
against the full 16-value ALU-op enumeration by hand before writing any
of it into the actual file.

### Correctness: verified, not assumed

This is the highest-stakes change made in this whole line of work --
unlike a rendering bug, a wrong flag elimination silently corrupts GBA
*game logic*, not just pixels, and might not surface for many frames.
Built, then run on ares for 40 PROF windows (2400 frames) from the
overworld savestate, comparing every available signal against this
project's already-established, repeatedly-confirmed baseline values for
this exact scene:

  - RDP canary: **100% of screen, 1257 tiles, 21 TMEM slices, 27 palette
    groups, in every single one of 32 steady windows** -- bit-for-bit
    identical to the pre-change baseline, no drift.
  - `5 HLE SWI dispatches/frame, 1 m4a code-change flush` -- identical to
    baseline in every window.
  - `4 total ROM translation cache flushes since boot` -- identical.
  - RSP selftest: PASS, 0 mismatches (unrelated to this change, but
    reconfirms nothing else broke).

No crash, no hang, no divergence on any counter this project has
instrumented. Correct, as far as this savestate and this tooling can
show it.

### Performance: a real regression, controlled A/B

First look (uncontrolled, comparing against an earlier turn's numbers)
suggested a small win. Rerun as a proper controlled A/B instead --
`git stash` the change, rebuild, bench the *exact* same 40-window
overworld scene, compare:

| build | frame time | FPS |
|---|---|---|
| baseline (flags always computed) | 24.0 ms/f | 41.7 |
| ARM dead-flag elimination | 25.0 ms/f | 40.0 |

A real, reproducible **regression of ~1 ms/frame (~4%)**, not noise --
same sample size, same scene, only the classifier differing. Likely
cause: ARM compilers lean on conditional execution (if-conversion) far
more heavily than Thumb ever could, and this classifier's conservative
"conditionally executed -> needs all four flags" rule (necessary for
correctness, not optional) means a large fraction of real ARM
data-processing instructions end up requiring every flag anyway, gaining
nothing from the classification while still paying for it: the
classifier's own translate-time cost, and ~22 KB of new generated-code
size in `cpu_threaded.o` shifting where other, unrelated hot code lands
in the VR4300's direct-mapped 16 KB I-cache -- the same layout-sensitivity
this project has hit and documented before (`docs/CACHE_PROFILING.md`,
the `N64_STUBPAD` numbers earlier in this file).

**Reverted.** The diagnosis (register allocation is a non-issue, flag
computation is the real ARM gap, Thumb's machinery is a proven, reusable
template) stands and is recorded here for whoever revisits this; the
actual classifier code was not kept in the tree given it is a net loss on
the one workload available to test it. A full working copy (the exact
diff that produced the numbers above) is not part of this commit.
