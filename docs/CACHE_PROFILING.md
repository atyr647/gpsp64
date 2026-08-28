# Measuring cache behaviour on this port

Most of this port's remaining cost is not instructions. It is cache
stalls, and for a long time that was suspected but not measured — the
Makefile carried an explicit unsolved note saying so:

> a host-side PC profile puts 33.6% of the frame inside `execute_arm` for
> only ~9,560 interpreted instructions — over 200 VR4300 cycles each,
> where an interpreter should manage 20-40. […] The measurement is solid
> and repeatable; the mechanism is not established. Do not "fix" this by
> reasoning about it — measure.

It can be measured. ares models both VR4300 caches properly and this
build already reports them.

## What ares models

| | |
| --- | --- |
| I-cache | 16 KB, direct-mapped, 32-byte lines, **48 cycles** to fill |
| D-cache | 8 KB, direct-mapped, 16-byte lines, **40 cycles** to fill, 40 more for a dirty writeback |

Both are charged in the recompiled path, not just the interpreter — the
recompiler emits the tag check inline and calls `CPU::icacheFillLine` on
a miss (`ares/n64/cpu/recompiler.cpp`, near "Keep icache tag/coherency
checks in sync with interpreter behavior"). The older claim that "ares's
I-cache counters report nothing because it recompiles and never runs the
accounting path" is half right: only the *hit* counter is gated on
homebrew mode, and the timing was always modelled.

Because the D-cache is direct-mapped with a 9-bit index over bits 12:4,
**any two hot addresses 8 KB apart destroy each other.** That is the
single most useful fact for interpreting the histograms below.

## What it prints, for free

Every `native/ares_bench.sh` run already emits these on stderr, every 600
VI frames, with no ROM support and no build flag:

```
IMISS +3849062 hits +59182488 (6% miss)
DMISS total=23486559 (+2578545) wb=10557014 | top: 21:479012(18%) 17:326369(12%) ...
DMISSFINE 8017d800:152458(5%) 807ff400:121433(4%) 80303800:110525(4%) ...
DMISSPC 801363ac:1154370 8013642c:161037 80137308:143011 ...
```

- `DMISS … top:` buckets misses by 64 KB of physical address.
- `DMISSFINE` does the same at 1 KB, which resolves to individual objects
  in the link map — 64 KB is useless here, because the small-data window
  around `_gp` is itself 64 KB and conflicts with itself eight times over.
- `DMISSPC` attributes each miss to the instruction that caused it. Feed
  those through `mips64-elf-addr2line -e gpsp.elf -f -C`.

To turn `DMISSFINE` addresses into names, dump the symbols with
`mips64-elf-nm -S --defined-only` and bracket each address; LTO merges a
lot of BSS so some regions have no symbol and have to be read off the
`.map` instead.

## What it says today

At 37 ms/frame on the overworld, with the RDP renderer:

| | per frame | cost |
| --- | --- | --- |
| D-cache misses | ~9,550 | ~5.7 ms |
| I-cache fills | (count is partial, see below) | the rest of ~35% |

Roughly a third of the frame is cache-miss stalls. Where the D-cache
misses land, by 1 KB bucket: the stack 7%, the interpreter's rodata and
jump tables 7%, `memory_map_read` and `reg` 5%, the EWRAM tail 4%, the
RDP renderer's own draw list and sort arrays 7%, the IWRAM tail 2%.

**The I-cache miss count is an undercount.** The recompiler charges the
48 cycles but does not increment `icacheMisses` on its fill path, so the
number only reflects misses the interpreter and `jitFetch` saw. Treat
`IMISS` as a comparative signal between builds, not an absolute.

## Why this matters more than it sounds

It explains a run of results that otherwise look like noise. Every one of
these removed real work from the hot loop and changed nothing:

- expanding AOT coverage (interpreted instructions −25%, exactly as
  predicted);
- merging the two halves of each scanline (−228 of 672 timeslice yields);
- deleting the cheat-hook compare (5.6% of `execute_arm`'s samples);
- giving the runtime idle detector a sequential fast path.

None of them touched the cost, because the cost is not in the instruction
stream. It also explains why `-O2` beat `-O3` before the renderer moved
to the RDP and `-O3` beats `-O2` after: with the CPU rasteriser gone from
the I-cache, the interpreter can afford bigger code.

## The two things ares does not charge for

Both were real blind spots — decisions in this port turned on them and
could not be settled — and both are now optional, off by default, so that
every measurement taken before they existed stays comparable.

### RDP fill time

`RDP::main()` is, in full:

```cpp
while(Thread::clock < 0) { step(clocks); command.clock += clocks / 3; }
```

The RDP thread advances its clock in fixed chunks with no relation to the
work queued. A renderer can enqueue any amount of drawing and the frame
time will not move. That matters here because the GBA background renderer
issues ~1,200 textured rectangles a frame and nothing in ares could say
whether that fits on a 62.5 MHz part.

The fix is not to emulate RDP timing — it is to **count the work**. The
Vulkan command walk in `ares/n64/vulkan/vulkan.cpp` already decodes every
command and has the length table, so a passive pass there accumulates
primitives, covered pixels by cycle type, and TMEM load words. Reported
every 600 VI frames as `RDPWORK`:

```
RDPWORK syncs=536 prims=326412 tris=0 px1=20868512 px2=0 pxfast=10184268
        tmemwords=1492224 | per sync: 608 prims 57934 px -> ~64737 RDP cyc = 1.03 ms
```

The counts are exact. The cycle figure is an **estimate from a stated
model** — 1 cycle per pixel in 1-cycle mode, 2 in 2-cycle, 4 pixels per
cycle in copy/fill, 1 cycle per 64-bit TMEM word, and
`GPSP_RDP_SETUP` (default 30) cycles per primitive — printed alongside
the raw counts so it can be recomputed with different constants without
re-running anything. Real hardware adds span setup, TMEM conflicts and
RDRAM contention, so read it as a floor.

Divide by `syncs`, not by frames: how many `SYNC_FULL`s a frame costs is
the ROM's business. This port issues two per frame, so **the RDP
renderer costs the RDP about 2.06 ms per frame** — 1,209 primitives and
115,010 pixels. Against a 34 ms frame, or even a 16.7 ms one, the RDP is
nowhere near the bottleneck.

`GPSP_RDP_CHARGE=1` additionally advances the RDP thread's clock by the
estimate before raising the DP interrupt, so anything waiting on the RDP
waits for a plausible duration instead of none.  Measured: it costs 1.0
ms of a 35 ms frame, not the full 2.06 ms of estimated RDP work, because
the RDP runs asynchronously and the CPU only waits for it at
`rdpq_detach_wait`.  That is the number to quote as the hardware-faithful
frame time.

### Uncached store stalls

`MI::writeRdram` reaches `rdram.ram.write()` without stepping the thread,
so a burst of uncached stores is free in the CPU timing model. On a
VR4300 it is not: the write buffer is shallow and each access pays RDRAM
latency.

`GPSP_UNCACHED_WCOST` and `GPSP_UNCACHED_RCOST` set a per-access charge
in CPU cycles for uncached CPU accesses to RDRAM, applied in
`CPU::busWrite` / `CPU::busRead`. Both default to 0. MMIO is deliberately
not charged — it is uncached too but has its own latencies.

This immediately settled a decision that had been shelved as
unmeasurable. `-DN64_RDP_EXEC` builds the RDP command list in cached
memory instead of pushing each rectangle through rspq's uncached buffers,
removing ~44,000 uncached accesses a frame:

| cycles charged per uncached store | 0 | 20 | 40 |
| --- | --- | --- | --- |
| rspq command buffers | 37.0 ms | 39.0 ms | 42.0 ms |
| `rdpq_exec` | 36.0 ms | 36.0 ms | 36.0 ms |

Flat, as it must be. It is no worse at zero cost and strictly better at
any real cost, so it is now the default.

**Sweep the knob, do not pick a number.** The useful output is not "the
frame is X ms if a store costs 20 cycles" — it is "this change is
insensitive to the cost and the other is linear in it", which holds
whatever the true figure turns out to be.

## The scoreboard, and what it means

Knowing that a third of the frame is cache stalls did not make it easy to
fix. Attempts, in order:

| change | result |
| --- | --- |
| `-O3` on `cpu.cc` | **-4.5 ms** (found by sweeping) |
| `-Os` on `aot_generated.c` | **-2.0 ms**, and spread 5 ms -> 1 ms (found by sweeping) |
| fold `sound_buffer` to 1 KB when audio is off | -0.5 ms, at the noise floor |
| 1 KB bitmap in front of the 64 KB AOT page table | **+2.5 ms**, D-misses +44% |
| `-falign-loops=32` (matching the I-cache line) | +2.0 ms |
| `-freorder-blocks-algorithm=simple` | +4.3 ms |
| expand AOT coverage (-25% interpreted instructions) | neutral |
| merge HBlank events (-34% timeslice yields) | neutral |
| delete the cheat-hook compare | neutral |
| idle-detector fast path | neutral |

Everything found by **sweeping a knob** helped. Everything designed by
**reasoning about the cache** did not. The bitmap is the sharpest example:
the argument was that a 64 KB table indexed per instruction cannot stay
resident in 8 KB while a 1 KB bitmap answering the same question can. It
is a correct argument and the change made things much worse, because the
bitmap's lines collided with something hotter and it became the single
worst miss site in the build.

On a direct-mapped cache **size is not the variable — index conflict is**,
and index conflict is not something you can reason about from source. It
depends on where the linker put everything, which is why the same build
varies by 5 ms across `-DN64_TEXT_PAD` values and why a change that looks
locally sound can land anywhere in that range.

The practical consequence: sweep, do not deduce. And quote means over
several layouts, never a single run.

## Reading the counters from inside the ROM

`n64/emux_prof.h` reads the same counters through ares's `emux`
extension instructions (`XPROF` / `XPROFREAD` in COP0 space), which lets
a region of ROM code be measured rather than a whole frame. It needs
`--setting Developer/HomebrewMode=true`; `native/ares_bench.sh` passes it
when `ARES_HOMEBREW=true`.

**It distorts what it measures.** The instrumented build runs 58 ms
against 37 and reports four times the D-cache misses, because the inline
asm clobbers memory and ares's recompiler flushes state around the call.
It is useful for answering yes/no questions — it is how "all the uncached
RDRAM traffic is the renderer's, emulation has none" was established —
but never quote its absolute numbers. ares's own stderr reporting needs
nothing from the ROM and does not perturb the build.
