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
/* 8 bytes, packed.
 *
 * This list is written once and read back through an index gather, so
 * its size is memory traffic twice over -- 1,217 draws a frame, and the
 * gather is random over the whole array.  It showed up in the D-cache
 * attribution at 7% of all misses.
 *
 * The sort key does not need storing: it is (slice << 4) | palette and
 * the slice is just vt >> 5, so it is two shifts from fields already
 * here.  y0/y1 are 0..8 and palette is 0..15, so they pack into a nibble
 * each.  12 bytes -> 8. */
typedef struct {
  s16 x, y;        /* screen position of the tile's top-left corner */
  u8  yy;          /* y0 | (y1 << 4): rows [y0,y1) of the tile to draw */
  u8  pf;          /* palette | (flip << 4); flip bit0 = h, bit1 = v */
  u16 vt;          /* absolute VRAM tile number */
} rdpbg_draw_t;

#define RDPBG_KEY(d)  ((u16)((((d)->vt >> 5) << 4) | ((d)->pf & 15)))
#define RDPBG_Y0(d)   ((d)->yy & 15)
#define RDPBG_Y1(d)   ((d)->yy >> 4)
#define RDPBG_FLIP(d) ((d)->pf >> 4)

static rdpbg_draw_t rdpbg_draws[RDPBG_MAX_DRAWS];
static u32 rdpbg_ndraws = 0;

u32 n64_rdpbg_slices = 0, n64_rdpbg_groups = 0, n64_rdpbg_tiles = 0;
u32 n64_rdpbg_tluts = 0;
u32 n64_rdpbg_frames = 0, n64_rdpbg_overflow = 0;
/* Where the CPU half of the frame actually goes.  The whole point of
 * this renderer is to move work off the VR4300, so it matters which of
 * the three remaining costs -- walking the tilemap, generating rdpq
 * commands, or waiting for the RDP to finish -- is the one left. */
u32 n64_rdpbg_t_sort = 0, n64_rdpbg_t_emit = 0, n64_rdpbg_t_wait = 0;
/* Non-destructive split of the slice-change block.  The time-weighted PC
 * profiler puts 23% of n64_rdpbg_flush there, but the sample sits on a
 * branch at the block merge point, so it cannot say whether the cost is
 * libdragon building the upload or RDPBG_SUBMIT's writeback + exec + two
 * pipeline syncs.  Wrapping each in COUNT answers it without changing a
 * single command, which matters: ablating the syncs or the upload would
 * change what the RDP does and what the canary reports. */
u32 n64_rdpbg_t_wb = 0, n64_rdpbg_t_exec = 0, n64_rdpbg_t_sync = 0,
    n64_rdpbg_t_upl = 0, n64_rdpbg_n_sub = 0;
#define RDPBG_TICK() ({ u32 _t; __asm__ volatile("mfc0 %0, $9" : "=r"(_t)); _t; })

/* XBGR1555 (as gpSP's converted palette holds it) -> RGBA5551, which is
 * what a 16-bit N64 framebuffer wants.  Index 0 of every sub-palette is
 * the GBA's transparent colour, so its alpha bit is cleared and the RDP
 * discards it on alpha compare. */
static u16 rdpbg_tlut[2][256] __attribute__((aligned(16)));
static int rdpbg_tlut_loaded = -1;

/* Two TLUTs, because the GBA has 256 BG colours and 256 OBJ colours and
 * the RDP has one 256-entry TLUT.  Whichever the current group needs is
 * uploaded when it changes, which is a handful of times per frame at the
 * BG/OBJ boundaries of the painter order. */
void n64_rdpbg_build_tlut(const u16 *pal_converted)
{
  u32 h, i;
  for (h = 0; h < 2; h++)
    for (i = 0; i < 256; i++) {
      u16 v = pal_converted[h * 256 + i];
      u16 r = (u16)(v & 0x1F), g = (u16)((v >> 5) & 0x1F), b = (u16)((v >> 10) & 0x1F);
      rdpbg_tlut[h][i] = (u16)((r << 11) | (g << 6) | (b << 1) | ((i & 15) ? 1 : 0));
    }
  rdpbg_tlut_loaded = -1;
}

/* Deliberately does NOT acquire the framebuffer.  This runs at scanline
 * 0, inside the PPU timer, and display_get() blocks until the VI releases
 * a buffer -- so acquiring here charged a vsync wait to BG rendering and
 * made the PPU look *more* expensive with 45% of its rows skipped.  The
 * buffer is taken at flush time, at the end of the frame, exactly where
 * the blit always took it. */
#ifdef N64_RDP_EXEC
/* Write the RDP commands into cached RAM and hand the whole run over with
 * rdpq_exec(), instead of pushing each one through rspq.
 *
 * MEASURED WORSE ON ares, AND OFF BY DEFAULT.  The reasoning was that
 * rspq's command buffers are uncached, so each textured rectangle costs
 * five uncached word stores, and that this was most of the ~250 cycles a
 * rectangle takes.  Writing the same words to cached memory and paying
 * one bulk writeback should then have been most of a 3.3 ms saving.
 * Instead emit went 3.27 -> 3.95 ms and the frame 38.7 -> 40.0.
 *
 * The likely reason is that ares does not model uncached store stalls at
 * all -- it models D-cache misses, which is why every cache experiment in
 * this port has read true, but a write-buffer stall costs nothing in its
 * CPU model.  So the rspq stores were already free in the measurement and
 * this only adds write-allocate misses and an explicit writeback.  On
 * console it could still win; there is no way to tell from here, and a
 * change that measures worse on the only instrument available does not
 * get to be the default.  Kept, behind -DN64_RDP_EXEC, for whoever has
 * hardware.
 *
 * The catch is that rdpq's autosync engine cannot see these rectangles,
 * so it will not know a SYNC_LOAD or SYNC_TILE is owed when the next TMEM
 * load or tile change comes along.  Those are issued explicitly at every
 * group boundary instead -- a couple of dozen per frame.
 *
 * The buffer is only rewritten from the start on the next frame, and the
 * frame ends with rdpq_detach_wait(), so the RDP is always done with it
 * before anything overwrites it.
 */
#define RDPBG_CMDWORDS (RDPBG_MAX_DRAWS * 4 + 16)
static u32 rdpbg_cmds[RDPBG_CMDWORDS] __attribute__((aligned(16)));
static u32 rdpbg_cw = 0;      /* next word to write   */
static u32 rdpbg_csent = 0;   /* first word not yet submitted */

/* 0xE4 is the RDP's own TEXTURE_RECTANGLE opcode; coordinates are 10.2,
 * texture coordinates 10.5, and the steps s5.10. */
#define RDPBG_RECT(X0, Y0, X1, Y1, S0, T0, DSDX, DTDY) do {                 \
    u32 *_p = &rdpbg_cmds[rdpbg_cw];                                        \
    _p[0] = 0xE4000000u | ((u32)((X1) * 4) << 12) | (u32)((Y1) * 4);        \
    _p[1] = ((u32)((X0) * 4) << 12) | (u32)((Y0) * 4);                      \
    _p[2] = ((u32)((S0) * 32) << 16) | (u32)(((T0) * 32) & 0xFFFF);         \
    _p[3] = ((u32)(((DSDX) * 1024) & 0xFFFF) << 16)                         \
          | (u32)(((DTDY) * 1024) & 0xFFFF);                                \
    rdpbg_cw += 4;                                                          \
  } while (0)

/* Hand over everything written since the last submit, then let rdpq know
 * the pipeline has been used so its next TMEM load or tile change is
 * ordered after these rectangles. */
#define RDPBG_SUBMIT() do {                                                 \
    if (rdpbg_cw > rdpbg_csent) {                                           \
      u32 _n = rdpbg_cw - rdpbg_csent, _a, _b;                              \
      n64_rdpbg_n_sub++;                                                    \
      _a = RDPBG_TICK();                                                    \
      data_cache_hit_writeback(&rdpbg_cmds[rdpbg_csent], _n * 4);           \
      _b = RDPBG_TICK(); n64_rdpbg_t_wb += _b - _a;                         \
      rdpq_exec(&rdpbg_cmds[rdpbg_csent], (int)(_n * 4));                   \
      _a = RDPBG_TICK(); n64_rdpbg_t_exec += _a - _b;                       \
      rdpbg_csent = rdpbg_cw;                                               \
      rdpq_sync_load();                                                     \
      rdpq_sync_tile();                                                     \
      n64_rdpbg_t_sync += RDPBG_TICK() - _a;                                \
    }                                                                       \
  } while (0)
#else
#define RDPBG_RECT(X0, Y0, X1, Y1, S0, T0, DSDX, DTDY) \
  rdpq_texture_rectangle_raw(TILE0, X0, Y0, X1, Y1, S0, T0, DSDX, DTDY)
#define RDPBG_SUBMIT() do {} while (0)
#endif

int n64_rdpbg_begin(void)
{
  rdpbg_ndraws = 0;
#ifdef N64_RDP_EXEC
  rdpbg_cw = rdpbg_csent = 0;
#endif
  return 1;
}

void n64_rdpbg_add(int x, int y, int y0, int y1, u32 vt, u32 pal, u32 flip)
{
  rdpbg_draw_t *d;
  if (rdpbg_ndraws >= RDPBG_MAX_DRAWS) { n64_rdpbg_overflow++; return; }
  d = &rdpbg_draws[rdpbg_ndraws++];
  d->x = (s16)x; d->y = (s16)y;
  d->yy = (u8)(y0 | (y1 << 4));
  d->pf = (u8)(pal | (flip << 4));
  d->vt = (u16)vt;
}

/* Counting sort by (slice, palette).  1024 buckets is 2 KB of counters,
 * cheaper to clear than any comparison sort is to run on 1600 items --
 * and the emit loop then walks slices in order, so each 1 KB slice of
 * the VRAM shadow is uploaded to TMEM once per layer.
 *
 * Once per *layer*, not once per frame: the RDP has no per-pixel priority
 * beyond draw order, so the GBA's back-to-front layer order has to survive
 * into the command stream.  Sorting the whole frame by slice was cheaper
 * -- 5 TMEM loads instead of 20 -- and wrong, because it let a background
 * layer be drawn after the one that should cover it.  Anywhere two layers
 * both had an opaque pixel, whichever happened to sort later won.  So the
 * caller flushes per layer and the sort only ever reorders within one. */
/* 96 slices (the whole 96 KB VRAM shadow, OBJ tiles included) times 16
 * palettes.  Kept zeroed between calls so it never has to be memset. */
static u16 rdpbg_count[96 * 16];
#define RDPBG_MAX_KEYS 64
static u16 rdpbg_order[RDPBG_MAX_DRAWS];

void n64_rdpbg_flush(int obj_palette, int sortable)
{
  extern surface_t *n64_video_acquire(void);
  u32 i, n = rdpbg_ndraws, sum = 0;
  u32 cur_slice = 0xFFFF, cur_key = 0xFFFF;

  if (!n) return;
  rdpbg_disp = n64_video_acquire();
  if (!rdpbg_disp) return;

  u32 _t0 = RDPBG_TICK();
  /* Sorting is only safe within a background layer.  A layer's own tiles
   * never overlap each other, so reordering them by TMEM slice is free --
   * but two sprites can overlap, and the GBA resolves that by OAM index,
   * so the order they were added in *is* the answer.  Sprites are emitted
   * as they come and pay for the extra TMEM loads; a sprite's tiles are
   * consecutive under 1D mapping, so that is a load or two per sprite. */
  if (sortable) {
    /* Only a handful of (slice, palette) keys are ever live in one layer
     * -- 27 across a whole frame -- so clearing and prefix-summing all
     * 1536 buckets costs far more than the sort itself.  Collect the
     * distinct keys instead, sort those, and prefix-sum over them; the
     * bucket array is left clean for the next call by zeroing exactly the
     * entries that were touched. */
    u32 nk = 0, k, j;
    u16 keys[RDPBG_MAX_KEYS];
    for (i = 0; i < n; i++) {
      k = RDPBG_KEY(&rdpbg_draws[i]);
      if (!rdpbg_count[k]) {
        if (nk >= RDPBG_MAX_KEYS) { sortable = 0; break; }
        keys[nk++] = (u16)k;
      }
      rdpbg_count[k]++;
    }
    if (sortable) {
      for (i = 1; i < nk; i++) {              /* insertion sort, nk is tiny */
        u16 v = keys[i];
        for (j = i; j && keys[j - 1] > v; j--) keys[j] = keys[j - 1];
        keys[j] = v;
      }
      for (i = 0; i < nk; i++) {
        u32 c = rdpbg_count[keys[i]];
        rdpbg_count[keys[i]] = (u16)sum;
        sum += c;
      }
      for (i = 0; i < n; i++)
        rdpbg_order[rdpbg_count[RDPBG_KEY(&rdpbg_draws[i])]++] = (u16)i;
      for (i = 0; i < nk; i++) rdpbg_count[keys[i]] = 0;
    } else {
      for (i = 0; i < nk; i++) rdpbg_count[keys[i]] = 0;
      memset(rdpbg_count, 0, sizeof(rdpbg_count));
      for (i = 0; i < n; i++) rdpbg_order[i] = (u16)i;
    }
  } else {
    for (i = 0; i < n; i++) rdpbg_order[i] = (u16)i;
  }

  n64_rdpbg_t_sort += RDPBG_TICK() - _t0;
  _t0 = RDPBG_TICK();

  if (!rdpbg_attached) { rdpq_attach(rdpbg_disp, NULL); rdpbg_attached = 1; }
  rdpq_set_mode_standard();
  rdpq_mode_tlut(TLUT_RGBA16);
  rdpq_mode_alphacompare(1);          /* index 0 of each sub-palette is transparent */
  if (rdpbg_tlut_loaded != obj_palette) {
    data_cache_hit_writeback(rdpbg_tlut[obj_palette], 512);
    rdpq_tex_upload_tlut(rdpbg_tlut[obj_palette], 0, 256);
    rdpbg_tlut_loaded = obj_palette;
    n64_rdpbg_tluts++;
  }

  for (i = 0; i < n; i++) {
    const rdpbg_draw_t *d = &rdpbg_draws[rdpbg_order[i]];
    u32 key = RDPBG_KEY(d), y0f = RDPBG_Y0(d), y1f = RDPBG_Y1(d);
    u32 flip = RDPBG_FLIP(d);
    u32 slice = key >> 4;
    int x0, y0, x1, y1, s0, t0, dsdx, dtdy;

    if (slice != cur_slice) {
      surface_t sl = surface_make_linear(&vram_swapped[slice * 1024],
                                         FMT_CI4, 8, 256);
      RDPBG_SUBMIT();
      { u32 _u = RDPBG_TICK();
        rdpq_tex_upload(TILE0, &sl, NULL);
        n64_rdpbg_t_upl += RDPBG_TICK() - _u; }
      cur_slice = slice; cur_key = 0xFFFF;
      n64_rdpbg_slices++;
    }
    if (key != cur_key) {
      rdpq_tileparms_t p = {0};
      p.palette = (u8)(key & 15);
      RDPBG_SUBMIT();
      rdpq_set_tile(TILE0, FMT_CI4, 0, 8, &p);
      rdpq_set_tile_size(TILE0, 0, 0, 8, 256);
      cur_key = key;
      n64_rdpbg_groups++;
    }

    /* The tile's own rows [y0,y1) are drawn; the caller has already
     * clipped that range to the rows the RDP owns this frame.  Columns
     * clip against the 240-pixel screen the same way, since a scrolled
     * layer's first and last tiles hang off the edges. */
    x0 = d->x; x1 = d->x + 8;
    y0 = d->y + (int)y0f; y1 = d->y + (int)y1f;
    s0 = 0; t0 = (int)((d->vt & 31) * 8) + (int)y0f;
    dsdx = 1; dtdy = 1;

    if (flip & 1) { s0 = 7; dsdx = -1; }
    if (flip & 2) { t0 = (int)((d->vt & 31) * 8) + (7 - (int)y0f); dtdy = -1; }

    if (x0 < 0)   { if (dsdx > 0) s0 -= x0; else s0 += x0; x0 = 0; }
    if (x1 > 240) x1 = 240;
    if (x0 >= x1) continue;

    RDPBG_RECT(GBA_OFFSET_X + x0, GBA_OFFSET_Y + y0,
               GBA_OFFSET_X + x1, GBA_OFFSET_Y + y1, s0, t0, dsdx, dtdy);
    n64_rdpbg_tiles++;
  }

  RDPBG_SUBMIT();
  n64_rdpbg_t_emit += RDPBG_TICK() - _t0;
  rdpbg_ndraws = 0;
}

/* Backdrop: GBA palette entry 0, painted under the layers for the rows
 * the RDP owns.  gpSP's CPU renderer gets this for free by having the
 * bottom layer write opaquely; on the RDP the layers are all alpha-tested,
 * so the floor has to be laid first. */
void n64_rdpbg_backdrop(int y0, int y1)
{
  extern surface_t *n64_video_acquire(void);
  if (y0 >= y1) return;
  if (!rdpbg_disp) rdpbg_disp = n64_video_acquire();
  if (!rdpbg_disp) return;
  if (!rdpbg_attached) { rdpq_attach(rdpbg_disp, NULL); rdpbg_attached = 1; }
  rdpq_set_mode_fill(color_from_packed16(rdpbg_tlut[0][0] | 1));
  rdpq_fill_rectangle(GBA_OFFSET_X, GBA_OFFSET_Y + y0, GBA_OFFSET_X + 240, GBA_OFFSET_Y + y1);
}

/* Hand the frame back.  The CPU still owns any row the RDP declined, and
 * writes those straight into the same framebuffer, so the RDP has to be
 * finished before the blit touches them. */
surface_t *n64_rdpbg_end(void)
{
  surface_t *d = rdpbg_disp;
  /* Count frames here, not in flush().  flush() runs once per layer and
   * once per sprite priority group, so counting there divided every
   * per-frame figure by the number of groups -- which is why the tile
   * count appeared to *fall* when coverage went from 45% to 100%. */
  n64_rdpbg_frames++;
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
