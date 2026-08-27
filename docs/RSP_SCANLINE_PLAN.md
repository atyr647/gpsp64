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

## Measured before building (2026-08, overworld savestate)

Three things that were assumptions when this plan was written now have
numbers, and they change the verdict.

**The ceiling.**  `-DN64_NO_BG` skips BG tile rasterisation and fills with
backdrop -- a timing probe, the image is wrong under it:

```
  BG rasterisation on    68.0 ms/f   14.71 fps
  BG skipped             49.0 ms/f   20.41 fps
```

19 ms of a 68 ms frame, +38.8%.  That is the hard ceiling for any BG
offload; the RSP path only recovers part of it, since it still pays a
band gather (1.8 ms measured, more with flips and wrapping), a CPU
palette pass (~1.6 ms) and ~2200 cycles per RSP invocation.  A realistic
net is 12-14 ms, so roughly +25%.

**Applicability.**  In the overworld the fast path applies to *100%* of
scanlines (effect none, no obj blend); in the intro cutscene, 0%.  The
plan shelved this work believing effects would keep it from ever
applying, which was measured during boot and is false for gameplay.
OBJ is never interleaved between BG layers either -- 60% of scanlines
have no OBJ, 40% have it last, 0% interleaved -- so a batched BG
composite is always ordering-valid.  Tiles are 100% 4bpp.

**The precondition nobody had checked.**  gpSP renders one scanline at a
time as the emulated PPU advances, because mid-frame register writes have
to be honoured per scanline.  Banding several lines from one snapshot is
only sound if the drawing state holds still during HDraw.  Counted:

```
  mid-frame writes per frame (HDraw only)   VRAM   palette   BG regs
    overworld                                0.0       1.7       0.0
    intro cutscene                           0.0       0.0       0.0
```

No VRAM and no BG register writes during HDraw at all, so bands are safe.
The 1.7 palette writes per frame in the overworld are the one hazard: a
deferred palette pass would apply a late palette to earlier scanlines.
Either snapshot the palette per band or detect the write and fall back.

## The RDP can do what the RSP could not (measured)

The RSP verdict below still stands, but it is a verdict about *splitting
the fused per-pixel loop into passes*, not about hardware offload as
such.  The RDP moves the whole loop instead, and it measures completely
differently.  Numbers from n64/n64_rdp_bench.c, on hardware terms:

```
  BG rasterisation on the VR4300 today            27 ms
  issuing the same frame as RDP textured rects   3.33 ms CPU (195 cyc/tile)
  TMEM upload, one 8x8 tile at a time            1472 cyc  -> 37 ms/frame
  TMEM upload, one 64x64 atlas (64 tiles, 2 KB)  1214 cyc  -> 19 cyc/tile
```

Per-tile uploads would sink it; the atlas does not, because the cost is
fixed command overhead and not bandwidth -- 2 KB uploads for less than
32 bytes does.  A frame becomes a handful of atlas uploads plus ~1,600
rectangles: **~3.4 ms of CPU replacing 27 ms**, with the RDP's own fill
running in parallel.  That is a 55 ms frame going to roughly 31.

What is still unmeasured or unbuilt, in order of how much it could hurt:

  * **RDP fill time is invisible in ares** (its RDP thread advances a
    clock in fixed chunks).  96,000 pixels/frame is 1.5 ms at a pixel per
    cycle and ~6 ms at 4 cycles/px, so it should fit a 31 ms frame with
    room -- but that is arithmetic, and it needs console to confirm.
  * **Semantics.**  Four priority layers, per-tile palettes, h/v flip,
    the blend modes and windows all have to map onto RDP primitives with
    a CPU fallback for what does not.  bgband_* in video.cc is a
    pixel-exact reference for exactly these, verified over 300K+
    scanlines.
  * **rspq owns the RSP.**  libdragon drives rdpq from RSP ucode, and
    this port's rsp_gbascan blit overwrites it, so the two cannot
    coexist.  Either the blit becomes an rspq overlay or it goes back to
    rdpq_tex_blit.
  * **No gather is needed** -- measured.  A GBA charblock is already a
    linear array of 4bpp tiles in VRAM, and although tiles are tile-major
    while RDP textures are row-major, an 8-pixel-wide strip lines up
    exactly: tile k is rows 8k..8k+7, drawn with t = 8k.  TMEM fills
    straight from vram_raw.

    ```
      VRAM strip 8x64   =  8 tiles:  512 TMEM bytes, 1247 cyc (155/tile)
      VRAM strip 8x128  = 16 tiles: 1024 TMEM bytes, 1372 cyc  (85/tile)
      VRAM strip 8x256  = 32 tiles: 2048 TMEM bytes, 1362 cyc  (42/tile)
      VRAM strip 8x512  = 64 tiles: does not fit
    ```

    A tile costs 64 TMEM bytes rather than 32: TMEM rows are 8-byte
    aligned and a CI4 row of 8 pixels is 4 bytes, so half of every line
    is padding.  With a TLUT resident that caps a slice at 32 tiles, and
    8x512 fails outright.  Cost is fixed per upload regardless of size,
    so 32-tile slices are the shape to use.

    Budget for a frame: ~47 slice loads (600 distinct tiles per layer,
    2.5 layers) at ~1360 cyc = 0.7 ms, plus ~1,500 rectangles at 195 cyc
    = 3.3 ms, plus 0.05 ms maintaining the nibble-swapped shadow below.
    **~4.1 ms of CPU against 27 ms today**, which is a 55 ms frame going
    to roughly 32.

  * **The RDP's CI4 nibble order is opposite to the GBA's** -- verified,
    not assumed.  It takes the HIGH nibble as the left pixel; the GBA
    packs byte = (right << 4) | left.  Feeding it raw tile bytes
    transposes every adjacent pixel pair, which renders an almost-correct
    image.  The boot selftest in n64/n64_rdp_bench.c caught this on 256
    synthetic pixels before any renderer existed.

    Swapping on demand costs **3.78 ms/frame** for 1,500 tiles (48 KB) --
    memory-bound at ~30 cycles a word, not the handful of ops it looks
    like.  But VRAM barely changes:

    ```
      VRAM per frame   overworld: 1 CPU store, 0.6 KB DMA
                       cutscene:  0 CPU stores, 1.3 KB DMA
    ```

    So keep a **nibble-swapped shadow of VRAM**, updated on write.  ~0.6 KB
    a frame instead of 48 KB: 0.05 ms against 3.78, for 96 KB of RAM.
    TMEM then uploads straight from the shadow with no gather at all.

  * **Per-tile palettes and flips still need a plan.**  Each GBA tile
    picks one of 16 sub-palettes; a CI4 TLUT holds all 16 at once, but
    the palette is a field in the tile descriptor and the RDP has only 8
    of those, so draws want batching by (slice, palette).  Flips map to
    flipped texture rectangles.  Neither is hard, both are fiddly, and
    bgband_* is the reference to check them against.

## VERDICT: measured dead.  Do not build this.

The BG offload cannot pay, and the reason is structural rather than a
matter of ucode quality.  Every number below is from ares on the
overworld savestate; the offload's CPU half was built and verified
pixel-exact first, so these measure the real thing.

```
  gpSP renderer (baseline)                          68.0 ms
  BG rasterisation skipped entirely                 49.0 ms   -> BG = 19.0

  offload floor, first attempt                      79.0 ms
  offload floor, + band tile cache                  82.0 ms   WORSE
  offload floor, + band cache + linear output       81.0 ms   <- the floor
  offload complete (merge on CPU)                   94.0 ms
```

The floor is what the CPU still pays with a **free, instantaneous RSP**:
81 ms against a 68 ms baseline.  The offload loses 13 ms before the RSP
does anything at all.

Why: gpSP's renderer fetches a tile byte, extracts the nibble, looks up
the palette and stores, in one pass with the tile pointer live in a
register.  Any offload must instead write tile bytes out to memory, read
them back, and walk the result again for the palette.  Those two extra
round-trips per pixel cost more than the entire fused rasterisation they
were meant to accelerate, and the RSP never touches either of them.

Both obvious repairs were tried and both failed:

  * **Band tile cache** (fetch whole 32-byte tiles once per tile row
    instead of a 4-byte row per scanline) made it *worse*, 79 -> 82 ms.
    It removes fetch misses but adds a 32-byte copy per tile per row
    while the per-scanline 4bpp packing remains -- a copy added, none
    removed.  The VRAM reads were evidently already cheap enough that the
    misses were not the cost.
  * **Linear ucode output** instead of planar bought 1 ms of 14.  The
    planar index arithmetic was not the cost either; the palette pass is
    dominated by the per-pixel load, gather and store, which no layout
    changes.

Estimate history, worth reading before trusting a projection here: +25%
predicted, floor predicted at 54 ms against an actual 79, then the two
"corrections" above predicted to recover most of it and delivering -3 ms
and +1 ms.  Four consecutive wrong estimates, every one of them about
memory behaviour; the instruction-count predictions in the same exercise
were all correct.

What survives: `bgband_*` in video.cc, a pixel-exact reference
implementation of gpSP's BG semantics (300K+ scanlines across three game
states, 0 mismatches), including the discovery that the ucode writes
`nibble | palette` for the base layer where gpSP wants the backdrop --
resolvable in the palette pass with `(v & 0xF) ? pal[v] : pal[0]`.  It is
compiled out by default and kept as documentation for any future attempt
at a *different* design.  A design that keeps the fused per-pixel loop and
moves it wholesale -- the RDP, say -- is not refuted by any of this; one
that splits the loop into passes is.

## What the offload actually costs (measured, not modelled)

Building the CPU half first and measuring each piece separately:

```
  gpSP renderer (baseline)                68.0 ms
  BG rasterisation skipped                49.0 ms   -> BG raster = 19.0

  + palette pass only                     63.0 ms   -> palette   = 14.0
  + gather (per scanline)                 79.0 ms   -> gather    = 16.0
  + merge (naive C model)                 92.0 ms   -> merge     = 13.0
```

The merge is the only part that moves to the RSP.  Everything else is
what the CPU still pays, and at 30 ms it already exceeds the 19 ms of
rasterisation being replaced -- so as built, this loses ~11 ms even with
a free RSP.

But two of those numbers measure implementation mistakes, not the design:

**The gather is per-scanline and should be per-band.**  Eight consecutive
scanlines read eight different rows of the *same* tile, so fetching the
whole 32-byte tile once per band turns eight scattered 4-byte reads into
one sequential 32-byte read -- two cache lines instead of eight.  That is
what "band" was supposed to mean, and n64_rsp.c already measures the
correct version at **1.83 ms/frame** against the 16 ms measured here.

**The palette pass is expensive because the ucode's output is planar.**
Consecutive pixels land in four different planes, so every pixel costs an
index computation and a strided load.  A linear output would make it
`dst[x] = pal[src[x]]`.

Corrected arithmetic, if both are fixed: 49 + 1.8 + (linear palette) +
RSP overhead.  That is roughly 59 ms, or +15% -- worth having, but a
third of the original projection, and it needs a ucode change on top of a
band gather.

Note the estimate history before trusting that number: the projection
for this work was +25%, the predicted floor was 54 ms against an actual
79, and the merge-dominates-M prediction was right.  Estimates about
*memory behaviour* here have been consistently wrong and estimates about
*instruction counts* consistently right.  Measure the band gather and the
linear palette pass on the CPU before writing any ucode.

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
