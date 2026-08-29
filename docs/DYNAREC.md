# The dynarec: why it is slow, and what that number actually means

`Makefile.n64` records the dynarec as **2.6× slower than the interpreter**
— 20.5 N64 cycles per GBA cycle against 7.9, with 5.56 being real time —
and that figure has been the reason not to invest in it. It is not a
measurement of the dynarec running the game.

## What it actually does

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

## Where to look next

The hang is in translated code, not in the lookup or translation
machinery (all three counters freeze). So: which emit path does Emerald's
Thumb idle loop at 0x080008C6..0x080008CE take, and why does that path
not emit a `reg_cycles` check? `generate_branch_cycle_update` and
`generate_indirect_branch_cycle_update` are the candidates.

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
