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

**Correction: the I-cache miss count is complete, not an undercount.**
This used to say the recompiler charged the 48 cycles without
incrementing `icacheMisses`, so only interpreter and `jitFetch` misses
were counted. That is wrong. The recompiler increments it with an inline
`add64` in emitted code (`recompiler.cpp`, in the I-cache refill slow
path) whenever `system.homebrewMode` is set -- which is what
`ARES_HOMEBREW=true` turns on. Proven by construction: the `IMISSHIST`
histogram added at that same inline site sums to exactly the `IMISS`
delta, every interval. `IMISS` is an absolute figure, provided homebrew
mode is on; with it off, both counters read zero.

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

## The PC profile is not a to-do list

A related trap, and the more expensive one. The direct PC profile says
`execute_arm` is 42% of the frame, `update_gba` 13%, `update_scanline`
6%. Every attempt to act on those numbers by removing work from the named
function has measured neutral:

| change | work removed | result |
| --- | --- | --- |
| expand AOT coverage | 25% of interpreted instructions | neutral |
| merge HBlank events | 228 of 672 timeslice yields | neutral |
| delete the cheat-hook compare | 5.6% of `execute_arm` samples | neutral |
| idle-detector fast path | a compare per instruction | neutral |
| skip `order_layers` on RDP rows | 160 sorts per frame | neutral |
| BG-register dirty flag | ~1,900 ioreg reads per frame | neutral |
| 8-byte draw record | 5 KB per frame of list traffic | neutral |

Seven in a row. The reason is that a third of the frame is cache stalls,
and **a sample lands on the instruction that stalls, not on the code that
caused the stall to be expensive**. `execute_arm` is 42% of samples
because it is where the CPU is standing when the memory system makes it
wait; the waiting is caused by the working set as a whole. Deleting
instructions from the function does not shrink the working set.

What has actually worked, every time, is removing **tens of kilobytes per
frame of memory traffic**, or changing code layout materially:

| change | traffic removed | result |
| --- | --- | --- |
| BG and sprites to the RDP | ~96,000 layer-pixel writes/frame | -19 ms |
| `-O3` on cpu.cc, `-Os` on the AOT | I-cache footprint | -6.5 ms |
| `rdpq_exec` | 44,000 uncached accesses/frame | -1 ms |
| fold `sound_buffer` to 1 KB | ~128 KB/frame of dead writes | -0.5 ms |

The threshold is real: 5 KB a frame is 0.2 ms, which is under the noise
floor. So the question to ask of a proposed change is not "does this run
fewer instructions" but "does this move less memory, by tens of KB".

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

## The time-weighted PC profiler

`GPSP_PCPROF` samples every N executed *instructions*. On a VR4300 that is
the wrong axis: a PC that stalls forty cycles on a D-cache miss counts
exactly the same as one that retires in one, so an instruction-weighted
profile systematically hides the stalls that dominate a frame. It also
lives in `instructionEpilogue<0>`, which ares's recompiler barely runs.

`GPSP_PCCYC=<cycles per sample>` samples from `CPU::step()`, which is
handed the real cycle cost of everything — cache fills, DMA stalls, the
lot — and runs under the recompiler. ares keeps a histogram and dumps the
top 400 PCs every `GPSP_PCCYC_DUMP` samples (default 200,000):

    GPSP_PCCYC=1009 GPSP_PCCYC_DUMP=300000 tools/jit-savestate-run.sh <label>
    tools/pccyc.py <scratch>/gpsp.elf bench-results/<label>.jit.txt <watermark>

1009 is prime, so the sampler does not beat against periodic guest code.
The watermark is the byte count printed on the `JITSTUB scanning` line;
without it the generated stubs and translated code are reported together.

`tools/pccyc.py` also fixes an attribution trap. The dynarec's output lives
in `rom_translation_cache` / `ram_translation_cache`, which are declared
with `.space` in a nobits section and therefore carry **no ELF size** — nm
reports them at zero bytes, so every sample inside them is filed under
whatever symbol happens to precede them. That is how `ewram_raw +0x70d6c`
appeared in an earlier debugging session as if it meant something, and how
21.8% of the first profile taken with this tool arrived labelled
`__text_end`. The script takes each such object's extent as running to the
next symbol, and splits the stub area from translated code.

### What it said the first time it was pointed at the default build

    16.8%  dynarec stubs (mips_stub.S)      <- generated memory-access stubs
    13.9%  n64_rdpbg_flush
     9.8%  update_gba
     8.8%  update_scanline
     8.2%  n64_rdpbg_frame_end
     6.6%  render_gbc_sound
     5.4%  sound_timer
     4.8%  translated code (ROM blocks)
     2.5%  bios_hle_swi
     2.5%  mips_indirect_branch_dual

This overturned the conclusion reached the day before by subtraction —
that roughly 40% of a frame was translated code executing. It is 4.8%.
The dynarec's real cost is the generated **memory-access stubs**: every GBA
load and store leaves translated code for a stub that decodes the region,
masks the address and byte-swaps.

The lesson is worth keeping separate from the numbers. Three hypotheses in
a row — per-yield overhead, block lookups, translated code — were each
reached by attributing a residual, and each over-predicted by roughly an
order of magnitude. A residual is not a measurement. Point this profiler
at the question instead.

## Attributing samples inside the stub area

`-DN64_STUBMAP` makes the ROM print the address of every generated memory
handler (`tmemld[11][16]`, `tmemst[4][16]`) as `STUBMAP <addr> <op>.<region>`.
`tools/pccyc.py` reads those and labels a sample in the stub area with the
operation that owns it, so "16.8% of the frame is in the stubs" becomes:

    memory-access stubs, 13.3% of the frame
      by direction:  store 8.4%   load 3.2%
      by region:     iwram 9.7%   ewram 1.9%
      top:  st_u32.iwram 2.7%, ld_u8.iwram 1.4%, st_u16.iwram 1.4%,
            st_u8.iwram 1.2%, st_u32a.iwram 1.2%

## The SMC tag aliases its own data in the D-cache

**Historical: this described the bug this section's fix (below) has
since shipped.** The 2.6x/14.5% figures below are pre-fix; they are not
this build's current cost, kept only as the diagnosis that motivated
`GBA_SMC_SKEW`.

Stores cost 2.6x what loads do, and the reason is a cache-index collision
that is exact rather than incidental.

Every GBA store to IWRAM or EWRAM runs an SMC check: load the tag byte for
the address, and if it is non-zero the store landed on translated code.
gpSP places that tag block exactly one region-size away from the data --
32 KB for IWRAM, 256 KB for EWRAM. The VR4300's D-cache is 8 KB,
direct-mapped, 16-byte lines, so the index is address bits 12:4, and **both
of those separations are exact multiples of 8 KB**:

    iwram: data 0x80244d00 -> line 208
           tag  0x8023cd00 -> line 208     separation 0x8000 = 4 x 8 KB
    ewram: data 0x80264d00 -> line 208
           tag  0x802a4d00 -> line 208     separation 0x40000 = 32 x 8 KB

The tag and its own data are permanently assigned to the same cache line.
Every store misses twice: the tag load evicts the data line, the store
evicts the tag line back.

Confirmed, not just computed. The ares D-cache miss profiler's top PCs
include an adjacent pair with **identical** counts:

    DMISSPC ... 802eae44:128423 802eae4c:128423 ...

two instructions eight bytes apart missing exactly as often as each other
-- the tag load and the data store, ping-ponging. Symbolised, that pair is
`stub st_u32.iwram`, and the memory stubs together are 14.5% of all
sampled D-cache misses.

### Update: the first fix shipped, the second is a closed dead end

**Move the tag block: done.** `mips/mips_emit.h`'s `emit_pmemst_stub` now
subtracts `GBA_SMC_SKEW` (`gba_memory.h`) from the tag address, so the
separation from its data is no longer a multiple of 8 KB -- exactly the
fix this section originally proposed. Undocumented until now; this file
just hadn't caught up with the code. `iwram_raw`/`ewram_raw` were grown
and every reference site updated to make room.

**Skip the tag load entirely via `iwram_code_max`: measured not worth
it, before ever being built.** `emit_pmemst_stub` already carries a
validation-only `-DN64_NOSMC` flag that removes the tag check completely
-- a strictly *larger* win than the conservative `iwram_code_max` bound
could ever reach, since it drops the load unconditionally instead of only
when provably safe. Under `-DN64_STUBPAD` (which holds the stub region's
size fixed, so removing code from it does not also shift every
translated block's I-cache alignment and confound the result), the
measurement is on record in the same file: `-DN64_NOSMC` made the stub
code itself 0.85 ms/frame faster, and made `update_scanline` -- an
unrelated, fixed `.text` function that calls no stub at all -- 0.77 ms
*slower*, because the resulting layout re-aliases it against the rest of
the ROM in the 16 KB I-cache differently. Net frame time: unchanged.

So the *maximum possible* ceiling here, achieved by deleting the check
altogether (which cannot ship -- it is a correctness hole, not just a
performance one), already nets to ~0. The strictly smaller,
correctness-preserving `iwram_code_max` partial skip cannot do better
than that ceiling. Not worth building. Closed.

## The renderer is the largest single source of D-cache misses

    40.5%  n64_rdpbg_flush
    14.5%  memory-access stubs (above)
    10.8%  n64_rdpbg_add
     9.1%  n64_rdpbg_frame_end

`n64_rdpbg_flush` alone is 40% of sampled misses and 13.9% of frame time,
and the renderer as a whole (flush + frame_end + add + build_tlut) is about
24% of a frame. It emits RDP command words from a sorted draw list; the
sort and the emit both walk memory in an order the D-cache cannot follow.
That is a bigger single lever than anything remaining on the dynarec side.

## Fixing the SMC alias, and one attempt at the renderer that did not work

The tag skew landed: tag and data now sit on adjacent D-cache lines rather
than the same one. D-cache misses fell 11.7% and the frame 28.32 -> 27.40
ms, 35.3 -> 36.5 fps. One extra instruction per store buys back two
guaranteed misses.

The first attempt at it appeared to be a **41% win** and was worth nothing.
`iwram_raw` and `ewram_raw` are defined in `mips/mips_stub.S` with `.space`,
not in C, so growing them in `cpu.cc` did nothing and IWRAM data ran 16
bytes past its allocation into `vram_raw`. The emulator got faster because
the renderer had stopped drawing: `rdpbg` reported **0 rows, 0% of screen**,
and the fps was measuring an emulator that was no longer rendering the game.

That gives a cheap correctness canary for anything touching memory layout,
worth checking before believing any performance number:

    PROF:  rdpbg: 60 frames drew 160 rows each (100% of screen); 1216 tiles

A speed-up that comes with that line reading 0 is not a speed-up.

### The renderer's emit loop: sequential access made it worse

`n64_rdpbg_flush` is 40.5% of sampled D-cache misses and 13.9% of a frame.
The emit loop walked `rdpbg_draws[rdpbg_order[i]]` — an indirection through
a 20 KB array, two and a half times the D-cache — so once the sort stopped
the order being sequential, every read was a miss. Writing the records
themselves out in sorted order should have turned that into a sequential
walk.

Measured: **worse.** Emit fell 4.13 -> 3.76 ms but the sort rose 0.88 ->
1.51 ms, total misses went *up* 28.4M -> 32.7M, and the frame went 27.40 ->
27.81 ms. The counting sort's scatter is unavoidable either way, and
scattering 8-byte records costs more than scattering 2-byte indices saves.
Reverted.

The next idea for this path is not a cache trick but fewer records: 1216
tiles a frame fall into only 21 TMEM slices and 27 palette groups, so
horizontally adjacent tiles sharing a slice and palette could merge into
one wider texrect. That reduces the array being walked *and* the RDP
command count, rather than trying to walk the same array faster.

## Where the I-cache misses actually are: 57% is code the dynarec wrote

The D-cache had a per-address histogram from early on; the I-cache only
ever had an aggregate rate. That gap mattered, because it left the single
most important question about dynarec code quality unanswerable: the
generated code lives in `rom_translation_cache` and the emulator's own C
in `.text`, and **shrinking generated code is only worth doing if
generated code is what is missing.**

`IMISSHIST` now answers it. It buckets I-cache misses by 64 KB of
physical address exactly as `DMISS` does, so the two regions fall in
disjoint buckets and can simply be added up. Getting it working took four
attempts, which is worth recording because the obvious hook points all
silently report zero: with the recompiler driving, misses do not go
through `ICache::fetch`, `ICache::jitFetch`, or `CPU::jitIcacheFillMiss`.
The recompiler emits the *entire* refill inline -- tag write, clock step,
two `mov128`s -- and only calls `icacheFillLine` on the non-identity-map
path, which this workload never takes. The histogram therefore has to be
an emitted increment too; `slow.icachePaddr` is a compile-time constant
there, so the bucket address is one as well and it costs a single `add`
on the miss path.

Measured on the overworld savestate, current default build
(`ARES_HOMEBREW=true`, which is free -- frame time is 24.0 ms either way):

| region | buckets | share of I-cache misses |
| --- | --- | --- |
| dynarec ROM blocks | `2e`-`35` | **55.3%** |
| dynarec RAM blocks | `36` | 2.0% |
| emulator `.text` | `00`-`13` | 42.7% |

**57.3% of all I-cache misses are in code the dynarec generated.** The
buckets also say the generated working set is real and not a hot spot:
misses span `2e` through `33`, i.e. ~384 KB of the 512 KB ROM cache is
being touched, against a 16 KB I-cache. These are capacity misses, so a
proportional shrink of the generated code should buy a proportional
reduction in them.

Costing it, over one 600-VI-frame (10 s emulated, 937.5 M cycle)
reporting interval at 48 cycles a fill:

| | share of all CPU cycles | of a 24.0 ms frame |
| --- | --- | --- |
| I-cache stalls, total | 23.2% | 5.57 ms |
| — of which generated code | 13.3% | **3.19 ms** |
| D-cache stalls | 13.5% | 3.24 ms |

I+D together come to 36.8%, which independently reproduces this
document's own older "roughly a third of the frame is cache-miss stalls"
from a completely different measurement. That agreement is the reason to
trust the rest of the table.

### What this does and does not justify

It says plainly that **generated-code footprint, not generated-instruction
count, is the lever** -- and that retroactively explains the ARM dead-flag
elimination result in `docs/DYNAREC.md`. That change was correct and did
remove emitted instructions, but it added ~22 KB of `.text` for the
classifier: it moved cost out of the 57% bucket and into the 43% one. Net
negative, exactly as measured.

It also sizes the delay-slot work honestly. `tools/jitdis.py` puts
unfilled `j` delay slots at 8.4% of everything the dynarec emits. If that
footprint went away entirely, footprint-proportional scaling gives
~0.27 ms/frame of recovered I-cache stall plus ~0.13 ms of saved issue,
so **~0.4 ms of 24.0 ms, about 1.7%** -- and the single largest component
of it, block links, is only 3.7% of emitted code, worth ~0.75% on its own.

That is below this project's documented ~2.2% layout-noise floor, which
means **a frame-time A/B cannot prove any of it.** The useful conclusion
is not "don't bother" but "measure it with the right instrument":
`IMISSHIST`'s generated-code buckets are a direct, low-variance count of
the thing such a change actually moves, where frame time is a noisy proxy
for it. Any future codegen-size change should be judged on whether
buckets `2e`-`36` fall, and only then on whether the frame follows.

## One function's cache index is worth 12.5% of the frame

`IMISSFINE` resolves I-cache misses to 1 KB, and the two largest single
sites in the entire system landed on the same index:

    0x8000e400  update_gba+576                    252,954 misses
    0x80122400  render_window_n_pass<obj_pass,1>  191,006 misses

Both are `0x2400` mod 16 KB. The I-cache is 16 KB direct-mapped, so they
share lines -- and they interleave constantly, the event scheduler
running between CPU slices and the window renderer once per scanline.
Between them, 12.6% of every I-cache miss taken.

`-DN64_WINPASS_ALIGN=<n>` (video.cc, no-op unless set) forces a different
alignment on the renderer so the pair can be pulled apart:

| alignment | lands at index | frame | fps |
| --- | --- | --- | --- |
| none | `0x2400` (on `update_gba`) | 24.0 ms | 41.7 |
| 2048 | `0x3000` | **23.0 ms** | **43.5** |
| 4096 | `0x0000` (on the memory stubs) | 26.0 ms | 38.5 |
| 16384 | `0x0000` | 26.0 ms | 38.5 |

**Moving one function changes the frame by 12.5%.** And the effect is
mechanical, not luck: `4096` and `16384` place the function at two
different addresses 16 KB apart -- `0x80124000` and `0x80128000` -- which
therefore share a cache index, and they produce byte-identical results.
Same index, same performance; different address, no difference. Ordinary
layout noise cannot reproduce that pattern, and the `2048` result was
reproduced exactly across two separate runs.

The ranking also names the most cache-critical code in the build. Landing
on index `0x0000` is the worst of the three, and `0x0000` is where the
dynarec's shared memory stubs sit (`rom_translation_cache` + the stub
watermark). Those stubs run on *every* emulated load and store, and they
are **18,596 bytes against a 16 KB cache** -- so they not only conflict
with whatever else shares their indices, they wrap the index space and
conflict with themselves. Anything that collides with them pays for it.

### What this does and does not license

It does not license shipping `aligned(2048)`. That number is not a fix,
it is a build-specific accident: 2048-alignment happens to land this
function on `0x3000` *in this link*, and any future code change ahead of
it in link order moves it again. Landing it as a default would bank a
real 4.2% on a coin that gets re-flipped on the next commit.

What it licenses is the conclusion that **placement is the largest
untapped lever measured in this port** -- larger than the 8.4% of
generated code sitting in unfilled delay slots (~1.7%), larger than any
remaining renderer work -- and that the durable form of it is deliberate
placement rather than alignment roulette:

- the build already uses `-ffunction-sections -fdata-sections`, so the
  input sections exist to order explicitly;
- the hot `.text` set is small enough to matter and to fit: the buckets
  carrying real miss traffic total roughly 13 KB, against a 16 KB cache;
- the memory stubs, at 18.2 KB, are the one component that cannot fit
  under any ordering, and shrinking them below 16 KB is a separate,
  self-contained win that would stop them conflicting with themselves.

The measurement to judge any such change by is `IMISSFINE`, not fps: it
names the colliding pair directly, where frame time only says something
moved.
