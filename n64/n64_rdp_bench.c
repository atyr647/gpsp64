/* gameplaySP - N64: is an RDP background renderer affordable?
 *
 * The GBA rasterises 4 background layers in dedicated hardware, in
 * parallel with its CPU.  This port does that job on the VR4300, and it
 * costs 27 ms of a 55 ms frame -- about 26 cycles per layer-pixel across
 * ~96,000 of them.  Measured attempts to make it cheaper have all
 * failed: the RSP offload's floor was 13 ms *worse* than the baseline,
 * a palette lookup table is worth 4 ms at most, and the renderer already
 * compiles to 1,019 instructions with no calls and 2.4% stack traffic.
 *
 * The N64's actual answer to "the GBA has a PPU" is the RDP.  On paper
 * the pixels fit easily: 96,000 pixels at ~1 cycle each on a 62.5 MHz
 * part is 1.5 ms, or ~6 ms even at 4 cycles/pixel for textured fills,
 * against 27 ms of CPU today.
 *
 * What is NOT obvious is the CPU side.  Drawing a GBA background through
 * the RDP means one textured rectangle per 8x8 tile -- roughly 650 tiles
 * per layer, ~1,600 per frame at 2.5 layers -- plus a TMEM load whenever
 * the tile is not already resident.  If generating those commands costs
 * the CPU more than the 27 ms being replaced, the idea is dead no matter
 * how fast the RDP draws, and that is worth knowing before building a
 * renderer around it.
 *
 * ares does not model RDP fill timing (its RDP thread advances a clock
 * in fixed chunks), so what this measures is exactly the CPU half: queue
 * generation, TMEM uploads, and any stalls when the command queue backs
 * up.  The RDP's own time has to come from the fill-rate arithmetic above
 * and, ultimately, from console.
 *
 * NOTE ON ORDERING: this must run before n64_rsp_init().  libdragon
 * drives rdpq from the RSP -- rspq is a command processor running as RSP
 * ucode -- and this port loads its own rsp_gbascan ucode for the
 * framebuffer blit, which overwrites it.  Any rdpq call after that hangs
 * in rspq_next_buffer waiting for a processor that is no longer running.
 *
 * That is a genuine constraint on any RDP renderer here, not merely on
 * this benchmark: the custom RSP blit (worth +7.1% when it landed) and
 * the RDP cannot both be used as things stand.  Either the blit ucode
 * becomes a proper rspq overlay, or the blit goes back to rdpq -- which
 * is what the original non-fused path did with rdpq_tex_blit, and which
 * only looked useless because Vulkan was never initialising in ares.
 *
 * N64 port Copyright (C) 2026
 */

#include <libdragon.h>
#include <string.h>
#include "../common.h"

#ifdef N64_RDP_BENCH

#define RDPB_TICK() ({ u32 _t; __asm__ volatile("mfc0 %0, $9" : "=r"(_t)); _t; })

/* One frame of Pokemon Emerald overworld: 30x20 visible tiles per layer,
 * ~2.5 layers with anything to draw. */
#define RDPB_TILES_PER_FRAME 1600

/* Prove the RDP mechanics before any renderer depends on them.
 *
 * Four things have to be right and none of them are obvious from the
 * documentation:
 *
 *   - which nibble of a 4bpp byte is the LEFT pixel.  The GBA packs
 *     byte = (right << 4) | left; if the RDP unpacks the other way every
 *     pair of pixels comes out swapped, which is subtle enough to survive
 *     a casual look at a screenshot.
 *   - that a tile-major VRAM run really does address as an 8-wide strip,
 *     tile k at t = 8k.
 *   - that the TLUT resolves indices to the colours we loaded.
 *   - that per-tile palette selection works, since a GBA tile picks one
 *     of 16 sub-palettes and that is a tile-descriptor field.
 *
 * Renders into a scratch surface and reads it back, so this reports a
 * verdict rather than needing someone to look at a screen.
 */
void n64_rdp_selftest(void)
{
  enum { NTILES = 4, W = NTILES * 8, H = 8 };
  static u8  strip[NTILES * 32] __attribute__((aligned(16)));
  static u16 tlut[16]           __attribute__((aligned(16)));
  surface_t tex, target;
  u32 k, x, y, bad = 0, checked = 0;

  /* index(x,y,k) = (x + y + k) & 15, packed GBA-style: low nibble left */
  for (k = 0; k < NTILES; k++)
    for (y = 0; y < 8; y++)
      for (x = 0; x < 8; x += 2) {
        u32 lo = (x + y + k) & 15, hi = (x + 1 + y + k) & 15;
        strip[k * 32 + y * 4 + x / 2] = (u8)((hi << 4) | lo);
      }
  for (k = 0; k < 16; k++) tlut[k] = (u16)((k << 11) | (k << 6) | (k << 1) | 1);

  tex    = surface_make_linear(strip, FMT_CI4, 8, NTILES * 8);
  target = surface_alloc(FMT_RGBA16, W, H);
  if (!target.buffer) { debugf("[gpSP]: RDP selftest: no surface\n"); return; }

  data_cache_hit_writeback_invalidate(strip, sizeof(strip));
  data_cache_hit_writeback_invalidate(tlut, sizeof(tlut));

  rdpq_attach_clear(&target, NULL);
  rdpq_set_mode_standard();
  rdpq_mode_tlut(TLUT_RGBA16);
  rdpq_tex_upload_tlut(tlut, 0, 16);
  rdpq_tex_upload(TILE0, &tex, NULL);
  for (k = 0; k < NTILES; k++)
    rdpq_texture_rectangle(TILE0, k * 8, 0, k * 8 + 8, 8, 0, k * 8);
  rdpq_detach_wait();

  data_cache_hit_invalidate(target.buffer, target.stride * H);

  for (k = 0; k < NTILES; k++)
    for (y = 0; y < 8; y++)
      for (x = 0; x < 8; x++) {
        u16 got = ((u16 *)((u8 *)target.buffer + y * target.stride))[k * 8 + x];
        u16 want = tlut[(x + y + k) & 15];
        checked++;
        if (got != want && bad++ < 4)
          debugf("[gpSP]: RDP selftest mismatch tile%lu (%lu,%lu): "
                 "got %04x want %04x\n",
                 (unsigned long)k, (unsigned long)x, (unsigned long)y, got, want);
      }

  if (!bad)
    debugf("[gpSP]: RDP selftest OK -- CI4 nibble order, 8-wide VRAM strip "
           "addressing and TLUT all verified over %lu px\n",
           (unsigned long)checked);
  else
    debugf("[gpSP]: RDP selftest FAILED: %lu of %lu px\n",
           (unsigned long)bad, (unsigned long)checked);

  surface_free(&target);
}

void n64_rdp_bench(void)
{
  static u8 tilemem[8 * 8 / 2] __attribute__((aligned(16)));   /* 8x8 @ 4bpp */
  static u16 tlut[16] __attribute__((aligned(16)));
  surface_t tile;
  surface_t *disp;
  u32 i, t0, t1, cyc_draw, cyc_upload;

  for (i = 0; i < sizeof(tilemem); i++) tilemem[i] = (u8)(i * 17 + 3);
  for (i = 0; i < 16; i++) tlut[i] = (u16)((i * 0x1111) | 1);

  tile = surface_make_linear(tilemem, FMT_CI4, 8, 8);

  disp = display_get();
  if (!disp) { debugf("[gpSP]: RDP bench: no display\n"); return; }

  rdpq_attach(disp, NULL);
  rdpq_set_mode_standard();
  rdpq_mode_tlut(TLUT_RGBA16);
  rdpq_tex_upload_tlut(tlut, 0, 16);
  rdpq_tex_upload(TILE0, &tile, NULL);

  /* (a) marginal cost of one textured rectangle, tile already in TMEM */
  rdpq_texture_rectangle(TILE0, 0, 0, 8, 8, 0, 0);   /* warm */
  t0 = RDPB_TICK();
  for (i = 0; i < RDPB_TILES_PER_FRAME; i++) {
    u32 x = (i % 30) * 8, y = ((i / 30) % 20) * 8;
    rdpq_texture_rectangle(TILE0, x, y, x + 8, y + 8, 0, 0);
  }
  t1 = RDPB_TICK();
  cyc_draw = (t1 - t0) * 2;

  /* (b) the same, but reloading TMEM for every tile -- the worst case,
   *     where no tile in the scene repeats and nothing can be cached. */
  t0 = RDPB_TICK();
  for (i = 0; i < RDPB_TILES_PER_FRAME; i++) {
    u32 x = (i % 30) * 8, y = ((i / 30) % 20) * 8;
    rdpq_tex_upload(TILE0, &tile, NULL);
    rdpq_texture_rectangle(TILE0, x, y, x + 8, y + 8, 0, 0);
  }
  t1 = RDPB_TICK();
  cyc_upload = (t1 - t0) * 2;

  /* (c) The number the design actually turns on.  Per-tile uploads cost
   *     ~2200 cycles for 32 bytes, which is command overhead rather than
   *     bandwidth -- so one bulk upload of many tiles should cost about
   *     the same as one tile.  TMEM's texture half is 2 KB with a TLUT
   *     resident, which is 64 4bpp tiles; if a whole atlas loads for the
   *     price of one tile, the renderer uploads a few times per frame
   *     instead of 1,600 times, and the 37 ms collapses. */
  {
    static u8 atlasmem[64 * 64 / 2] __attribute__((aligned(16)));  /* 2 KB */
    surface_t atlas;
    u32 j, t0a, t1a, cyc_atlas, cyc_single;
    for (j = 0; j < sizeof(atlasmem); j++) atlasmem[j] = (u8)(j * 7 + 1);
    atlas = surface_make_linear(atlasmem, FMT_CI4, 64, 64);

    rdpq_tex_upload(TILE0, &atlas, NULL);          /* warm */
    t0a = RDPB_TICK();
    for (j = 0; j < 25; j++) rdpq_tex_upload(TILE0, &atlas, NULL);
    t1a = RDPB_TICK();
    cyc_atlas = ((t1a - t0a) * 2) / 25;

    rdpq_tex_upload(TILE0, &tile, NULL);           /* warm */
    t0a = RDPB_TICK();
    for (j = 0; j < 25; j++) rdpq_tex_upload(TILE0, &tile, NULL);
    t1a = RDPB_TICK();
    cyc_single = ((t1a - t0a) * 2) / 25;

    debugf("[gpSP]:   TMEM upload: 8x8 tile (32 B) %lu cyc, "
           "64x64 atlas (2 KB, 64 tiles) %lu cyc\n",
           (unsigned long)cyc_single, (unsigned long)cyc_atlas);
    debugf("[gpSP]:   -> %lu cyc per tile via atlas vs %lu individually\n",
           (unsigned long)(cyc_atlas / 64), (unsigned long)cyc_single);
  }

  /* (d) Can TMEM be filled straight from GBA VRAM, with no gather?
   *
   *     This is what separates the RDP idea from the RSP one that failed:
   *     that attempt had to copy tile bytes into a staging buffer, and
   *     the copying cost more than the rasterisation it replaced.
   *
   *     GBA tiles are tile-major -- 32 consecutive bytes per 8x8 tile,
   *     4 bytes a row -- while an RDP texture is row-major, so a 64x64
   *     view of a tile run would scramble it.  An 8-pixel-wide strip
   *     lines up exactly: tile k is rows 8k..8k+7, drawn with t = 8k.
   *     The open question is how many tiles fit, since TMEM rows are
   *     8-byte aligned and a CI4 row of 8 pixels is only 4 bytes. */
  {
    static u8 vramlike[8 * 1024] __attribute__((aligned(16)));
    u32 h, j, t0b, t1b;
    for (j = 0; j < sizeof(vramlike); j++) vramlike[j] = (u8)(j * 11 + 5);
    for (h = 64; h <= 1024; h <<= 1) {
      surface_t strip = surface_make_linear(vramlike, FMT_CI4, 8, h);
      int bytes = rdpq_tex_upload(TILE0, &strip, NULL);
      rdpq_tex_upload(TILE0, &strip, NULL);            /* warm */
      t0b = RDPB_TICK();
      for (j = 0; j < 25; j++) rdpq_tex_upload(TILE0, &strip, NULL);
      t1b = RDPB_TICK();
      debugf("[gpSP]:   VRAM strip 8x%-4lu = %-3lu tiles: %lu TMEM bytes, "
             "%lu cyc (%lu cyc/tile)\n",
             (unsigned long)h, (unsigned long)(h / 8), (unsigned long)bytes,
             (unsigned long)(((t1b - t0b) * 2) / 25),
             (unsigned long)((((t1b - t0b) * 2) / 25) / (h / 8)));
    }
  }

  rdpq_detach_wait();

  debugf("[gpSP]: RDP bench, %d tiles (one GBA frame of BG):\n",
         RDPB_TILES_PER_FRAME);
  debugf("[gpSP]:   draw only      %lu cyc = %lu.%02lu ms  (%lu cyc/tile)\n",
         (unsigned long)cyc_draw,
         (unsigned long)(cyc_draw / 93750), (unsigned long)((cyc_draw % 93750) * 100 / 93750),
         (unsigned long)(cyc_draw / RDPB_TILES_PER_FRAME));
  debugf("[gpSP]:   upload + draw  %lu cyc = %lu.%02lu ms  (%lu cyc/tile)\n",
         (unsigned long)cyc_upload,
         (unsigned long)(cyc_upload / 93750), (unsigned long)((cyc_upload % 93750) * 100 / 93750),
         (unsigned long)(cyc_upload / RDPB_TILES_PER_FRAME));
  debugf("[gpSP]:   (CPU cost only -- ares does not time RDP fills.\n");
  debugf("[gpSP]:    Replacing 27 ms of CPU rasterisation.)\n");
}

#endif  /* N64_RDP_BENCH */
