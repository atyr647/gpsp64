# Backgrounds and sprites on the RDP

The GBA rasterises four background layers and 128 sprites in dedicated
hardware, in parallel with its CPU.  This port did that job on the
VR4300, and it cost 27 ms of a 55 ms frame -- about half the frame, and
about 26 cycles per layer-pixel across ~96,000 of them.

Every attempt to make the CPU version cheaper failed:

| attempt | result |
| --- | --- |
| RSP composite offload | floor measured 13 ms **worse** than the CPU renderer |
| band tile cache | 79 -> 82 ms |
| linear (non-planar) ucode output | 1 ms of a predicted 14 |
| palette lookup table | 4 ms at most, of 27 |
| cached screen buffer | 10.9% worse |

The renderer already compiled to 1,019 instructions with no calls and
2.4% stack traffic.  There was nothing left to shave.  The N64's actual
answer to "the GBA has a PPU" is the RDP.

## Result

Frame time on the captured overworld savestate, means over several link
layouts (see *Measuring* below for why that matters):

| build | frame | fps |
| --- | --- | --- |
| CPU renderer (baseline) | 60.2 ms | 16.61 |
| RDP renderer | 41.2 ms | 24.27 |
| shipped default today | 35.5 ms | 28.17 |
| …with RDP fill time charged | ~36.5 ms | ~27.4 |

The last row is the renderer plus the compiler settings it unlocked --
`cpu.cc` at -O3 and `aot_generated.c` at -Os, both of which lost to the
old defaults while the CPU still rasterised backgrounds and win now that
it does not -- and `rdpq_exec`, which ares could not evaluate until it
was taught to charge for uncached stores.  **60.2 -> 35.5 ms, 16.6 ->
28.2 fps, +70%.**  The spread across link layouts also collapsed, from
12 ms to 1.

The last row is the honest hardware figure.  ares does not charge for RDP
fill time, so every number above it excludes the ~2.06 ms the renderer
costs the RDP; running with `GPSP_RDP_CHARGE=1` adds 1.0 ms, not 2.06,
because the RDP works asynchronously and the CPU only waits for it at
`rdpq_detach_wait`.  So the RDP is real but mostly free, and it is
nowhere near being the bottleneck.

**-31.6% frame time, +46% fps** for the renderer alone, from five link
layouts each on the same tree.  The two distributions have the same shape -- 56/57/59/61/68
against 37/37/39/42/51 -- so the means are comparable; medians give
59 -> 39 ms, +51%.

Getting there, each step measured over three or four layouts:

| step | frame | fps |
| --- | --- | --- |
| CPU renderer | 58.0 ms | 17.24 |
| RDP, backgrounds only (45% of rows) | 54.25 ms | 18.44 |
| + sprites (100% of rows) | 46.7 ms | 21.41 |
| + correct frame boundary | 41.7 ms | 23.98 |
| + blank-tile skip | 41.3 ms | 24.20 |
| + sort only the live keys | 38.7 ms | 25.86 |

PPU time falls from 48% of emulated time to 5%.  100% of scanlines are
drawn by the RDP on this workload.

## Where the frame goes now

Measured directly, by sampling the emulated PC (`native/ares_pcprof.sh`)
rather than by deriving it from counters:

| | share |
| --- | --- |
| `execute_arm` (the interpreter loop) | 43.8% |
| `update_gba` (the event engine) | 12.1% |
| this renderer, across its five functions | ~16% |
| `update_scanline` | 5.6% |
| `render_gbc_sound` | 3.1% |
| `bios_hle_swi` | 2.4% |
| `sound_timer` | 2.3% |

The emulator core is 56% of the frame and does not respond to having
work removed from it -- see *What the emulation half is not* below.  The
renderer's own 16% is 1,217 textured rectangles a frame plus the tilemap
and OAM walk that produces them.

## The five things that had to be true

None of them were obvious, and each was verified against a CPU reference
in `n64/n64_rdp_bench.c` before anything was built on it.

**Nibble order is reversed.** The GBA packs a 4bpp byte as
`(right << 4) | left`; the RDP takes the *high* nibble as the left pixel.
Handing it raw VRAM transposes every adjacent pair -- subtle enough to
survive a casual look at a screenshot.  The selftest caught it by getting
all 256 pixels wrong, each holding its neighbour's colour.

**Tile-major VRAM addresses as an 8-pixel-wide strip.** GBA tiles are 32
consecutive bytes, which scrambles under any row-major texture view
*except* a width of 8, where tile k lands exactly at `t = 8k`.  So TMEM
loads straight out of a shadow of VRAM with no gather -- the copy that
sank the RSP attempt does not exist on this path.

**Flips are free.** The tile descriptor's mirror bit was the expected
mechanism, but mirror wraps on a power-of-2 mask, and masking `t` to 8
rows would fold the whole strip back onto its first tile: strip
addressing and t-mirroring cannot coexist.  A texture rectangle carries
its own signed `ds/dx` and `dt/dy`, so walking the texture backwards
flips it with no descriptor change at all.  That matters because only 1%
of tiles are flipped but 38% of scanlines contain one, so declining them
would have cost a third of the screen.

**Palette is a descriptor field.** The GBA's 256-colour BG palette maps
exactly onto the RDP's 256-entry TLUT: 16 sub-palettes, 16 windows.  A
second tile descriptor aimed at the same TMEM address with a different
palette number resolves through a different window with no reupload.

**A TMEM load is a slice, not a tile.** 2 KB of texture with a TLUT
resident is 32 tiles, and loading all 32 costs ~1725 cycles -- the same
as loading one.  So draws are sorted by (slice, palette) and each 1 KB
slice of the shadow uploads once.  A whole frame needs about 21.

## Shape of the renderer

`video.cc` decides *what* to draw and `n64/n64_rdp_bg.c` turns it into
RDP commands.

At scanline 0, `rdpbg_frame_begin()` decides whether the frame is
drawable at all and which rows belong to the RDP.  This needs no
prediction and no fixup, because `order_obj()` has already built a
per-row sprite table for the entire frame before scanline 0 renders -- a
row's sprite status is a fact, not a guess.  Only the BG scroll registers
have to be verified as the frame goes, and a mismatch hands the rest of
the frame back to the CPU.  Measured: zero mismatches, consistent with
52.5% of scanlines sitting in a stable 8-line band against 55.3% being
BG-only.

At each scanline, if the row belongs to the RDP the CPU rasteriser is
skipped outright.  That is the entire point.

At frame end, `rdpbg_frame_end()` walks the tilemaps and OAM, sorts, and
emits.  Layers are flushed one at a time, back to front: the RDP has no
per-pixel priority beyond draw order, so the GBA's layer order has to
survive into the command stream.  Sorting the whole frame by slice was
cheaper -- 5 TMEM loads instead of 21 -- and wrong, because it let a
background layer draw after the one that should cover it.

Sprites deliberately skip that sort.  A layer's own tiles never overlap,
so reordering them by slice is free; two sprites can overlap, and the GBA
resolves that by OAM index, so the order they were added in *is* the
answer.  They pay a TMEM load or two each instead.

## What still falls back to the CPU

Frames using a real window, a colour effect, a bitmap video mode, 8bpp
backgrounds or mosaic.  Rows carrying an affine, 8bpp, mosaic,
semi-transparent or OBJ-window sprite, and rows where gpSP's per-row
sprite budget would have dropped a sprite -- that budget is mirrored
exactly rather than guessed at, since a row where it bites would render
differently here.

A window is only a fallback if it *does* something.  Emerald's overworld
runs with WIN0 spanning the whole screen and `WININ = 0x1F1F`, so the
window enables all five layers everywhere and its only real effect is to
clear the effects bit.  Refusing every windowed frame threw away the
entire overworld; the test now asks for the enable flags the CPU renderer
would end up using and carries on if they are uniform.  The bounds tests
mirror `render_window_n_pass` and `in_window_y` exactly, because the
point is to agree with the renderer being replaced, not with the hardware
manual.

## The RSP blit had to go

libdragon drives rdpq from the RSP -- `rspq` is a command processor
running *as* RSP ucode -- and this port loaded its own `rsp_gbascan`
ucode for the framebuffer blit, which overwrites it.  They cannot both be
loaded.  The blit returns to the CPU, where it now only has to convert
the rows the RDP declined, which on this workload is none of them.

## Measuring

**The VR4300's 16 KB instruction cache is direct-mapped**, so which lines
of the interpreter collide with which lines of the renderer is decided by
where the linker happened to put them.  Two builds of this renderer
differing only in profiling counters measured 66 ms and 53 ms.  A change
worth 5 ms cannot be read off one run against one baseline when layout
alone is worth 13.

`-DN64_TEXT_PAD=<bytes>` resamples the layout without changing behaviour.
Quote a mean over three or four pad values, not a single run.

**Do not edit the tree while `ares_bench.sh` is looping.** It re-stages
the repo per label, so an edit mid-loop silently builds two labels of the
same "configuration" from different source.

**A speed number needs a picture next to it.** A renderer can get faster
by drawing the wrong thing, or nothing.  `native/ares_shot.sh <label>
<seconds> [flags]` boots the same savestate and captures the ares window;
it takes six captures and keeps the one with the most distinct colours,
because at 24 fps against a 60 Hz refresh most captures catch the
framebuffer mid-write.

## What the emulation half is not

Four separate attempts to make the other 56% cheaper all measured
neutral, and together they say something useful about the shape of the
problem.

**Not interpreted instructions.** Adding the largest remaining
contiguous run of hot ROM pages to the AOT cut interpreted instructions
by exactly the predicted 25% (426K -> 320K per window) and moved the
frame time not at all, for 790 KB of extra `.text`.

**Not timeslice transitions.** The emulator leaves the interpreter 672
times a frame.  The HBlank half of each scanline exists as a scheduling
point only so an HBlank IRQ or DMA can fire at the right cycle, and
neither is ever armed on this workload -- so merging the two halves
removed 228 of those 672 yields.  No change.
(`-DN64_MERGE_HBLANK`, off by default: it trades away HBlank
observability for nothing.)

**Not the per-instruction checks.** A line-level profile of
`execute_arm` shows ~60% of it is bookkeeping rather than instruction
execution -- the idle-page guard, the runtime idle detector,
`check_pc_region`, the cheat-hook compare, the AOT page lookup, the alert
check -- with the 256-way dispatch switch itself at 6%.  Deleting the
cheat-hook compare (`-DN64_NO_CHEATS`, 5.6% of samples, can never match
in this port) and giving the idle detector a sequential fast path
(`-DN64_FAST_IDLE_DETECT`) both measured slightly worse.

**Not the audio.** The m4a mixer does not appear in the profile at all,
which retires an old suspicion: the native substitution in
`n64/m4a_hle.c` really did make it free.  Skipping gpSP's own
DirectSound interpolation while audio output is off -- 128 KB of ring
buffer a frame that nothing reads -- also measured neutral.

What is left is memory behaviour, and the direct profile has no more
resolution to offer on it.

**ares does not model uncached store stalls either.** It models D-cache
misses -- which is why every cache experiment in this port has read true
-- but the write buffer costs nothing in its CPU model.  That makes one
class of change unmeasurable here: `-DN64_RDP_EXEC` builds the RDP
command list in cached memory and hands it over with `rdpq_exec()`
instead of pushing each rectangle through rspq's uncached buffers.  The
reasoning says it should save most of the 3.3 ms spent emitting; ares
says it costs 0.7 ms, because it only sees the write-allocate misses the
cached buffer adds and none of the uncached stalls it removes.  It is
kept behind the flag, off by default, for whoever has hardware.

**ares does not model RDP fill timing** -- its RDP thread advances a
clock in fixed chunks.  What is measured here is the CPU half: queue
generation, TMEM uploads, and stalls when the queue backs up.  The RDP's
own time has to come from fill-rate arithmetic (~104,000 textured pixels
per frame at roughly 1 pixel/cycle on a 62.5 MHz part, plus per-primitive
setup) and, ultimately, from console.

## Where the renderer's time actually goes (time-weighted, line level)

`GPSP_PCCYC` samples plus `addr2line` give per-line attribution. On the
default build, overworld savestate:

    n64_rdpbg_flush   13.9% of frame, 40.5% of all D-cache misses
      42%  n64_rdp_bg.c:298   d = &rdpbg_draws[rdpbg_order[i]]
      23%  n64_rdp_bg.c:312   rdpq_tex_upload (TMEM slice change)
      12%  n64_rdp_bg.c:272   the counting sort's scatter
       7%  n64_rdp_bg.c:258   the key-counting pass

    n64_rdpbg_frame_end  8.2% of frame
      28%  video.cc:3288  gba_deref16 of the tilemap entry
      27%  video.cc:3297  N64_TILE_NZ blank check
      27%  video.cc:3298  the n64_rdpbg_add call

### Three things that do not work

**Merging horizontally adjacent tiles into one wider texrect is structurally
impossible.** A slice is loaded as `surface_make_linear(..., FMT_CI4, 8, 256)`
-- an eight-pixel-wide strip, because that is the only row-major view that
matches tile-major GBA VRAM. Tiles within a slice stack *vertically*, so two
tiles that are side by side on screen are 8 texels apart in T, and no
rectangle can span them. Widening the load would interleave tile data
incorrectly. This was the obvious next idea and it is a dead end.

**Materialising the sorted records** so the emit loop reads sequentially,
instead of indirecting through `rdpbg_order`: emit 4.13 -> 3.76 ms, but sort
0.88 -> 1.51 ms, misses 28.4M -> 32.7M, frame 27.40 -> 27.81. A counting sort
scatters either on write or on read; scattering 8-byte records costs more
than scattering 2-byte indices saves.

**Not sorting at all.** The sort groups tiles by TMEM slice and palette, and a
TMEM reload costs RDP time -- of which there is plenty spare, `wait-for-RDP`
being 0.24 ms of a 27 ms frame. So spending idle RDP time to buy CPU time
back looked free. It is not: TMEM slices went 21 -> 135 and palette groups
27 -> 150, and **emit rose 4.14 -> 7.63 ms**, frame 27.40 -> 29.99 (+9.4%).
`wait-for-RDP` stayed flat at 0.18 ms, confirming the RDP did absorb the
extra loads -- but each one also costs CPU to *generate*. The sort is paying
for itself in command generation, not in RDP time.

### What is left, and what it costs

The 23% on line 312 is 21 `rdpq_tex_upload` calls a frame at roughly 45 us
each. Emitting those as raw RDP words into `rdpbg_cmds`, the way the
rectangles already are, would also collapse the 48 `RDPBG_SUBMIT()` calls per
frame -- each of which does a `data_cache_hit_writeback`, an `rdpq_exec` and
two pipeline syncs -- down towards one. That means hand-emitting
SetTextureImage, LoadBlock and SetTile and taking TMEM management over from
libdragon: the largest remaining renderer win, and the first one here that
cannot be tested with a one-line change.
