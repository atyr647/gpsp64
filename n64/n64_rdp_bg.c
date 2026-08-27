/* gameplaySP - N64: GBA backgrounds on the RDP
 *
 * BG rasterisation is 49.5% of a 55 ms frame -- 27 ms spent turning
 * tilemaps into pixels on a 93.75 MHz in-order CPU, which is exactly the
 * job the N64 has dedicated silicon for.  Every attempt to make the CPU
 * version cheaper has failed (the RSP offload's floor was 13 ms *worse*,
 * a palette LUT is worth 4 ms at most), so the remaining move is to stop
 * doing it on the CPU at all.
 *
 * The mechanics were all verified against a CPU reference first, in
 * n64/n64_rdp_bench.c, because three of them are not obvious:
 *
 *   - the RDP takes the HIGH nibble of a 4bpp byte as the left pixel and
 *     the GBA packs the low nibble left, so tile bytes need a nibble
 *     swap.  gba_memory.c keeps a swapped VRAM shadow up to date on the
 *     write path, so this costs 0.05 ms/frame rather than a per-frame
 *     conversion pass;
 *   - GBA tiles are tile-major, 32 bytes each, which scrambles under any
 *     row-major texture view *except* an 8-pixel-wide strip, where tile k
 *     lands exactly at t = 8k.  So TMEM loads straight from the shadow
 *     with no gather -- the copy that sank the RSP attempt does not exist
 *     here;
 *   - flips need no tile descriptor at all: a texture rectangle carries
 *     its own signed ds/dx and dt/dy, so walking the texture backwards
 *     flips it inline.  That matters because only 1% of tiles are
 *     flipped but 38% of scanlines contain one, so declining them would
 *     have cost a third of the screen.
 *
 * TMEM holds 2 KB of texture with a TLUT resident: 32 tiles, one 1 KB
 * slice of the shadow, ~1725 cycles to load however many of its tiles get
 * used.  So draws are sorted by (slice, palette) across the whole frame,
 * and each slice loads once.  The GBA's 256-colour BG palette maps 1:1
 * onto the RDP's 256-entry TLUT -- 16 sub-palettes, 16 windows -- so
 * palette selection is a descriptor field, not a reload.
 *
 * N64 port Copyright (C) 2026
 */

#include <libdragon.h>
#include <string.h>
#include "../common.h"
#include "n64_video.h"

#ifdef N64_RDP_BG

extern u8 vram_swapped[1024 * 96];

/* The attached framebuffer for the frame in progress, or NULL when the
 * RDP has nothing to draw and the CPU renderer owns the whole screen. */
static surface_t *rdpbg_disp = NULL;
static int rdpbg_attached = 0;

/* Sorted draw list.  A GBA screen is 30x20 tiles, but a scroll that is
 * not a multiple of 8 pulls in a partial tile on each edge, so the
 * visible grid is 31x21 = 651 per layer and ~1600 for a typical frame. */
#define RDPBG_MAX_DRAWS 2600
typedef struct {
  s16 x, y;        /* screen position of the tile's top-left corner */
  u8  y0, y1;      /* rows [y0,y1) of the tile to actually draw      */
  u16 key;         /* (slice << 4) | palette -- the sort key         */
  u16 vt;          /* absolute VRAM tile number                      */
  u8  flip;        /* bit0 = h, bit1 = v                             */
  u8  pad;
} rdpbg_draw_t;

static rdpbg_draw_t rdpbg_draws[RDPBG_MAX_DRAWS];
static u32 rdpbg_ndraws = 0;

u32 n64_rdpbg_slices = 0, n64_rdpbg_groups = 0, n64_rdpbg_tiles = 0;
u32 n64_rdpbg_frames = 0, n64_rdpbg_overflow = 0;
/* Where the CPU half of the frame actually goes.  The whole point of
 * this renderer is to move work off the VR4300, so it matters which of
 * the three remaining costs -- walking the tilemap, generating rdpq
 * commands, or waiting for the RDP to finish -- is the one left. */
u32 n64_rdpbg_t_sort = 0, n64_rdpbg_t_emit = 0, n64_rdpbg_t_wait = 0;
#define RDPBG_TICK() ({ u32 _t; __asm__ volatile("mfc0 %0, $9" : "=r"(_t)); _t; })

/* XBGR1555 (as gpSP's converted palette holds it) -> RGBA5551, which is
 * what a 16-bit N64 framebuffer wants.  Index 0 of every sub-palette is
 * the GBA's transparent colour, so its alpha bit is cleared and the RDP
 * discards it on alpha compare. */
static u16 rdpbg_tlut[256] __attribute__((aligned(16)));

void n64_rdpbg_build_tlut(const u16 *pal_converted)
{
  u32 i;
  for (i = 0; i < 256; i++) {
    u16 v = pal_converted[i];
    u16 r = (u16)(v & 0x1F), g = (u16)((v >> 5) & 0x1F), b = (u16)((v >> 10) & 0x1F);
    rdpbg_tlut[i] = (u16)((r << 11) | (g << 6) | (b << 1) | ((i & 15) ? 1 : 0));
  }
}

int n64_rdpbg_begin(void)
{
  extern surface_t *n64_video_acquire(void);
  rdpbg_ndraws = 0;
  rdpbg_disp = n64_video_acquire();
  return rdpbg_disp != NULL;
}

void n64_rdpbg_add(int x, int y, int y0, int y1, u32 vt, u32 pal, u32 flip)
{
  rdpbg_draw_t *d;
  if (rdpbg_ndraws >= RDPBG_MAX_DRAWS) { n64_rdpbg_overflow++; return; }
  d = &rdpbg_draws[rdpbg_ndraws++];
  d->x = (s16)x; d->y = (s16)y;
  d->y0 = (u8)y0; d->y1 = (u8)y1;
  d->vt = (u16)vt;
  d->flip = (u8)flip;
  d->key = (u16)(((vt >> 5) << 4) | pal);
}

/* Counting sort by (slice, palette).  1024 buckets is 2 KB of counters,
 * cheaper to clear than any comparison sort is to run on 1600 items --
 * and the emit loop then walks slices in order, so each 1 KB slice of
 * the VRAM shadow is uploaded to TMEM exactly once per frame. */
static u16 rdpbg_count[1024];
static u16 rdpbg_order[RDPBG_MAX_DRAWS];

void n64_rdpbg_flush(void)
{
  u32 i, n = rdpbg_ndraws, sum = 0;
  u32 cur_slice = 0xFFFF, cur_key = 0xFFFF;

  if (!rdpbg_disp) return;
  if (!n) return;

  u32 _t0 = RDPBG_TICK();
  memset(rdpbg_count, 0, sizeof(rdpbg_count));
  for (i = 0; i < n; i++) rdpbg_count[rdpbg_draws[i].key]++;
  for (i = 0; i < 1024; i++) { u32 c = rdpbg_count[i]; rdpbg_count[i] = (u16)sum; sum += c; }
  for (i = 0; i < n; i++) rdpbg_order[rdpbg_count[rdpbg_draws[i].key]++] = (u16)i;

  n64_rdpbg_t_sort += RDPBG_TICK() - _t0;
  _t0 = RDPBG_TICK();

  if (!rdpbg_attached) { rdpq_attach(rdpbg_disp, NULL); rdpbg_attached = 1; }
  rdpq_set_mode_standard();
  rdpq_mode_tlut(TLUT_RGBA16);
  rdpq_mode_alphacompare(1);          /* index 0 of each sub-palette is transparent */
  data_cache_hit_writeback(rdpbg_tlut, sizeof(rdpbg_tlut));
  rdpq_tex_upload_tlut(rdpbg_tlut, 0, 256);

  for (i = 0; i < n; i++) {
    const rdpbg_draw_t *d = &rdpbg_draws[rdpbg_order[i]];
    u32 slice = d->key >> 4;
    int x0, y0, x1, y1, s0, t0, dsdx, dtdy;

    if (slice != cur_slice) {
      surface_t sl = surface_make_linear(&vram_swapped[slice * 1024],
                                         FMT_CI4, 8, 256);
      rdpq_tex_upload(TILE0, &sl, NULL);
      cur_slice = slice; cur_key = 0xFFFF;
      n64_rdpbg_slices++;
    }
    if (d->key != cur_key) {
      rdpq_tileparms_t p = {0};
      p.palette = (u8)(d->key & 15);
      rdpq_set_tile(TILE0, FMT_CI4, 0, 8, &p);
      rdpq_set_tile_size(TILE0, 0, 0, 8, 256);
      cur_key = d->key;
      n64_rdpbg_groups++;
    }

    /* The tile's own rows [y0,y1) are drawn; the caller has already
     * clipped that range to the rows the RDP owns this frame.  Columns
     * clip against the 240-pixel screen the same way, since a scrolled
     * layer's first and last tiles hang off the edges. */
    x0 = d->x; x1 = d->x + 8;
    y0 = d->y + d->y0; y1 = d->y + d->y1;
    s0 = 0; t0 = (int)((d->vt & 31) * 8) + d->y0;
    dsdx = 1; dtdy = 1;

    if (d->flip & 1) { s0 = 7; dsdx = -1; }
    if (d->flip & 2) { t0 = (int)((d->vt & 31) * 8) + (7 - d->y0); dtdy = -1; }

    if (x0 < 0)   { if (dsdx > 0) s0 -= x0; else s0 += x0; x0 = 0; }
    if (x1 > 240) x1 = 240;
    if (x0 >= x1) continue;

    rdpq_texture_rectangle_raw(TILE0,
      GBA_OFFSET_X + x0, GBA_OFFSET_Y + y0, GBA_OFFSET_X + x1, GBA_OFFSET_Y + y1,
      s0, t0, dsdx, dtdy);
    n64_rdpbg_tiles++;
  }

  n64_rdpbg_t_emit += RDPBG_TICK() - _t0;
  rdpbg_ndraws = 0;
  n64_rdpbg_frames++;
}

/* Backdrop: GBA palette entry 0, painted under the layers for the rows
 * the RDP owns.  gpSP's CPU renderer gets this for free by having the
 * bottom layer write opaquely; on the RDP the layers are all alpha-tested,
 * so the floor has to be laid first. */
void n64_rdpbg_backdrop(int y0, int y1)
{
  if (!rdpbg_disp || y0 >= y1) return;
  if (!rdpbg_attached) { rdpq_attach(rdpbg_disp, NULL); rdpbg_attached = 1; }
  rdpq_set_mode_fill(color_from_packed16(rdpbg_tlut[0] | 1));
  rdpq_fill_rectangle(GBA_OFFSET_X, GBA_OFFSET_Y + y0, GBA_OFFSET_X + 240, GBA_OFFSET_Y + y1);
}

/* Hand the frame back.  The CPU still owns any row the RDP declined, and
 * writes those straight into the same framebuffer, so the RDP has to be
 * finished before the blit touches them. */
surface_t *n64_rdpbg_end(void)
{
  surface_t *d = rdpbg_disp;
  if (rdpbg_attached) {
    u32 _t = RDPBG_TICK();
    rdpq_detach_wait();
    n64_rdpbg_t_wait += RDPBG_TICK() - _t;
    rdpbg_attached = 0;
  }
  rdpbg_disp = NULL;
  return d;
}

#endif  /* N64_RDP_BG */
