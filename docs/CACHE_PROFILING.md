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
