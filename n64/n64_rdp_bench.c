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
