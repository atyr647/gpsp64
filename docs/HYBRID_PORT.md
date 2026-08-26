# Running game code natively instead of emulating it

The idea: find where the frame time actually goes at the level of the
*game's* own functions, then replace the worst offenders with native N64
code, using a decompilation as the specification.  A hybrid of an
emulator and a port.

This document records what the profiling found, what turned out to be
worth substituting, and what did not.

## How to profile at game-function granularity

The emulator-subsystem breakdown (CPU / PPU / blit) says nothing about
which *game* code is expensive.  Three counters in `cpu.cc`, all behind
`-DPROFILE_AOT -DPROFILE_REGIONS`, give that view:

| counter | what it answers |
| --- | --- |
| `prof_region_arm` / `prof_region_thumb` | which GBA memory region executes — ROM, IWRAM, EWRAM, BIOS |
| `prof_page_hist` | which 4 KB ROM page, so it can be matched against a decomp map |
| `prof_iwram_hist` | which 64-byte block of IWRAM, for code the game copies to RAM |
| `prof_iwram_entry` | the PC that IWRAM is entered at — the hook point |
| `prof_swi_hist` | which BIOS calls the game makes |

Run it on the host, not on the N64: the *instruction stream* is identical
either way, and the host harness reaches gameplay in seconds instead of
an hour.

```
BENCH_FRAMES=900 BENCH_WARMUP=1500 native/bench.sh prof -DPROFILE_REGIONS
```

## What it found

Pokemon Emerald, in gameplay, 900 frames:

```
  interpreted insns          29008 /frame
    ARM                      19630 /frame  (68%)
    Thumb                     9378 /frame  (32%)

  interpreted instructions by region:
    00 BIOS         75/frame    0.3%   ARM        75  Thumb         0
    03 IWRAM     19820/frame   68.3%   ARM     19553  Thumb       267
    08 ROM0       9113/frame   31.4%   ARM         2  Thumb      9112

  IWRAM entry points (PC arriving from outside IWRAM):
    0x03001aa8       1.0 entries/frame

  hottest IWRAM 64-byte blocks (19820 insns/frame total):
    0x03001dc0      9416/frame   47.5%
    0x03001ac0      3361/frame   17.0%
    0x03001e00      2964/frame   15.0%
```

Two thirds of every emulated instruction belongs to **one routine,
entered exactly once per frame**, occupying about 1.3 KB of IWRAM.

It is not game logic.  It is `SoundMainRAM` from Nintendo's m4a sound
driver — the software PCM mixer that every m4a game (which is to say
almost every GBA game) copies into IWRAM at boot.  The identification is
not a guess:

- `0x03007FF0` holds `SOUND_INFO_PTR`; the struct it points at begins
  with m4a's ident `0x68736D53`, and reports `pcmSamplesPerVBlank = 224`
  and `pcmFreq = 13379`.
- The inner loop reads the right-channel accumulator at `[r5, #1584]`.
  1584 is exactly `PCM_DMA_BUF_SIZE`.
- The body is m4a's 8-bit sample fetch with linear interpolation
  (`ldrsb` / `sub` / accumulate) unrolled four ways, plus the reverb pass
  that sums four channels and shifts right by 9.

The routine's entire output is 2 x 1584 bytes of PCM in the DMA buffer.
This port never calls `n64_audio_render_frame()`, so those samples are
computed and thrown away.  **Two thirds of the emulated CPU was producing
silence.**

## What was done about it

`n64/m4a_hle.c` finds the mixer by a code signature and patches its entry
to branch straight to its own epilogue, so the caller sees an ordinary
return.  Only the PCM mixing is skipped; `MPlayMain` — the sequencer that
starts and stops notes and maintains the state game code polls through
`IsFanfareTaskInactive` and friends — is in ROM and still runs.

Effect on the interpreter (native harness, from the same savestate):

```
              interpreted insns/frame     ARM      Thumb
  baseline               29008           19630      9378
  mixer skipped           9351             180      9171   (-67.8%)
```

ROM-side execution is unchanged (9113 -> 9161 insns/frame), which is the
check that game logic was not disturbed.  A 6000-frame soak confirms the
game keeps running.

## The bug this uncovered

The first attempt to measure the change on the N64 said it bought
nothing, and the reason turned out to matter more than the change.

`n64/aot_hle.c` resolved GBA addresses like this:

```c
u32 aot_read32(u32 addr) {
    u8 *map = memory_map_read[addr >> 15];
    if (!map) return 0;
```

`memory_map_read` is NULL both for genuinely unmapped regions and for
cart pages that are merely not resident.  This port ships
`ROM_BUFFER_SIZE=2` -- 2 MB of a 16 MB cart in RAM, the rest paged in on
demand -- so every AOT-translated read of a paged-out page was silently
answering zero.  With the whole cart resident the entry is never NULL,
so the bug cannot appear in a full-buffer build, which is why no
host-side test had ever seen it.

It cost the game its sound.  m4a never finished initialising, the
`MusicPlayerInfo` structures stayed zeroed, and no music played for the
rest of the run -- which is also why the mixer was not showing up as
expensive on the N64.

Found with a differential trace (`PCTRACE=`): two builds that should be
running the same game must emit the same stream of (pc, register-hash)
pairs, and the first pair that differs is the instruction that went
wrong.  Entries are emitted per *interpreted* instruction, so an AOT
block appears as a single entry -- which is what pointed at the AOT
reader rather than at the interpreter.  `ROMCHECK=1` checksums the whole
cart through `read_memory8/16/32` and the mirrors; that path verified
identical across buffer sizes, which ruled the interpreter out.

After the fix, `ROM_BUFFER_SIZE=2` and a fully resident build produce
identical frame-by-frame instruction counts over 1200 frames.

## The harness had been measuring the wrong thing

Chasing this exposed a second problem with every performance number
taken before it.  ares emulates the N64 slowly enough that a 400-second
run reaches only ~180 emulated GBA frames -- BIOS decompression and the
Game Freak logo.  Boot runs roughly *thirty times* the instructions per
frame of gameplay and the sound driver has not started, so a boot-phase
benchmark is not measuring the workload the port exists for.  The
"35.71 fps" quoted before this was a boot number; gameplay was 12.

`native/bench.sh` can now write a savestate (`BENCH_SAVESTATE=`) and
`native/ares_bench.sh` can boot the ROM from one (`BENCH_STATE=`), which
puts the ares window in gameplay from frame zero:

```
BENCH_SAVESTATE=/tmp/boot.sav BENCH_WARMUP=1500 native/bench.sh mkstate
BENCH_STATE=/tmp/boot.sav native/ares_bench.sh mylabel
```

Capture the state with the same `ROM_BUFFER_SIZE` as the build under
test, or it will be a state that build could never reach.

## What it is worth

ares, 8 windows of 60 frames each, booted from a gameplay savestate,
`ROM_BUFFER_SIZE=2` as shipped:

```
                     ms/f    fps     CPU  PPU  Blt   mix
  mixer emulated     82.5   12.12    72%  28%   3%   ARM 67% / Thumb 32%
  mixer skipped      58.0   17.24    58%  42%   5%   ARM  2% / Thumb 98%
```

**+42.2% frame rate**, frame time down 29.7%.  Every window in both runs
is stable, and the instruction mix confirms what was removed.

Note what this does to the balance of the port: the PPU was 28% of the
frame and is now 42%.  Rendering, not the CPU, is where the next work
belongs.

## A cheaper lever found the same way

The host-side PC profiler added for this work (`GPSP_PCPROF=<n>` in the
patched ares, which samples the VR4300 PC every n instructions) paid for
itself immediately.  With the mixer dealt with, it reported:

```
  33.6%  execute_arm                          <- the interpreter
  17.9%  render_w_effects
  13.5%  render_scanline_text<INDXCOLOR,base>
   7.0%  render_scanline_text<INDXCOLOR,!base>
   5.0%  update_gba
   3.4%  rsp_wait
   2.0%  sound_timer / render_gbc_sound / update_scanline
```

Rendering is ~43%, the interpreter ~37%.  But only one of those is
*pathological*: 33.6% of a 63 ms frame is 21 ms for 9,560 interpreted
instructions, which is over 200 VR4300 cycles each where an interpreter
should manage 20-40.  Rendering at ~60 cycles/pixel for four layers plus
effects is merely a lot of work.

The obvious explanation was code size -- `execute_arm` is 79 KB against a
16 KB direct-mapped I-cache -- and it is wrong.  Sweeping the
optimisation level from a gameplay savestate:

```
                              ms/f    fps     .text      execute_arm
  -O3 everywhere              61.0   16.39   1,436,828     79,076
  -O1 on cpu.cc               62.0   16.13   1,456,796
  -Os on cpu.cc               60.5   16.53   1,428,716     66,288
  -O2 on cpu.cc               56.0   17.86   1,431,996     74,376
  -O2 everywhere              55.0   18.18   1,082,892
```

`-Os` produces the smallest `execute_arm` and is the second slowest, so
size is not the mechanism.  Spills per instruction are lowest at `-Os`
too (4.6% vs 5.0% at `-O2` vs 5.5% at `-O3`), and the stack frame is the
same size at `-O2` and `-O3`, so register pressure does not predict it
either.  The mechanism remains unexplained.

The result stands on the measurement rather than the story: **-O2
everywhere is +10.9% and a quarter smaller ROM**, stable across every
window, with all startup self-tests still passing.  It is now the
default.  Do not reason your way back to `-O3`; measure.

## Benchmark states, and why there are two

Two savestates, both produced by `native/bench.sh` and both needed,
because they exercise different halves of the emulator:

```
BENCH_SAVESTATE=/tmp/intro.sav BENCH_WARMUP=1500 \
  BENCH_INPUT=mash native/bench.sh mk

BENCH_SAVESTATE=/tmp/overworld.sav BENCH_LOADSTATE=/tmp/intro.sav \
  BENCH_WARMUP=21600 BENCH_INPUT=walk native/bench.sh mk
```

  * **intro** -- the Prof. Birch cutscene.  Music playing, so the m4a
    mixer is fully loaded; but a brightness effect on 100% of scanlines,
    which forces every scanline down the INDXCOLOR + merge path.
  * **overworld** -- the player's house in Littleroot, walking.  No
    colour effect at all, so the plain FULLCOLOR path; but the sound
    driver is idle here, so it says nothing about the mixer.

Reaching the second one needs `--input walk`; Start/A mashing alone parks
the game on a dialogue box, and every state past ~7200 frames was
byte-identical.  Capture a state with the same `ROM_BUFFER_SIZE` as the
build under test, or it is a state that build could never reach.

Measured on the same build (`-O2`, native mixer, no audio output):

```
                   ms/f    fps    interp  ppu  event  snd  aot  blit
  intro            55.0   18.18
  overworld        67.0   14.93     41%   43%   10%    5%   5%    4%
```

Actual gameplay is **slower** than the cutscene, not faster.  A guess to
the contrary -- that the overworld would render more cheaply because it
skips the merge pass -- was wrong: the overworld PPU costs 28.8 ms
against the cutscene's 25.3, because it draws more layers and more
sprites than the merge pass ever saved.  It is also doing more CPU work
(12,811 interpreted instructions per frame against 9,854).

The number that matters for the renderer: in the overworld the RSP BG
fast path is applicable on **100%** of scanlines.

## Inspecting the interpreter anomaly

The interpreter costs ~197 VR4300 cycles per interpreted GBA instruction,
where a decent ARM interpreter manages 20-50.  Three things were checked.

**It is not memory stalls.**  D-cache misses run ~14,400/frame plus
~5,200 writebacks; at ares's 40 cycles each that is ~8.3 ms of a 70 ms
frame, about 12%.  (ares's I-cache counters read zero because it
recompiles and never runs that accounting path -- they measure nothing
and are not evidence either way.)

**It is the shared per-instruction path, not the opcode handlers.**
Bucketing PC samples by offset *within* execute_arm, rather than treating
the function as one blob:

```
  +3e0..+430   AOT dispatch check + idle-target compares   1033 samples  35%
  +494..+560   fetch, byte-swap, dispatch, cycle acct,      827          28%
               idle detector
  +6a8,+728    idle detector slow path / region change      227           8%
  ------------------------------------------------------------------------
  shared path                                              2087          72%
  actual opcode handlers                                   ~830          28%
```

Seventy-two percent of interpreter time is bookkeeping around the GBA
instruction rather than executing it.

**The largest piece of that bookkeeping is justified.**  The AOT dispatch
check runs on every interpreted instruction, so it looked like a straight
loss.  It is not:

```
  AOT on       69.0 ms/f   14.49 fps    828K insns/window
  AOT off      75.0 ms/f   13.33 fps   1426K insns/window
  table off    76.0 ms/f   13.16 fps   1426K insns/window
```

AOT is worth +8.7%.  The "5.6% coverage" figure that made it look
worthless is coverage of emulated *GBA cycles*; what the check actually
buys is interpreted *instructions avoided*, and that is 598K of 1426K --
**42%**.  It covers instructions that are numerous but cheap in emulated
cycles, which is exactly the population an interpreter is worst at.  Two
different denominators, and picking the wrong one inverts the conclusion.

Note also that this was previously measured as "neutral" and shelved.
That measurement was taken during boot.

What remains in the shared path looks redundant: reg[REG_PC] loaded
twice and stored once per instruction, idle_loop_target_pc reloaded from
$gp twice (it is set once by load_gamepak and never moves, but the
compiler cannot prove that across the opcode handlers), and
prof_arm_insns/prof_thumb_insns -- a global read-modify-write on every
instruction, compiled in unconditionally, feeding nothing but the PROF
line.

**Removing the reloads makes it slower.**  Hoisting idle_loop_target_pc
and its page into locals, exactly as _cheat_hook already is:

```
  baseline (no hoist, counters on)   69.0 ms/f   14.49 fps
  hoist only                         72.0 ms/f   13.89 fps   -4.3%
  hoist + counters off               69.5 ms/f   14.39 fps   -0.7%
```

Two more values live across the 256-way dispatch cost more in spills than
the two reloads they removed.  That is worth knowing for its own sake:
the interpreter is **register-pressure-bound at the dispatch**, which is
also the likeliest reason -O3 loses to -O2 on this file -- -O3's extra
passes lengthen live ranges in the one function that can least afford it.

**Removing the profiling counters is not a win either.**  prof_arm_insns
and prof_thumb_insns are a global read-modify-write on every instruction:

```
  counters on    69.0 ms/f   14.49 fps
  counters off   69.5 ms/f   14.39 fps
```

Nothing.  (An earlier reading of 2.5 ms came from measuring on top of the
hoist regression.)  That a global RMW per instruction is free says the
loop is not issue-bound -- it is waiting on something else, which fits
spilling rather than instruction count.

So the obvious micro-optimisations in this path are all unavailable, and
the structural answer is to shrink what is live across the dispatch
rather than trim around it.  The concrete candidate: execute_arm holds
*both* the ARM and the Thumb interpreter in one function under one
register allocation, and 97% of executed instructions are Thumb.  The ARM
half inflates pressure on a path that almost never runs it.  Splitting
them into separate functions is the change the evidence actually points
at.  (_cheat_hook is cached in that function for the same reason the
hoist seemed sensible, and predates the dispatch growing; it may no
longer pay for itself either -- worth re-measuring in the same pass.)

## What else is worth substituting

With the mixer gone, the remaining interpreted work is ~9,200 Thumb
instructions per frame of genuine game logic, spread thinly:

```
  0x08006000  4488/frame   49%    0x0806f000   275/frame   3.0%
  0x08000000  2233/frame   24%    0x08002000   214/frame   2.4%
  0x08004000   955/frame   10%    0x08007000   168/frame   1.9%
```

There is no second `SoundMainRAM` here — no single routine worth a native
rewrite, and each one substituted would have to reproduce its writes to
GBA memory exactly.  BIOS SWIs are similarly small: one `CpuSet` and one
`ObjAffineSet` per frame.

The lever that remains is the PPU, which after this change is the large
majority of the frame.  See `docs/RSP_SCANLINE_PLAN.md`.

## The native mixer

Skipping the mixer is only correct while audio is dead, so the finished
version replaces it instead.  Not all of it: the per-64-byte-block
profile is extremely lopsided.

```
  0x03001dc0  9416/frame  47.5%    resampling mix loop
  0x03001ac0  3361/frame  17.0%    reverb loop
  0x03001e00  2964/frame  15.0%    resampling mix loop, tail
  0x03001cc0  1498/frame   7.6%    unity-rate mix loop
  0x03001c80   961/frame   5.3%    unity-rate mix loop, second variant
  0x03001d80   926/frame   4.7%
```

Four loops are 97% of the cost.  Everything else -- the envelope state
machine, the compressed and reverse-playback paths, the loop-point
handling when a sample runs out -- is under 1% and is left interpreted,
so it stays correct without being reimplemented.  That is the whole
design: reimplement only what is hot, and let the driver keep its own
control flow.

Each hot loop is replaced, in the driver's IWRAM copy, by a single ARM
`SWI` in the 0xF0 block that the BIOS leaves unused.  gpSP already routes
SWIs through `bios_hle_swi`, so no interpreter change was needed: the
handler runs the loop natively, writes back the registers the loop would
have left, and sets the PC to wherever that loop exits.  A loop that
meets a case it does not model -- an unresolvable address, a source
sample running out mid-group -- hands the exact register state back and
returns control to the interpreter at the instruction that handles it.
Emulated time is charged for every instruction the loop would have
executed, so the game's timing does not shift; only host time is saved.

The loops themselves are the delicate part.  m4a packs four output
samples into one 32-bit accumulator word, rotating each new sample into
the top byte and clearing bits 16-23 so a negative product borrows into
the discarded bit 32 rather than into its neighbour, and it counts the
four samples in the *unused top two bits of the write pointer* -- `adds
r5, r5, #0x40000000` carries out on exactly the fourth one.  The
resampler keeps a 9.23 fixed-point position and interpolates between two
8-bit samples.  All of that is reproduced as written.

### Verifying it without being able to hear it

There is no audio in ares and no console here, so the mixer is checked by
differential execution instead: the driver's entire output -- the PCM
buffer and all channel state -- lives in IWRAM, so running the same
savestate under an interpreted build and a native build and diffing IWRAM
is a complete check.

Over 1800 frames (30 seconds of gameplay) the two builds' IWRAM differs
in exactly twelve bytes: the four patched instruction words. Nothing
else.

### What it costs

```
                            interp insns/frame     of which IWRAM
  mixer interpreted               28591              19620  (68.6%)
  mixer's hot loops native         9854                667  ( 6.8%)
```

The mixer went from 19,620 interpreted instructions per frame to 667 --
96.6% less -- for identical output.  In ares, from the same gameplay
savestate:

```
                                     ms/f    fps     CPU  PPU
  mixer interpreted                  82.5   12.12    72%  28%
  mixer skipped entirely             58.0   17.24    58%  42%
  mixer's hot loops native           61.0   16.39    60%  40%
  ...and audio actually played       75.5   13.25    68%  32%
```

Running the game's whole sound driver now costs **3 ms/frame instead of
24.5** -- 5% of the frame where it used to be 30%.  `N64_M4A_NATIVE` is
the default; `N64_M4A_STUB` remains as the skip-it variant for anyone who
wants the last 5%.

Playing the result is a separate 15.5 ms/frame, and none of it is the
mixer.  Building with `-DN64_AUDIO_NOPUSH`, so the samples are still read
and resampled but never handed to the AI, measures 61.0 ms/f -- identical
to producing no audio at all.  The whole cost is libdragon's
`audio_push`, so gpSP's resampling is not implicated either.

That makes it the next thing worth attacking rather than a reason not to
have sound.  Two things in libdragon's `audio.c` are worth checking
first, though neither is confirmed: `audio_push`'s loop condition is
`(blocking || dst || audio_can_write())`, so a call that arrives with a
partially-filled buffer left over from last time skips the
`audio_can_write()` guard entirely; and `audio_write_begin()` busy-waits
on `while (buf_full & (1<<next))` with interrupts toggling inside the
loop.  This port pushes 369 samples per emulated frame against a buffer
length that does not divide it, so a leftover partial buffer is the
normal case.

`-DN64_AUDIO_OUT` turns sound on.

