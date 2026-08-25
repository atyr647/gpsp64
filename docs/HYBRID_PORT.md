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
check that game logic was not disturbed.

## The harness had been measuring the wrong thing

Chasing this exposed a problem with every performance number taken before
it.  ares emulates the N64 slowly enough that a 400-second run reaches
only ~180 emulated GBA frames — BIOS decompression and the Game Freak
logo.  Boot runs roughly *thirty times* the instructions per frame of
gameplay and the sound driver has not started, so a boot-phase benchmark
is not measuring the workload the port exists for.

`native/bench.sh` can now write a savestate (`BENCH_SAVESTATE=`) and
`native/ares_bench.sh` can boot the ROM from one (`BENCH_STATE=`), which
puts the ares window in gameplay from frame zero:

```
BENCH_SAVESTATE=/tmp/boot.sav BENCH_WARMUP=1500 native/bench.sh mkstate
BENCH_STATE=/tmp/boot.sav native/ares_bench.sh mylabel
```

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

## The unfinished half

Skipping the mixer is correct only while audio is dead.  The proper
version is to implement `SoundMainRAM` in C against the same `SoundInfo`
layout: the same PCM in the same buffer, at roughly a twentieth of the
emulated cost, which would restore audio *and* keep the speed.  The hook
point, the signature match and the struct layout established here are
what that needs; `m4a_hle.c` is written so that the body can be filled in
without moving anything else.
