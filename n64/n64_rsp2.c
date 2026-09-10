/* n64_rsp2.c -- CPU side of the RDP-command-generation RSP offload.
 *
 * See rsp_rdpbg.S for what the ucode does and why.  This file registers
 * it as an rspq overlay (coexisting with rdpq, unlike the old raw
 * rsp_init/rsp_load path n64_rsp.c uses) and verifies its output against
 * the exact computation n64_rdp_bg.c's RDPBG_RECT macro performs, byte
 * for byte, before anything live depends on it.
 *
 * Status: verification only.  Nothing in the real rendering path calls
 * into this yet -- n64_rsp_rdpbg_selftest() is called once at boot,
 * behind -DN64_RSP_RDPBG_TEST, and reports PASS/FAIL plus how long the
 * RSP took, which is the number needed before deciding whether wiring
 * this into the live per-frame path (with the double-buffering an async
 * version would need) is worth doing at all.
 */
#include <libdragon.h>
#include <rsp.h>
#include <rspq.h>
#include <string.h>
#include "../common.h"

DEFINE_RSP_UCODE(rsp_rdpbg);

static uint32_t rdpbg_ovl_id;
static bool rdpbg_rsp_ready = false;

void n64_rsp_rdpbg_init(void)
{
  rspq_init();
  rdpbg_ovl_id = rspq_overlay_register(&rsp_rdpbg);
  rdpbg_rsp_ready = true;
}

bool n64_rsp_rdpbg_ready(void)
{
  return rdpbg_rsp_ready;
}

/* Live async path: queue one batch, non-blocking, and hand back a
 * syncpoint the caller can wait on whenever it actually needs the
 * result -- which, done well, is considerably later than "right away".
 * See n64/n64_rdp_bg.c's N64_RSP_RDPBG_LIVE path for the pipelining this
 * exists to support.
 *
 * The per-batch byte count (META in the ucode) is not needed here: every
 * tile now produces exactly 16 bytes of output, kept or dropped (see
 * rsp_rdpbg.S's RDPBG_keep -- a dropped tile becomes a zero-width rect
 * instead of being omitted), specifically so the CALLER can compute
 * every batch's destination offset in rdpbg_cmds immediately, without
 * waiting on any RSP result first.  All batches share one throwaway
 * destination for it: the RSP processes queued commands strictly in
 * order, one at a time, so there is never more than one writer to it at
 * once despite many batches being in flight from the CPU's point of
 * view. */
static uint32_t rdpbg_count_sink[4] __attribute__((aligned(16)));

rspq_syncpoint_t n64_rsp_rdpbg_queue(const void *src, uint32_t count, void *dst)
{
  rspq_write(rdpbg_ovl_id, 0,
             PhysicalAddr(src), count,
             PhysicalAddr(dst), PhysicalAddr((void*)rdpbg_count_sink));
  return rspq_syncpoint_new();
}

/* Mirrors rdpbg_draw_t in n64/n64_rdp_bg.c exactly -- the ucode's DMA-in
 * assumes this layout (s16 x, s16 y, u8 yy, u8 pf, u16 vt, 8 bytes). */
typedef struct {
  int16_t  x, y;
  uint8_t  yy;
  uint8_t  pf;
  uint16_t vt;
} test_draw_t;

/* Reference implementation: the exact computation RDPBG_RECT performs,
 * transcribed rather than shared, so a bug common to both sides could
 * still slip through -- but it is checked separately below against
 * hand-computed expected words for a few cases, which a shared-code bug
 * could not fake.
 *
 * A dropped tile (x0>=x1 after clip) now produces a degenerate
 * zero-width rect rather than no output at all -- see the comment on
 * RDPBG_keep in rsp_rdpbg.S for why: the live path needs every batch's
 * output size to be exactly count*16 bytes, known before the RSP has
 * necessarily run, so it can place the NEXT batch immediately after
 * without waiting on this one's result first.  *kept still reports
 * whether the ORIGINAL CPU path would have emitted anything, purely so
 * the selftest can log it; it is no longer used to decide how much
 * output to compare. */
static void cpu_reference(const test_draw_t *d, uint32_t out[4], int *kept)
{
  int y0f = d->yy & 15, y1f = d->yy >> 4;
  int flip = d->pf >> 4;
  int x0 = d->x, x1 = d->x + 8;
  int y0 = d->y + y0f, y1 = d->y + y1f;
  int s0 = 0, t0 = ((d->vt & 31) * 8) + y0f;
  int dsdx = 1, dtdy = 1;

  if (flip & 1) { s0 = 7; dsdx = -1; }
  if (flip & 2) { t0 = ((d->vt & 31) * 8) + (7 - y0f); dtdy = -1; }

  if (x0 < 0) { if (dsdx > 0) s0 -= x0; else s0 += x0; x0 = 0; }
  if (x1 > 240) x1 = 240;

  *kept = (x0 < x1);
  if (!*kept) x1 = x0;    /* degenerate: zero width, same as RDPBG_keep now does */

  { int X0 = 40 + x0, Y0 = 40 + y0, X1 = 40 + x1, Y1 = 40 + y1;
    out[0] = 0xE4000000u | ((uint32_t)(X1 * 4) << 12) | (uint32_t)(Y1 * 4);
    out[1] = ((uint32_t)(X0 * 4) << 12) | (uint32_t)(Y0 * 4);
    out[2] = ((uint32_t)(s0 * 32) << 16) | (uint32_t)((t0 * 32) & 0xFFFF);
    out[3] = ((uint32_t)(((dsdx * 1024)) & 0xFFFF) << 16)
           | (uint32_t)(((dtdy * 1024)) & 0xFFFF);
  }
}

void n64_rsp_rdpbg_selftest(void)
{
  /* Edge cases: unflipped/h-flip/v-flip/both, on-screen, left-clip,
   * right-clip, fully-clipped-away (dropped), and left-clip combined
   * with h-flip (the only case where the clip math takes the "s0 += x0"
   * branch instead of "s0 -= x0"). */
  static test_draw_t in[16] __attribute__((aligned(16)));
  int n = 0;
  #define ADD(X,Y,Y0,Y1,PAL,FLIP,VT) \
    in[n].x=(X); in[n].y=(Y); in[n].yy=((Y0)|((Y1)<<4)); \
    in[n].pf=((PAL)|((FLIP)<<4)); in[n].vt=(VT); n++

  ADD(  40,  40, 0, 8, 3, 0, 0x0041);   /* plain, on-screen */
  ADD(  -4,  40, 0, 8, 1, 0, 0x0002);   /* left-clip, unflipped (s0 -= x0) */
  ADD( 236,  40, 0, 8, 2, 0, 0x0003);   /* right-clip (x1 > 240) */
  ADD( -20,  40, 0, 8, 0, 0, 0x0004);   /* fully off-screen: dropped */
  ADD(  80,  16, 2, 6, 5, 1, 0x0060);   /* h-flip */
  ADD(  80,  16, 1, 7, 6, 2, 0x0061);   /* v-flip */
  ADD(  80,  16, 0, 8, 7, 3, 0x0062);   /* h+v flip */
  ADD(  -3,  16, 0, 8, 9, 1, 0x0063);   /* left-clip + h-flip (s0 += x0) */
  ADD( 239,  40, 0, 8,15, 0, 0x07FF);   /* right edge, single column kept */
  #undef ADD

  static uint32_t rsp_out[16 * 4] __attribute__((aligned(16)));
  /* data_cache_hit_invalidate (and its writeback_invalidate sibling)
     assert both address AND length are 16-byte multiples -- a lone
     uint32_t fails the length check even when it is itself 16-byte
     aligned.  Give the count its own full cache line so the operation
     never touches a neighbouring global. */
  static uint32_t rsp_count_buf[4] __attribute__((aligned(16)));
  #define rsp_count (rsp_count_buf[0])
  rsp_count = 0xFFFFFFFFu;   /* poison, so "untouched" is detectable */

  data_cache_hit_writeback_invalidate(in, sizeof(in));
  data_cache_hit_writeback_invalidate(rsp_out, sizeof(rsp_out));
  data_cache_hit_writeback_invalidate((void*)rsp_count_buf, sizeof(rsp_count_buf));

  if (!rdpbg_rsp_ready) {
    debugf("[gpSP]: rdpbg-rsp: not initialised, skipping selftest\n");
    return;
  }

  /* Time the round trip with the same CP0 COUNT technique used
     throughout this project's profiling (COUNT ticks at CPU/2, so x2
     gives PClock).  This does NOT measure overlap/hiding -- the CPU is
     blocked on rspq_syncpoint_wait the whole time, so it is an upper
     bound on RSP throughput for this batch size, not a frame-time
     prediction. */
  u32 _t0, _t1;
  __asm__ __volatile__("mfc0 %0, $9" : "=r"(_t0));
  rspq_write(rdpbg_ovl_id, 0,
             PhysicalAddr(in), (uint32_t)n,
             PhysicalAddr(rsp_out), PhysicalAddr((void*)&rsp_count));
  rspq_syncpoint_t sp = rspq_syncpoint_new();
  rspq_flush();
  rspq_syncpoint_wait(sp);
  __asm__ __volatile__("mfc0 %0, $9" : "=r"(_t1));
  { u32 ticks = _t1 - _t0;
    debugf("[gpSP]: rdpbg-rsp: %lu tiles round-trip in %lu COUNT ticks "
           "(%lu PClock, includes rspq dispatch + 3 DMAs + sync overhead)\n",
           (unsigned long)n, (unsigned long)ticks, (unsigned long)(ticks * 2)); }

  data_cache_hit_invalidate(rsp_out, sizeof(rsp_out));
  data_cache_hit_invalidate((void*)rsp_count_buf, sizeof(rsp_count_buf));

  {
    /* Output is now fixed-size: every tile, kept or degenerate, produces
       exactly 4 words, at index i for input tile i.  Compare all n. */
    int i, fail = 0, kept_expected = 0;
    for (i = 0; i < n; i++) {
      uint32_t ref[4]; int kept;
      cpu_reference(&in[i], ref, &kept);
      if (kept) kept_expected++;
      if (memcmp(&rsp_out[i * 4], ref, sizeof(ref)) != 0) {
        fail++;
        debugf("[gpSP]: rdpbg-rsp MISMATCH tile %d (kept=%d): "
               "cpu %08lx %08lx %08lx %08lx  rsp %08lx %08lx %08lx %08lx\n",
               i, kept, (unsigned long)ref[0], (unsigned long)ref[1],
               (unsigned long)ref[2], (unsigned long)ref[3],
               (unsigned long)rsp_out[i*4], (unsigned long)rsp_out[i*4+1],
               (unsigned long)rsp_out[i*4+2], (unsigned long)rsp_out[i*4+3]);
      }
    }
    if (rsp_count != (uint32_t)(n * 16)) {
      fail++;
      debugf("[gpSP]: rdpbg-rsp MISMATCH: byte count %lu, expected %d (fixed-size output)\n",
             (unsigned long)rsp_count, n * 16);
    }
    debugf("[gpSP]: rdpbg-rsp selftest: %d tiles, %d would-be-kept, %s (%d mismatches)\n",
           n, kept_expected, fail ? "FAIL" : "PASS", fail);
  }

  /* Throughput at realistic batch size.  1216 tiles/frame over ~27 groups
     averages ~45 tiles/group; use a round 64 so the timing reflects a
     representative batch rather than the 9-tile correctness set, whose
     fixed per-call overhead (DMA setup, rspq dispatch) would dominate and
     say nothing about steady-state throughput. */
  {
    static test_draw_t big[64] __attribute__((aligned(16)));
    static uint32_t big_out[64 * 4] __attribute__((aligned(16)));
    int i;
    for (i = 0; i < 64; i++) {
      big[i].x = (int16_t)(40 + (i % 25) * 8);
      big[i].y = 40;
      big[i].yy = 0 | (8 << 4);
      big[i].pf = (i & 15) | ((i & 3) << 4);
      big[i].vt = (uint16_t)(i & 0x3FF);
    }
    data_cache_hit_writeback_invalidate(big, sizeof(big));
    data_cache_hit_writeback_invalidate(big_out, sizeof(big_out));
    rsp_count = 0xFFFFFFFFu;
    data_cache_hit_writeback_invalidate((void*)rsp_count_buf, sizeof(rsp_count_buf));

    u32 _bt0, _bt1;
    __asm__ __volatile__("mfc0 %0, $9" : "=r"(_bt0));
    rspq_write(rdpbg_ovl_id, 0,
               PhysicalAddr(big), (uint32_t)64,
               PhysicalAddr(big_out), PhysicalAddr((void*)&rsp_count));
    rspq_syncpoint_t sp2 = rspq_syncpoint_new();
    rspq_flush();
    rspq_syncpoint_wait(sp2);
    __asm__ __volatile__("mfc0 %0, $9" : "=r"(_bt1));
    data_cache_hit_invalidate((void*)rsp_count_buf, sizeof(rsp_count_buf));

    { u32 ticks = _bt1 - _bt0;
      debugf("[gpSP]: rdpbg-rsp: 64-tile batch: %lu COUNT ticks (%lu PClock), "
             "count=%lu, %lu PClock/tile\n",
             (unsigned long)ticks, (unsigned long)(ticks * 2),
             (unsigned long)rsp_count,
             (unsigned long)(ticks * 2 / 64)); }
  }
}
