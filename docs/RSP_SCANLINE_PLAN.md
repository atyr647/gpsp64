# Moving GBA scanline rendering to the RSP

## Why this is the lever

Measured on ares, fitting `frame_time = base + render/(interval+1)` across
frameskip 0/1/2 (the third point predicted to 0.3 ms):

    base (all CPU emulation, event engine)  21.5 ms/frame
    render (per rendered frame)             43.0 ms

Rendering costs **twice all CPU emulation combined**. `PROFILE_PPU` puts 93%
of it in `render_scanline_window`; OAM sort, layer ordering, forced blank and
affine are 0-3% each.

It is also the *safe* lever. Rendering is provably cosmetic: `insns_k=516`
is identical at frameskip 0, 1 and 2, so the emulated game executes exactly
the same work regardless of how many frames are drawn. `update_scanline`
returns before touching anything but the screen buffer, and nothing reads
that buffer back. Worst case for a bug here is a wrong image, never a
desync or a crash.

Targets, at frameskip 0 (`frame = 21.5 + render`):

    render 43.0 ms -> 15.5 fps   (today)
    render 21.5 ms -> 23.3 fps   frameskip becomes free to delete
    render 11.5 ms -> 30.0 fps   every frame drawn
    render  0.0 ms -> 46.5 fps   hard ceiling; CPU emulation remains

## PREMISE CORRECTED (read this first)

This plan was written believing rendering was memory-bound, and its data-flow
section is built around DMA'ing tiles into DMEM to eliminate cache misses.
**That premise is false**, measured after the blit/screen-buffer fixes:

    rendering        27.40 ms/frame
      memory stall    4.15 ms  (15%)   misses 7554 + writebacks 2175 per frame
      compute        23.25 ms  (85%)

Measured by frameskip ablation on D-cache miss counts (frameskip 0 vs 9,
360 emulated frames each, 40 cycles per miss in ares's model).  Eliminating
*all* memory stall would buy 4 ms of 27.

The work is simply large: 4 layers x 240 x 160 = 153,600 layer-pixels per
frame at ~6 instructions each is ~920K instructions/frame, running at ~2.8
cycles/instruction.  That is not pathological, it is one 93.75 MHz CPU doing
a lot of arithmetic.

So the RSP is still worth doing, but for the opposite reason: not to avoid
misses, but because it is **a second execution unit running concurrently**.
That changes what matters in the design below:

  * The DMEM tile-caching scheme is still useful (it keeps the RSP fed
    without RDRAM round-trips) but it is no longer the source of the win.
  * The vector unit is now the whole point, so the 4bpp palette question is
    central rather than incidental.  Confirmed 100% 4bpp, so the vmrg
    select tree applies.
  * Amdahl still binds: rendering is 27.4 ms of a 55 ms frame, so even a
    perfect RSP renderer caps the frame at ~28 ms (~36 fps) until CPU
    emulation improves.
  * objblend/bright are active on most scanlines, so a ucode handling only
    plain opaque tiles would fall back to C most of the time.

Cheaper alternative to try first, given the workload is compute-bound:
**skip redundant rendering**.  If nothing affecting the image changed since
the previous frame, reuse it.  That attacks the 85% directly, costs no
ucode, and on static screens (menus, dialogue, title) could skip nearly all
of the 27.4 ms.

## Do the cheap thing first

Rendering is currently ~105 VR4300 cycles per output pixel
(43 ms x 93.75 MHz / 38400 px). A tuned tiled renderer is 10-20. But that
figure counts each *output* pixel once while a scanline renders several BG
layers plus objects and then composites, so the real number is
105 / (layers + obj + compose passes). `-DPROFILE_PPU2` reports the layer
count and which effect path runs; get that first. If Emerald is running 4
layers, per-layer-pixel cost is ~21 cycles and the win is in reducing
*passes*, not in rewriting the pixel loop.

`render_tile_Nbpp` is already good: 32 bits of tile data per read, early-out
on transparent rows, fully templated on rdtype/isbase/is8bpp/hflip so there
are no runtime branches, ~5-6 instructions per pixel. Do not rewrite it
blindly. The gap between 6 and 105 is passes and cache misses, and cache
misses are plausible: 96 KB VRAM plus tilemaps against an 8 KB D-cache,
with scattered tile access.

## Infrastructure (verified present)

libdragon ships what is needed, no toolchain work:

  * `rsp.h`, `rspq.h` - RSP control and the command queue
  * `rsp.inc`, `rsp_dma.inc`, `rsp_queue.inc` - ucode-side macros
  * `n64.mk:157` assembles any source file named `rsp*.S` as ucode
    automatically, using `N64_RSPASFLAGS`

## Data flow

The RSP has 4 KB DMEM and 4 KB IMEM, and DMAs to/from RDRAM itself.

The naive approach - DMA the tile rows needed for one scanline - is wrong:
each tile row is 4 bytes at a scattered VRAM address, so a scanline needs
~31 tiny DMAs per layer and setup overhead dominates.

Use the vertical reuse instead. Consecutive scanlines within a tile row
band use the *same* tiles, so fetch whole tiles once per 8 scanlines:

  1. On entering an 8-line band, DMA the tilemap row (32 entries, 64 B) and
     the ~31 unique tiles (31 x 32 B = 992 B at 4bpp) per layer.
  2. Render 8 scanlines entirely out of DMEM, no further RDRAM reads.
  3. DMA each finished scanline out.

RDRAM traffic drops to ~1 KB per layer per 8 scanlines.

### DMEM budget

Full-width (240 px), 4bpp, with tile caching:

    per layer: tilemap 64 B + tiles 992 B            = 1056 B
    palettes (BG 512 B + OBJ 512 B)                  = 1024 B
    output scanline (u16)                            =  480 B

Two layers fits (3616 B). **Four layers does not** - 4224 B of tiles alone
exceeds DMEM before palettes or output.

Fix: process **half scanlines** (120 px, 16 tiles per layer):

    per layer: tilemap 32 B + tiles 512 B            =  544 B
    four layers                                      = 2176 B
    palettes                                         = 1024 B
    output half-scanline                             =  240 B
    total                                            = 3440 B   fits

## The real technical risk

Per-pixel palette lookup does not vectorise. The RSP's vector unit has no
gather - it cannot index memory per lane - and palette lookup is exactly a
per-pixel table read. A scalar lookup per pixel throws away the vector unit
and most of the reason to use the RSP at all.

For 4bpp this is workable: the palette is 16 entries, so a 4-level
`vmrg` select tree over the 4 index bits resolves 8 pixels in ~15 vector
ops (~2 ops/pixel), which is competitive. For 8bpp (256 entries) it is not
feasible vectorised and would have to stay scalar.

**Check what Emerald actually uses before committing.** If the hot layers
are 4bpp the plan works; if they are 8bpp, the RSP is much less attractive
and pass-reduction on the CPU is the better investment. `PROFILE_PPU2` plus
a check of `BGxCNT` bit 7 answers this.

## Sequencing

1. Run `-DPROFILE_PPU2`: layer count, effect path, video mode, obj-blend.
2. Determine 4bpp vs 8bpp for the hot layers.
3. If passes are the problem, reduce passes on the CPU first - cheaper and
   lower risk than any ucode.
4. Only then write ucode, starting with the plain 4bpp tiled path and
   falling back to the C renderer for every case it does not handle
   (affine, bitmap modes, mosaic, windows, 8bpp). A partial RSP renderer
   that defers the hard cases is worth shipping; a complete one is a much
   larger project.

The RSP is completely idle in this port today, and it runs *concurrently*
with the VR4300 rather than competing for it, so work moved there is close
to free rather than merely faster.
