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
#include <rspq.h>
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
/* -DRDPBG_R=1|2|3 splits the ~95 COUNT ticks a tile costs after CDE into
 * record load, arithmetic/clip/flip, and the two command stores.  Three
 * separate binaries, one range each: six mfc0s around a 95-tick body would
 * be a tenth of the thing being measured.  Grouping is untouched, so the
 * 21-slice / 0.59 ms upload structure holds and this is not another
 * slice-count experiment.
 *
 * RESULT, and the reason not to re-run this: it is below the instrument's
 * floor.  COUNT ticks once per two PClock and a back-to-back mfc0 pair plus
 * the accumulate costs ~6 ticks a tile -- measurable as ~0.16 ms/frame of
 * extra emit in every probe build -- so a range shorter than the probe reads
 * as 0.00.  Ranges 1 and 3 did.  They are bounded above by ~6 ticks each,
 * not zero.  Only range 2, which contains branches, is long enough to
 * resolve: 13.5 ticks.  Splitting a ~95-tick body any finer than this needs
 * a different instrument. */
u32 n64_rdpbg_t_r = 0;
u32 n64_rdpbg_t_wb = 0, n64_rdpbg_t_exec = 0, n64_rdpbg_t_sync = 0,
    n64_rdpbg_t_upl = 0, n64_rdpbg_n_sub = 0;
/* Time spent blocked on rspq_syncpoint_wait() in the live RSP path below --
 * the CPU-side cost of the one-batch-deep pipeline actually not overlapping
 * (a full pipeline would keep this near zero; a pipeline that has degenerated
 * into lockstep submit/wait would show it equal to the RSP's own compute
 * time). Meaningless, and left at 0, when N64_RSP_RDPBG_LIVE is off. */
u32 n64_rdpbg_t_rspwait = 0;
#define RDPBG_TICK() ({ u32 _t; __asm__ volatile("mfc0 %0, $9" : "=r"(_t)); _t; })
/* Same read, but a full compiler barrier.  RDPBG_TICK has no memory clobber,
 * so GCC is free to schedule loads and stores across it -- which is exactly
 * what it did the first time this split was run: ranges 1 and 3 measured
 * 0.00 ms because the record load and the command stores had been hoisted
 * out from between the two reads.  Wrapping a handful of instructions needs
 * the barrier or the probe measures nothing. */
#define RDPBG_TICK_B() ({ u32 _t; __asm__ __volatile__("mfc0 %0, $9" \
                          : "=r"(_t) :: "memory"); _t; })

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

/* The command buffer is write-only from the CPU: 19.5 KB streamed through
 * an 8 KB cache every frame, and each rectangle fills exactly one aligned
 * 16-byte line (rdpbg_cmds is aligned(16) and rdpbg_cw only ever advances
 * by 4 words here).  A normal store therefore triggers a read-for-ownership
 * that fetches 16 bytes about to be overwritten in full.  Create Dirty
 * Exclusive (cache op 3, D-cache) claims the line without the fetch.
 * Build with -DRDPBG_NO_CDE to compare. */
#if defined(N64) && !defined(RDPBG_NO_CDE)
#define RDPBG_CLAIM_LINE(p) \
  __asm__ __volatile__ ("cache 0xD, 0(%0)" :: "r"(p) : "memory")
#else
#define RDPBG_CLAIM_LINE(p) do {} while (0)
#endif

/* 0xE4 is the RDP's own TEXTURE_RECTANGLE opcode; coordinates are 10.2,
 * texture coordinates 10.5, and the steps s5.10. */
#define RDPBG_RECT(X0, Y0, X1, Y1, S0, T0, DSDX, DTDY) do {                 \
    u32 *_p = &rdpbg_cmds[rdpbg_cw];                                        \
    RDPBG_CLAIM_LINE(_p);                                                   \
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

#ifdef N64_RSP_RDPBG_LIVE
/* Live RSP offload of the per-tile clip/flip/encode arithmetic below --
 * see rsp_rdpbg.S and n64/n64_rsp2.c.  Fixed-size output (every tile,
 * kept or degenerate, is exactly 16 bytes) means the CPU knows a batch's
 * footprint in rdpbg_cmds the instant it decides to queue it, without
 * waiting for the RSP to say how many tiles it actually kept -- so the
 * next batch's destination can be reserved immediately and the RSP can
 * be left to work while the CPU goes on gathering more batches.
 *
 * Up to RDPBG_RSP_DEPTH batches may be in flight at once: at every
 * tile/palette state change (and once more, draining everything left, at
 * end of frame in n64_rdpbg_end()), a new batch is queued and, only once
 * DEPTH are already outstanding, the *oldest* one is waited on and handed
 * to rdpq_exec first -- which is also the earliest point it is safe to
 * change tile state, since the RDP command stream only cares about the
 * order the CPU calls rdpq_exec/rdpq_tex_upload/rdpq_set_tile in, not the
 * order the RSP actually computed each batch's contents.  Draining oldest
 * first, in the same order batches were queued, is what keeps this
 * correct: rdpq_exec calls must reach the RDP command stream in the same
 * relative order their tiles were gathered in.
 *
 * A one-deep pipeline (the original version of this) could only hide one
 * batch's RSP round trip behind the next group's own gather time -- a
 * measured 2.4ms/frame of the 4.68ms loop was spent just blocked waiting,
 * because this renderer's (slice,palette) groups average only ~40 tiles,
 * nowhere near enough CPU-side gathering to cover one batch's RSP time.
 * Going DEPTH batches deep lets DEPTH groups' worth of gathering
 * accumulate against the oldest batch's RSP time instead of just one.
 * Measured on ares (overworld savestate, 17-25 steady windows each):
 *
 *   DEPTH=1   rspwait 2.40 ms/f   emit 4.68 ms/f   26.0 ms/f frame
 *   DEPTH=4   rspwait 1.10 ms/f   emit 3.40 ms/f   26.0 ms/f frame
 *   DEPTH=8   rspwait 0.06 ms/f   emit 2.83 ms/f   25.0 ms/f frame (24.6 mean)
 *
 * DEPTH=4's 1.3ms of reclaimed emit time did not show up in frame time at
 * all -- a smaller, unrelated shift in GBA-CPU-emulation time (the classic
 * direct-mapped-I-cache layout sensitivity this project has hit before,
 * see docs/CACHE_PROFILING.md) happened to cancel it out that run. DEPTH=8
 * reclaimed enough (rspwait down to noise) to show through regardless:
 * a real ~2ms/frame, +3 FPS win over the DEPTH=1 baseline. Shipped as the
 * default. DEPTH=16 was not tried -- rspwait at 8 is already down to where
 * going deeper has little left to reclaim.
 *
 * DEPTH=8 did not work until n64_rsp2.c's n64_rsp_rdpbg_queue() gained an
 * explicit rspq_flush() after rspq_write(): without it, DEPTH=8 hung
 * solid (ares PCSAMPLE stuck at one PC). Only rspq_syncpoint_wait is
 * documented to imply a flush, and at DEPTH=8 enough batches can queue up
 * before this code ever waits on one that the RSP apparently never
 * learned they existed. DEPTH=1 and 4 happened to wait often enough to
 * avoid this by accident.
 *
 * Gathering cycles through DEPTH+1 scratch buffers (not DEPTH): DEPTH of
 * them can be outstanding (queued, RSP still working or done but not yet
 * drained) at once, plus the one currently being filled by the CPU. Safe
 * to reuse a buffer once its previous occupant, DEPTH+1 batches back in
 * FIFO order, has been drained. */
extern bool n64_rsp_rdpbg_ready(void);
extern rspq_syncpoint_t n64_rsp_rdpbg_queue(const void *src, u32 count, void *dst);

/* Must match rsp_rdpbg.S's own RDPBG_RSP_MAXBATCH (IN_BUF/OUT_BUF sizing) --
 * there is no shared header between the ucode's assembler and this file. */
#define RDPBG_RSP_MAXBATCH 128

/* Tunable via EXTRA_CFLAGS=-DRDPBG_RSP_DEPTH=N for A/B testing without
 * editing this file.  Costs RDPBG_RSP_DEPTH+1 gather buffers of 1 KB each
 * -- trivial RAM, unlike the RSP's own fixed DMEM budget, which this
 * does not touch at all: each queued command still uses the same static
 * IN_BUF/OUT_BUF in turn, one command fully finishing before the next
 * starts, exactly as rspq already serialises them. */
#ifndef RDPBG_RSP_DEPTH
#define RDPBG_RSP_DEPTH 8
#endif
static rdpbg_draw_t rsp_gather[RDPBG_RSP_DEPTH + 1][RDPBG_RSP_MAXBATCH] __attribute__((aligned(16)));
static int rsp_gather_slot = 0;

typedef struct {
  rspq_syncpoint_t sp;
  u32 off;    /* word offset into rdpbg_cmds */
  u32 words;  /* word count (count*4) */
} rsp_pending_t;
static rsp_pending_t rsp_pending[RDPBG_RSP_DEPTH];
static int rsp_pending_head  = 0;  /* oldest not-yet-drained batch */
static int rsp_pending_count = 0;  /* batches outstanding, 0..RDPBG_RSP_DEPTH */

/* Wait for and submit the single oldest outstanding batch. */
static void rdpbg_rsp_drain_one(void)
{
  rsp_pending_t *p;
  u32 _a, _b, _c;
  if (!rsp_pending_count) return;
  p = &rsp_pending[rsp_pending_head];
  _a = RDPBG_TICK();
  rspq_syncpoint_wait(p->sp);
  _b = RDPBG_TICK(); n64_rdpbg_t_rspwait += _b - _a;
  rdpq_exec(&rdpbg_cmds[p->off], (int)(p->words * 4));
  _c = RDPBG_TICK(); n64_rdpbg_t_exec += _c - _b;
  rdpq_sync_load();
  rdpq_sync_tile();
  n64_rdpbg_t_sync += RDPBG_TICK() - _c;
  n64_rdpbg_n_sub++;
  rsp_pending_head = (rsp_pending_head + 1) % RDPBG_RSP_DEPTH;
  rsp_pending_count--;
}

/* If DEPTH batches are already outstanding, drain the oldest first, then
 * queue the batch just finished gathering (rsp_gather[rsp_gather_slot],
 * "count" records) as the newest pending one, reserving its output slot
 * in rdpbg_cmds up front. */
static void rdpbg_rsp_close_batch_from(const rdpbg_draw_t *recs, u32 count)
{
  void *dst;
  int tail;
  if (rsp_pending_count >= RDPBG_RSP_DEPTH)
    rdpbg_rsp_drain_one();
  dst = &rdpbg_cmds[rdpbg_cw];
  tail = (rsp_pending_head + rsp_pending_count) % RDPBG_RSP_DEPTH;
  rsp_pending[tail].sp    = n64_rsp_rdpbg_queue(recs, count, dst);
  rsp_pending[tail].off   = rdpbg_cw;
  rsp_pending[tail].words = count * 4;
  rdpbg_cw += count * 4;
  rsp_pending_count++;
  n64_rdpbg_tiles += count;
}

static void rdpbg_rsp_close_batch(u32 count)
{
  rdpbg_rsp_close_batch_from(rsp_gather[rsp_gather_slot], count);
  rsp_gather_slot = (rsp_gather_slot + 1) % (RDPBG_RSP_DEPTH + 1);
}
#endif /* N64_RSP_RDPBG_LIVE */
#else
#define RDPBG_RECT(X0, Y0, X1, Y1, S0, T0, DSDX, DTDY) \
  rdpq_texture_rectangle_raw(TILE0, X0, Y0, X1, Y1, S0, T0, DSDX, DTDY)
#define RDPBG_SUBMIT() do {} while (0)
#endif

#ifndef RDPBG_LPROBE
#define RDPBG_LPROBE 0
#endif


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
/* The counting sort's output: indices, not records.
 *
 * Carrying the 8-byte record through the sort instead was tried, on the
 * theory that it would remove two things at once -- the emit loop's
 * random walk back through these indices over ~10KB of rdpbg_draws
 * against an 8KB D-cache, and (on the RSP path) the 8-byte copy into a
 * staging buffer that exists only because the sorted records are not
 * contiguous.  Both of those did go away, and it was still not worth it:
 *
 *                     sort    emit     sum
 *   index sort        0.82    2.36    3.18 ms
 *   record sort       1.52    1.73    3.25 ms
 *
 * The emit side fell by 0.63 ms exactly as expected.  The sort side rose
 * by 0.70 and ate it.  The reason is the same 8KB D-cache at both ends:
 * the placement pass writes 9.7KB of records to as many cursors as there
 * are live keys, and every 16-byte line it touches costs a fetch it does
 * not need (the line is about to be fully overwritten) plus a writeback.
 * Moving work between the two passes does not help, because the cost is
 * the pass over 10KB, not which pass it is in.
 *
 * It also introduced a race worth remembering: rdpbg_sorted would be
 * rewritten by the next flush()'s sort while the RSP still had batches
 * from this one in flight -- flush() deliberately leaves its last batch
 * pending, and up to RDPBG_RSP_DEPTH can be outstanding.  The staging
 * ring is what makes that safe today.  The RDP's pixel counter caught it
 * without any visible symptom: 38,952 px/sync against the CPU path's
 * 38,933.  That counter is the check to run on anything that changes what
 * the RSP reads.
 *
 * What would actually help is not materialising a sorted list at all:
 * bucket the records as the tilemap walk produces them, into a chained
 * block per (slice,palette) key, so the sort pass and the gather both
 * disappear and the walk's existing write pass becomes the only one.
 * That is one pass over 10KB where there are now three.  -DRDPBG_LPROBE=2
 * puts the floor of the remaining emit loop at 0.94 ms of the 2.36, so
 * the reachable prize is around 1.2-1.5 ms of a 22 ms frame. */
static u16 rdpbg_order[RDPBG_MAX_DRAWS];

#ifdef RDPBG_BUCKET
/* Bucketing: sort the draws as they are produced, not afterwards.
 *
 * The pipeline makes three passes over a ~10KB draw list against an 8KB
 * D-cache.  The tilemap walk writes it; the counting sort reads it twice
 * (once to count keys, once to place indices); the emit loop reads it
 * again through those indices, in scattered order, and examines every
 * record to find the group boundaries.
 *
 * Every record's group is known the moment it is produced -- the key is
 * (vt >> 5) << 4 | palette, both of which the walk already has in
 * registers.  Appending straight to that key's bucket collapses all of
 * it: the sort disappears, the gather disappears, and so does the emit
 * loop's per-record work, because a bucket is by construction a single
 * group and the RSP can be handed the whole block with no CPU pass over
 * its contents at all.
 *
 * Buckets are chains of fixed blocks rather than per-key arrays: only a
 * dozen keys are live in a layer but any one of them could hold all 651
 * of its tiles, and sizing every key for the worst case is 1536 * 651
 * records of nothing.  A block is exactly RDPBG_RSP_MAXBATCH records, so
 * a block is a batch and chaining costs no extra RSP submissions.
 *
 * Blocks are allocated round-robin and never freed, which is deliberate.
 * The RSP DMAs a block after the CPU has moved on, and flush() leaves its
 * last batch in flight on purpose, so a block must not be rewritten until
 * well after it was handed over.  Reusing the pool in order guarantees
 * RDPBG_NBLK allocations between writes to the same block, against a
 * pipeline at most RDPBG_RSP_DEPTH deep.  (Rewriting records under a
 * pending batch is not theoretical: it is what the earlier
 * sorted-records attempt did, and the RDP's pixel counter caught it at
 * 38,952 px/sync against a reference 38,933.) */
/* Block size and pool size are a D-cache budget, not a capacity one.
 *
 * The pool is cycled once per frame, so every byte of it is dirtied and
 * written back each frame.  The list it replaced was rewritten from index
 * zero every flush, so it only ever touched ~10 KB however many flushes
 * there were.  A 48-block pool of 128-record blocks is 50 KB, and the
 * extra 40 KB of writebacks -- 2,500 lines at 40 cycles -- costs about a
 * millisecond a frame, which lands on the emulator rather than on the
 * renderer and very nearly cancelled the win.
 *
 * Halving the pool to 24 blocks halves that to 25 KB.  Shrinking the
 * blocks instead was tried and is worse: 64-record blocks cut the
 * footprint to 12.7 KB but raised blit from 13.5M to 14.3M ticks, because
 * a block is a batch and smaller blocks mean more rspq submissions.  Keep
 * the batch size, halve the pool.
 *
 * The pool only has to exceed RDPBG_RSP_DEPTH so that round-robin reuse
 * cannot overwrite a block the RSP has not read yet; 24 against 8 is
 * ample, and 24 * 128 records is 3,072 against a layer's 651. */
#define RDPBG_BLK_RECS  128
#define RDPBG_NBLK      24

/* Two records of padding per block, and they are not slack: a block is
 * RDPBG_BLK_RECS * 8 = 1024 bytes, so without padding blocks sit exactly
 * 1 KB apart and the 8 KB direct-mapped D-cache gives every eighth block
 * the same index.  A dozen buckets are live at once during the walk, each
 * with an open write cursor, so a plain array guarantees those cursors
 * evict each other -- which is what made the walk 1.1 ms more expensive
 * than the flush saved, the first time this was measured.  A 1040-byte
 * stride is coprime enough with 8192 to spread them. */
typedef struct { rdpbg_draw_t r[RDPBG_BLK_RECS]; rdpbg_draw_t pad[2]; } rdpbg_blk_t;
static rdpbg_blk_t rdpbg_blkbuf[RDPBG_NBLK] __attribute__((aligned(16)));
#define rdpbg_blk(b) (rdpbg_blkbuf[b].r)
static u16 rdpbg_blk_n[RDPBG_NBLK];
static s16 rdpbg_blk_next[RDPBG_NBLK];
static u32 rdpbg_blk_cursor = 0;

/* Live buckets for the flush being built.  rdpbg_count[] doubles as the
 * key -> slot map (slot + 1, so zero still means "unused") -- it is
 * already a 1536-entry u16 table that this file clears by touched entry,
 * which is exactly what is needed and saves another 3KB. */
static u16 rdpbg_bkey[RDPBG_MAX_KEYS];
static s16 rdpbg_bhead[RDPBG_MAX_KEYS];
static s16 rdpbg_btail[RDPBG_MAX_KEYS];
static u32 rdpbg_bnk = 0;
/* Sprites keep the old path: their order is OAM order and bucketing would
 * destroy it.  There are four of them a frame, so it costs nothing. */
static int rdpbg_bucketing = 0;

void n64_rdpbg_bucket_mode(int on) { rdpbg_bucketing = on; }

static int rdpbg_blk_alloc(void)
{
  u32 b = rdpbg_blk_cursor;
  rdpbg_blk_cursor = (rdpbg_blk_cursor + 1) % RDPBG_NBLK;
  rdpbg_blk_n[b] = 0;
  rdpbg_blk_next[b] = -1;
  return (int)b;
}
#endif

static int rdpbg_tile_load_ready = 0;

int n64_rdpbg_begin(void)
{
  rdpbg_ndraws = 0;
  rdpbg_tile_load_ready = 0;
#ifdef N64_RDP_EXEC
  rdpbg_cw = rdpbg_csent = 0;
#ifdef N64_RSP_RDPBG_LIVE
  /* The RSP DMAs its output straight into rdpbg_cmds, bypassing the CPU
   * D-cache entirely -- so any dirty line left over this buffer from a
   * previous frame (or, with the flag off, a previous CPU-authored run)
   * would eventually get evicted and clobber what the RSP just wrote.
   * One bulk writeback+invalidate per frame, before anything touches the
   * buffer this frame, rules that out. */
  data_cache_hit_writeback_invalidate(rdpbg_cmds, sizeof(rdpbg_cmds));
  rsp_pending_head = rsp_pending_count = 0;
  rsp_gather_slot = 0;
#endif
#endif
  return 1;
}

#ifdef RDPBG_BUCKET
/* Out of line: opening a bucket, or chaining a new block onto a full one.
 * Returns the block to append to, or -1 if there are no slots left. */
__attribute__((noinline))
static int rdpbg_bucket_slow(u32 key, u32 sl)
{
  int b;
  if (!sl) {
    if (rdpbg_bnk >= RDPBG_MAX_KEYS) return -1;
    b = rdpbg_blk_alloc();
    sl = ++rdpbg_bnk;
    rdpbg_count[key] = (u16)sl;
    rdpbg_bkey[sl - 1] = (u16)key;
    rdpbg_bhead[sl - 1] = (s16)b;
  } else {
    b = rdpbg_blk_alloc();
    rdpbg_blk_next[rdpbg_btail[sl - 1]] = (s16)b;
  }
  rdpbg_btail[sl - 1] = (s16)b;
  return b;
}
#endif

void n64_rdpbg_add(int x, int y, int y0, int y1, u32 vt, u32 pal, u32 flip)
{
  rdpbg_draw_t *d;
#ifdef RDPBG_BUCKET
  if (rdpbg_bucketing) {
    /* Hot path only: the bucket exists and its tail block has room.  Both
     * of the other cases -- first record for a key, block full -- are out
     * of line, because this function runs once per kept tile (1216 a
     * frame) and inlining them here grew it from 136 to 788 bytes.  That
     * is 25 cache lines for a path that needs five, and it cost more in
     * I-cache misses on the walk than the bucketing saved on the flush. */
    u32 key = ((vt >> 5) << 4) | (pal & 15);
    u32 sl = rdpbg_count[key];
    int b;
    if (__builtin_expect(sl != 0, 1)) {
      b = rdpbg_btail[sl - 1];
      if (__builtin_expect(rdpbg_blk_n[b] < RDPBG_BLK_RECS, 1))
        goto rdpbg_have_block;
    }
    b = rdpbg_bucket_slow(key, sl);
    if (b < 0) { n64_rdpbg_overflow++; return; }
  rdpbg_have_block:
    d = &rdpbg_blk(b)[rdpbg_blk_n[b]++];
    d->x = (s16)x; d->y = (s16)y;
    d->yy = (u8)(y0 | (y1 << 4));
    d->pf = (u8)(pal | (flip << 4));
    d->vt = (u16)vt;
    rdpbg_ndraws++;
    return;
  }
#endif
  if (rdpbg_ndraws >= RDPBG_MAX_DRAWS) { n64_rdpbg_overflow++; return; }
  d = &rdpbg_draws[rdpbg_ndraws++];
  d->x = (s16)x; d->y = (s16)y;
  d->yy = (u8)(y0 | (y1 << 4));
  d->pf = (u8)(pal | (flip << 4));
  d->vt = (u16)vt;
}



/* Internal helper tile used only to DMA a slice into TMEM -- see
 * rdpbg_load_slice() below.  TILE1 (tex_loader's own choice, (TILE0+1)&7,
 * for the exact same purpose) is free for this: rdpq's TLUT path uses
 * RDPQ_TILE_INTERNAL = TILE7, and the only other TILE1 user in this tree
 * is n64_rdp_bench.c's one-shot boot selftest, never concurrent with a
 * real frame. */
#define RDPBG_TILE_LOAD TILE1

/* Replacement for rdpq_tex_upload(TILE0, &sl, NULL), specialised for this
 * renderer's one fixed shape: an 8-texel-wide, 256-tall CI4 slice of
 * vram_swapped, tmem_addr 0, no wrap/mirror/shift.
 *
 * rdpq_tex_upload rebuilds a fresh tex_loader_t from scratch on every
 * call -- geometry recompute, asserts -- and because a CI4 8-texel-wide
 * row is 4 bytes (not 8-byte aligned), it always takes its LOAD_TILE path
 * rather than the cheaper LOAD_BLOCK: SET_TEXTURE_IMAGE, TWO SET_TILE
 * calls (one for its own internal helper tile, one for TILE0 with
 * palette 0), LOAD_TILE, SET_TILE_SIZE. That TILE0 SET_TILE and
 * SET_TILE_SIZE are immediately overwritten by this file's OWN key-change
 * handling right below every call site, since a slice change always
 * forces an immediate key change (cur_key resets to 0xFFFF) -- pure
 * waste. And the internal helper tile's own descriptor (format/pitch/
 * address) never changes call to call, so it does not need reissuing 21
 * times a frame either; RDPBG_TILE_LOAD is set up once a frame instead,
 * in n64_rdpbg_begin().
 *
 * So the only two RDP commands actually needed per slice change are
 * SET_TEXTURE_IMAGE (new source address) and LOAD_TILE (trigger the DMA
 * via the pre-configured helper tile) -- both already thin, single-call
 * inline wrappers in rdpq.h (see rdpq_set_texture_image_raw/
 * rdpq_load_tile), not rdpq_tex_upload's generic tex_loader_t machinery.
 * FMT_I8 is the same "lie about the format to address by byte instead of
 * by 4-bit texel" trick rdpq_tex_upload itself uses for CI4 -- a normal,
 * documented, supported way to drive LOAD_TILE, not a raw/undocumented
 * shortcut: what matters for correctness is that TILE0 (the tile actually
 * used to draw) is configured as CI4, which the key-change code already
 * does right after this runs. */
static inline void rdpbg_load_slice(u32 slice)
{
  /* Push this slice to RDRAM before the RDP reads it.
   *
   * The RDP DMAs tile data straight out of vram_swapped and does not snoop
   * the VR4300's write-back D-cache, while the emulated game writes
   * vram_swapped through it like any other store (N64_VRAM_SHADOW in
   * gba_memory.h).  A tile written this frame and still sitting dirty
   * would be loaded from whatever RDRAM held before it.
   *
   * It is the same hazard as the RSP's input records, and worse to leave
   * alone, because nothing here can see it: wrong tile *contents* draw the
   * same rectangles over the same pixels, so the canary, the primitive
   * count and the pixel count are identical either way.  It mostly
   * survives on eviction timing -- 96KB of shadow against an 8KB cache,
   * with a frame of emulation between a write and the load -- but that is
   * luck, and the record case proves the luck runs out.
   *
   * Written back a slice at a time rather than over the whole shadow
   * because that is what the RDP actually reads: 21KB a frame in 1KB
   * loads, against 96KB swept blindly.  Doing the whole shadow once a
   * frame was measured at +7% of frame time; tracking dirty kilobytes on
   * the write path was no better (+8%).  This is 64 cache operations per
   * load, on exactly the bytes about to be fetched. */
  data_cache_hit_writeback(&vram_swapped[slice * 1024], 1024);

  rdpq_set_texture_image_raw(0, PhysicalAddr(&vram_swapped[slice * 1024]),
                              FMT_I8, 4, 256);
  rdpq_load_tile(RDPBG_TILE_LOAD, 0, 0, 4, 256);
}

void n64_rdpbg_flush(int obj_palette, int sortable)
{
  extern surface_t *n64_video_acquire(void);
  u32 i, n = rdpbg_ndraws, sum = 0;
  u32 cur_slice = 0xFFFF, cur_key = 0xFFFF;

  if (!n) return;
  rdpbg_disp = n64_video_acquire();
  if (!rdpbg_disp) return;

  u32 _t0 = RDPBG_TICK();
#ifdef RDPBG_BUCKET
  if (rdpbg_bucketing && sortable) {
    /* The buckets are already the sort.  All that is left is to put the
     * groups in key order -- a dozen entries, insertion sorted -- and hand
     * each one's blocks over.  There is no pass over the records here at
     * all: a block is one group by construction, so the tile state is set
     * once per group rather than tested once per tile. */
    u32 i2, j2, nk2 = rdpbg_bnk;
    u16 ord[RDPBG_MAX_KEYS];
    for (i2 = 0; i2 < nk2; i2++) ord[i2] = (u16)i2;
    for (i2 = 1; i2 < nk2; i2++) {
      u16 v = ord[i2];
      for (j2 = i2; j2 && rdpbg_bkey[ord[j2 - 1]] > rdpbg_bkey[v]; j2--)
        ord[j2] = ord[j2 - 1];
      ord[j2] = v;
    }
    n64_rdpbg_t_sort += RDPBG_TICK() - _t0;
    _t0 = RDPBG_TICK();

    if (!rdpbg_attached) { rdpq_attach(rdpbg_disp, NULL); rdpbg_attached = 1; }
    rdpq_set_mode_standard();
    rdpq_mode_tlut(TLUT_RGBA16);
    rdpq_mode_alphacompare(1);
    if (!rdpbg_tile_load_ready) {
      rdpq_set_tile(RDPBG_TILE_LOAD, FMT_I8, 0, 8, NULL);
      rdpbg_tile_load_ready = 1;
    }
    if (rdpbg_tlut_loaded != obj_palette) {
      data_cache_hit_writeback(rdpbg_tlut[obj_palette], 512);
      rdpq_tex_upload_tlut(rdpbg_tlut[obj_palette], 0, 256);
      rdpbg_tlut_loaded = obj_palette;
      n64_rdpbg_tluts++;
    }

    for (i2 = 0; i2 < nk2; i2++) {
      u32 sl = ord[i2];
      u32 key = rdpbg_bkey[sl], slice = key >> 4;
      int b;
      if (slice != cur_slice) {
        { u32 _u = RDPBG_TICK();
          rdpbg_load_slice(slice);
          n64_rdpbg_t_upl += RDPBG_TICK() - _u; }
        cur_slice = slice; cur_key = 0xFFFF;
        n64_rdpbg_slices++;
      }
      if (key != cur_key) {
        rdpq_tileparms_t p = {0};
        p.palette = (u8)(key & 15);
        rdpq_set_tile(TILE0, FMT_CI4, 0, 8, &p);
        rdpq_set_tile_size(TILE0, 0, 0, 8, 256);
        cur_key = key;
        n64_rdpbg_groups++;
      }
      for (b = rdpbg_bhead[sl]; b >= 0; b = rdpbg_blk_next[b])
        if (rdpbg_blk_n[b])
          rdpbg_rsp_close_batch_from(rdpbg_blk(b), rdpbg_blk_n[b]);
      rdpbg_count[key] = 0;      /* leave the key map clean for next flush */
    }
    rdpbg_bnk = 0;
    n64_rdpbg_t_emit += RDPBG_TICK() - _t0;
    rdpbg_ndraws = 0;
    return;
  }
#endif
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
  if (!rdpbg_tile_load_ready) {
    /* Once a frame, not once per slice change -- see rdpbg_load_slice(). */
    rdpq_set_tile(RDPBG_TILE_LOAD, FMT_I8, 0, 8, NULL);
    rdpbg_tile_load_ready = 1;
  }
  if (rdpbg_tlut_loaded != obj_palette) {
    data_cache_hit_writeback(rdpbg_tlut[obj_palette], 512);
    rdpq_tex_upload_tlut(rdpbg_tlut[obj_palette], 0, 256);
    rdpbg_tlut_loaded = obj_palette;
    n64_rdpbg_tluts++;
  }

#ifdef N64_RSP_RDPBG_LIVE
  if (n64_rsp_rdpbg_ready()) {
    /* RSP-driven: no per-tile clip/flip/encode arithmetic on the CPU at
     * all.  Runs of same-(slice,palette) draws -- already contiguous in
     * rdpbg_order thanks to the counting sort above -- are gathered
     * (a plain struct copy, not the branchy RDPBG_RECT math) into batches
     * of up to RDPBG_RSP_MAXBATCH and handed to the RSP; see
     * rdpbg_rsp_close_batch() above for the pipelining.
     *
     * n64_rdpbg_tiles counts tiles *submitted* here (kept + degenerate
     * zero-width, see rsp_rdpbg.S), not just kept ones as the CPU path
     * below counts -- the fixed-size-output design that makes the async
     * pipeline possible means the CPU never learns which were dropped.
     * The screen-coverage canary in video.cc is unaffected and remains
     * the thing to trust for correctness. */
    u32 gcount = 0;
    for (i = 0; i < n; i++) {
      /* -DRDPBG_LPROBE isolates what is left on the CPU once the RSP has
       * the per-tile arithmetic.  Both settings draw the wrong screen on
       * purpose; the canary is expected to lie.
       *   1  drop the rdpbg_order[] indirection and read the draw list
       *      sequentially.  Note what this actually measures: not the
       *      gather's memory access, but the grouping.  Unsorted, the
       *      (slice,palette) key changes on nearly every tile, so it
       *      forces a TMEM load, a tile descriptor and a batch close per
       *      tile -- emit goes 2.21 -> 5.98 ms.  It prices the sort, not
       *      the gather.
       *   2  also stop reading the record at all, which prices the
       *      remaining loop: the key compare, the boundary tests and the
       *      8-byte staging copy. */
#if   RDPBG_LPROBE == 1
      const rdpbg_draw_t *d = &rdpbg_draws[i];
#elif RDPBG_LPROBE == 2
      static const rdpbg_draw_t dummy = { 0, 0, 0, 0, 0 };
      const rdpbg_draw_t *d = &dummy;
#else
      const rdpbg_draw_t *d = &rdpbg_draws[rdpbg_order[i]];
#endif
      u32 key = RDPBG_KEY(d);
      u32 slice = key >> 4;

      if (slice != cur_slice || key != cur_key) {
        if (gcount) { rdpbg_rsp_close_batch(gcount); gcount = 0; }
        if (slice != cur_slice) {
          { u32 _u = RDPBG_TICK();
            rdpbg_load_slice(slice);
            n64_rdpbg_t_upl += RDPBG_TICK() - _u; }
          cur_slice = slice; cur_key = 0xFFFF;
          n64_rdpbg_slices++;
        }
        if (key != cur_key) {
          rdpq_tileparms_t p = {0};
          p.palette = (u8)(key & 15);
          rdpq_set_tile(TILE0, FMT_CI4, 0, 8, &p);
          rdpq_set_tile_size(TILE0, 0, 0, 8, 256);
          cur_key = key;
          n64_rdpbg_groups++;
        }
      } else if (gcount >= RDPBG_RSP_MAXBATCH) {
        /* Group too big for one batch: close it out like any other
         * boundary, but the tile state hasn't changed so skip straight
         * back to gathering the rest of the same group. */
        rdpbg_rsp_close_batch(gcount);
        gcount = 0;
      }

      rsp_gather[rsp_gather_slot][gcount++] = *d;
    }
    if (gcount) rdpbg_rsp_close_batch(gcount);
    /* Deliberately no RDPBG_SUBMIT()/drain here: the last batch is left
     * pending, to be drained at the next boundary (this flush() call's
     * CPU-side successor -- rdpbg_emit_bg/rdpbg_emit_objs in video.cc --
     * runs while it is still in flight) or, if this was the last flush()
     * of the frame, by n64_rdpbg_end() below. */
  } else
#endif
  {
  for (i = 0; i < n; i++) {
#if   RDPBG_PROBE == 1
    /* Probe 1 (capacity): same gather shape, working set forced to 4 KB.
       Draws the wrong tiles on purpose -- the canary is expected to lie. */
    const rdpbg_draw_t *d = &rdpbg_draws[rdpbg_order[i] & 511];
#elif RDPBG_PROBE == 2
    /* Probe 2 (order): sequential over the full 20 KB, no gather. */
    const rdpbg_draw_t *d = &rdpbg_draws[i];
#else
#if RDPBG_R == 1 || RDPBG_R == 4
    u32 _rt1 = RDPBG_TICK_B();
#endif
    const rdpbg_draw_t *d = &rdpbg_draws[rdpbg_order[i]];
#endif
    u32 key = RDPBG_KEY(d), y0f = RDPBG_Y0(d), y1f = RDPBG_Y1(d);
    u32 flip = RDPBG_FLIP(d);
    u32 slice = key >> 4;
    int x0, y0, x1, y1, s0, t0, dsdx, dtdy;
#if RDPBG_R == 1
    n64_rdpbg_t_r += RDPBG_TICK_B() - _rt1;
#endif

    if (slice != cur_slice) {
      RDPBG_SUBMIT();
      { u32 _u = RDPBG_TICK();
        rdpbg_load_slice(slice);
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
#if RDPBG_R == 2
    { u32 _rt2 = RDPBG_TICK_B();
#endif
    x0 = d->x; x1 = d->x + 8;
    y0 = d->y + (int)y0f; y1 = d->y + (int)y1f;
    s0 = 0; t0 = (int)((d->vt & 31) * 8) + (int)y0f;
    dsdx = 1; dtdy = 1;

#ifndef RDPBG_NOCF
    if (flip & 1) { s0 = 7; dsdx = -1; }
    if (flip & 2) { t0 = (int)((d->vt & 31) * 8) + (7 - (int)y0f); dtdy = -1; }

    /* Not dead code, despite appearances: the caller emits x from -7 to
     * 248 (x = tx*8 - xsub, tx 0..30, xsub 0..7), so the fixups fire on the
     * first and last column of every tile row -- about 6% of tiles.  Only
     * the fixups are rare; the compares run every iteration.
     * -DRDPBG_NOCF removes both this and the flip handling to bound what
     * they cost.  It renders the screen edges wrong; it is a probe. */
    if (x0 < 0)   { if (dsdx > 0) s0 -= x0; else s0 += x0; x0 = 0; }
    if (x1 > 240) x1 = 240;
#endif
#if RDPBG_R == 2
    n64_rdpbg_t_r += RDPBG_TICK_B() - _rt2; }
#endif
#if RDPBG_R == 4
    if (x0 >= x1) n64_rdpbg_t_r += RDPBG_TICK_B() - _rt1;
#endif
    if (x0 >= x1) continue;

#if RDPBG_R == 3
    { u32 _rt3 = RDPBG_TICK_B();
#endif
    RDPBG_RECT(GBA_OFFSET_X + x0, GBA_OFFSET_Y + y0,
               GBA_OFFSET_X + x1, GBA_OFFSET_Y + y1, s0, t0, dsdx, dtdy);
#if RDPBG_R == 3
    n64_rdpbg_t_r += RDPBG_TICK_B() - _rt3; }
#endif
    n64_rdpbg_tiles++;
#if RDPBG_R == 4
    /* Range 4: the whole 1216-iteration body, one wrapper.  Ranges 1-3
     * wrapped three corners of it; this asks whether the ~95 COUNT/tile
     * the emit subtraction implies is actually inside this loop at all. */
    n64_rdpbg_t_r += RDPBG_TICK_B() - _rt1;
#endif
  }

  RDPBG_SUBMIT();
  }
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
#if defined(N64_RDP_EXEC) && defined(N64_RSP_RDPBG_LIVE)
  /* Up to RDPBG_RSP_DEPTH RSP batches can be left outstanding by flush()
   * at the end of the frame's last call.  Drain them all here, once,
   * rather than forcing every flush() call to drain its own tail and
   * losing the overlap with the next layer's tilemap walk in between
   * calls. */
  while (rsp_pending_count) rdpbg_rsp_drain_one();
#endif
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
