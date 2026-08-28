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
| CPU renderer (baseline) | 58.0 ms | 17.24 |
| RDP, backgrounds only | 54.25 ms | 18.44 |
| RDP, + sprites | 46.7 ms | 21.41 |
| RDP, + correct frame boundary | 41.7 ms | 23.98 |
| RDP, + blank-tile skip | 41.3 ms | 24.20 |

PPU time falls from 48% of emulated time to 5%.  100% of scanlines are
drawn by the RDP on this workload.

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
