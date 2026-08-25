/* RSP offload for GBA scanline work -- stage 1: infrastructure self-test.
 *
 * Rendering is 27.4 ms of a 55 ms frame and is 85% compute (only ~4.15 ms
 * of per-frame D-cache stall is attributable to it), so the RSP is wanted
 * as a second execution unit running alongside the VR4300.  Before writing
 * a renderer, prove the path: assemble, load, DMA in, compute, DMA out,
 * sync -- and verify the result on the CPU.
 */
#include <libdragon.h>
#include <rsp.h>
#include <string.h>
#include "../common.h"

DEFINE_RSP_UCODE(rsp_gbascan);

/* Mirrors the CTRL block at DMEM offset 0 in rsp_gbascan.S */
/* Padded to 32 bytes: libdragon's cache ops assert that both address and
   length are 16-byte multiples, and this struct is handed to them before
   every DMA.  At 5 words (20 bytes) that assertion fires. */
typedef struct {
  u32 src;
  u32 dst;
  u32 len;
  u32 orval;
  u32 op;        /* 0 = selftest, 1 = expand */
  u32 rep;       /* repeat kernel N times inside one invocation */
  u32 palv;      /* RDRAM: per-layer per-group palette vectors */
  u32 pad[1];
} rsp_ctrl_t;

static bool rsp_ready = false;



void n64_rsp_init(void)
{
  rsp_init();
  rsp_load(&rsp_gbascan);
  rsp_ready = true;
}

/* Offload the XBGR1555 -> RGBA5551 blit to the RSP.
 *
 * The CPU version costs ~11.7 cyc/px but only ~2-3 of that is the
 * conversion; the rest is uncached store throughput, because the
 * framebuffer is KSEG1 and every write goes to RDRAM through the write
 * buffer with no combining.  The RSP moves it with its own DMA engine and
 * converts with vector ops, so the CPU is freed entirely.
 *
 * Both buffers are already uncached (KSEG1), so no cache maintenance is
 * needed -- the CPU's writes to the screen buffer are in RDRAM by
 * construction.  RSP DMA takes physical addresses, hence the masking.
 */
void n64_rsp_blit(const void *src, void *dst, u32 bytes_per_line,
                  u32 lines, u32 pitch)
{
  static rsp_ctrl_t bc __attribute__((aligned(16)));
  if (!rsp_ready) return;

  bc.src   = ((u32)src) & 0x1FFFFFFF;
  bc.dst   = ((u32)dst) & 0x1FFFFFFF;
  bc.len   = bytes_per_line;
  bc.orval = lines;
  bc.op    = 5;
  bc.rep   = pitch;

  data_cache_hit_writeback_invalidate(&bc, sizeof(bc));
  rsp_load_data(&bc, sizeof(bc), 0);
  rsp_run();
}

/* Returns true if the RSP produced exactly what the CPU would have. */
bool n64_rsp_selftest(void)
{
  static u16 src[256] __attribute__((aligned(16)));
  static u16 dst[256] __attribute__((aligned(16)));
  /* Static and 16-byte aligned, not stack locals: these are handed to DMA,
     so they need cache-line-aligned writeback.  The first attempt used a
     stack local for ctrl and rsp_load_data transferred stale zeros --
     len came through as 0 and the ucode hung on a size -1 DMA. */
  static rsp_ctrl_t ctrl __attribute__((aligned(16)));
  static rsp_ctrl_t back __attribute__((aligned(16)));
  unsigned i;

  if (!rsp_ready) return false;

  for (i = 0; i < 256; i++) { src[i] = (u16)(i * 7 + 1); dst[i] = 0xDEAD; }

  /* The RSP DMAs from RDRAM, so the CPU's writes must be in RDRAM, not
     sitting dirty in the D-cache -- and its result must not be shadowed by
     stale cached lines on the way back. */
  data_cache_hit_writeback_invalidate(src, sizeof(src));
  data_cache_hit_writeback_invalidate(dst, sizeof(dst));

  ctrl.src   = (u32)src;
  ctrl.dst   = (u32)dst;
  ctrl.len   = sizeof(src);
  ctrl.orval = 0x0001;              /* same shape as the RGBA5551 alpha bit */
  ctrl.op    = 0;                   /* selftest */

  /* Verify where the control block actually lands before trusting it.
     The first attempt assumed .data starts at DMEM offset 0 and the ucode
     hung with t0 = 0xFFFFFFFF -- i.e. it read CTRL_LEN as 0 and issued a
     DMA with size -1.  Read DMEM back and check. */
  data_cache_hit_writeback_invalidate(&ctrl, sizeof(ctrl));
  rsp_load_data(&ctrl, sizeof(ctrl), 0);
  {
    memset(&back, 0, sizeof(back));
    data_cache_hit_writeback_invalidate(&back, sizeof(back));
    rsp_read_data(&back, sizeof(back), 0);
    data_cache_hit_invalidate(&back, sizeof(back));
    debugf("[gpSP]: RSP ctrl wrote src=%08lx dst=%08lx len=%lu or=%lu\n",
           (unsigned long)ctrl.src, (unsigned long)ctrl.dst,
           (unsigned long)ctrl.len, (unsigned long)ctrl.orval);
    debugf("[gpSP]: RSP dmem[0] read src=%08lx dst=%08lx len=%lu or=%lu\n",
           (unsigned long)back.src, (unsigned long)back.dst,
           (unsigned long)back.len, (unsigned long)back.orval);
    if (back.len != ctrl.len) {
      debugf("[gpSP]: RSP ctrl block is NOT at DMEM 0 -- aborting selftest\n");
      return false;
    }
  }
  rsp_run();                        /* blocks until the ucode breaks */

  data_cache_hit_invalidate(dst, sizeof(dst));

  for (i = 0; i < 256; i++) {
    u16 want = (u16)((i * 7 + 1) | 0x0001);
    if (dst[i] != want) {
      debugf("[gpSP]: RSP selftest FAILED at %u: got %04x want %04x\n",
             i, dst[i], want);
      return false;
    }
  }
  debugf("[gpSP]: RSP selftest OK (ucode loaded, DMA in/out, %u bytes)\n",
         (unsigned)sizeof(src));
  return true;
}


/* ---------------------------------------------------------------------
 * Stage 2: is the RSP actually faster at the work we would offload?
 *
 * The offloadable work is 4bpp index extraction -- render_tile_Nbpp's
 * INDXCOLOR path (`pval | px_comb`), which runs once per *layer*-pixel
 * across 4 layers.  Palette lookup is deliberately NOT offloaded: it is a
 * per-lane gather, which the RSP cannot vectorise, and it runs only once
 * per *output* pixel at merge time.
 *
 * Both sides produce the identical planar layout so the comparison is
 * like-for-like.  Planar rather than pixel-interleaved because
 * interleaving needs per-lane ssv stores that would dominate and measure
 * the shuffle rather than the extraction -- a real renderer still has to
 * pay that, so this number is an upper bound on the achievable win.
 * ------------------------------------------------------------------- */

/* 256 source bytes = 512 pixels.  Bounded by DMEM, not by taste: output is
   4 bytes per input byte (2 pixels, u16 each), so 256 in + 1024 out fits
   the 4 KB alongside code and constants.  1024 in would need 4 KB of
   output alone. */
#define EXP_BYTES 256

static u8  exp_src[EXP_BYTES]        __attribute__((aligned(16)));
static u16 exp_rsp[EXP_BYTES * 2]    __attribute__((aligned(16)));
static u16 exp_cpu[EXP_BYTES * 2]    __attribute__((aligned(16)));

#define RSP_TICK() ({ u32 _t; __asm__ volatile("mfc0 %0, $9" : "=r"(_t)); _t; })

/* Scalar reference, same planar layout the ucode writes. */
static void expand_cpu(const u8 *src, u16 *dst, u32 len, u16 flag)
{
  u32 n = len / 2;                        /* entries per plane */
  u16 *p0 = dst, *p1 = dst + n, *p2 = dst + 2*n, *p3 = dst + 3*n;
  u32 i;
  for (i = 0; i < n; i++) {
    u8 ev = src[i*2 + 0], od = src[i*2 + 1];
    p0[i] = (u16)(( ev       & 0xF) | flag);
    p1[i] = (u16)(((ev >> 4) & 0xF) | flag);
    p2[i] = (u16)(( od       & 0xF) | flag);
    p3[i] = (u16)(((od >> 4) & 0xF) | flag);
  }
}

void n64_rsp_bench(void)
{
  rsp_ctrl_t c;
  u32 i, t0, t1, rsp_ticks, cpu_ticks;
  const u16 flag = 0x0100;
  const u32 ITERS = 32;

  if (!rsp_ready) return;

  for (i = 0; i < EXP_BYTES; i++) exp_src[i] = (u8)(i * 37 + (i >> 3));
  data_cache_hit_writeback_invalidate(exp_src, sizeof(exp_src));
  data_cache_hit_writeback_invalidate(exp_rsp, sizeof(exp_rsp));

  c.src = (u32)exp_src; c.dst = (u32)exp_rsp;
  c.len = EXP_BYTES;    c.orval = flag;  c.op = 1;  c.rep = 1;
  data_cache_hit_writeback_invalidate(&c, sizeof(c));

  /* Two-point slope measurement.  A single timing conflates the kernel with
     the per-invocation cost (rsp_load_data + RSP start + rsp_wait polling),
     which on a 256-byte payload dominates completely -- the first attempt
     read 996% of CPU almost entirely as call overhead.  Time rep=1 and
     rep=1+R, subtract, and the fixed cost cancels leaving R kernels. */
  {
    u32 lo, hi;
    const u32 R = 64;

    c.rep = 1;
    data_cache_hit_writeback_invalidate(&c, sizeof(c));
    rsp_load_data(&c, sizeof(c), 0);
    rsp_run();                                    /* warm */

    t0 = RSP_TICK();
    for (i = 0; i < ITERS; i++) { rsp_load_data(&c, sizeof(c), 0); rsp_run(); }
    t1 = RSP_TICK();
    lo = t1 - t0;                                 /* ITERS x (overhead + 1 kernel) */

    c.rep = 1 + R;
    data_cache_hit_writeback_invalidate(&c, sizeof(c));
    t0 = RSP_TICK();
    for (i = 0; i < ITERS; i++) { rsp_load_data(&c, sizeof(c), 0); rsp_run(); }
    t1 = RSP_TICK();
    hi = t1 - t0;                                 /* ITERS x (overhead + 1+R kernels) */

    rsp_ticks = (hi - lo);                        /* ITERS x R kernels, no overhead */
    debugf("[gpSP]: RSP call overhead ~%lu cyc/invocation\n",
           (unsigned long)((u64)(lo * 2) / ITERS));

    /* The call is loop-invariant -- same arguments every iteration -- so GCC
       hoists it and runs it about once, which read as 0.0057 cycles/pixel.
       A memory barrier per iteration forces all ITERS*R to actually run. */
    t0 = RSP_TICK();
    for (i = 0; i < ITERS * R; i++) {
      expand_cpu(exp_src, exp_cpu, EXP_BYTES, flag);
      __asm__ volatile("" ::: "memory");
    }
    t1 = RSP_TICK();
    cpu_ticks = t1 - t0;                          /* ITERS x R kernels */
  }
  data_cache_hit_invalidate(exp_rsp, sizeof(exp_rsp));

  for (i = 0; i < EXP_BYTES * 2; i++) {
    if (exp_rsp[i] != exp_cpu[i]) {
      debugf("[gpSP]: RSP expand MISMATCH at %lu: rsp=%04x cpu=%04x\n",
             (unsigned long)i, exp_rsp[i], exp_cpu[i]);
      return;
    }
  }
  { u64 px = (u64)EXP_BYTES * 2 * ITERS * 64;
    debugf("[gpSP]: RSP expand OK | %lu px total | rsp %lu.%02lu cyc/px | cpu %lu.%02lu cyc/px\n",
           (unsigned long)px,
           (unsigned long)((u64)rsp_ticks * 2 / px),
           (unsigned long)((u64)rsp_ticks * 200 / px % 100),
           (unsigned long)((u64)cpu_ticks * 2 / px),
           (unsigned long)((u64)cpu_ticks * 200 / px % 100)); }
  debugf("[gpSP]: RSP expand OK | %lu px/iter x%lu | rsp=%luK cpu=%luK ticks"
         " | rsp is %lu%% of cpu\n",
         (unsigned long)(EXP_BYTES * 2), (unsigned long)ITERS,
         (unsigned long)(rsp_ticks / 1000), (unsigned long)(cpu_ticks / 1000),
         (unsigned long)(cpu_ticks ? (u64)rsp_ticks * 100 / cpu_ticks : 0));

  /* Price the palette step on its own.  op=2 is extraction plus a scalar
     16-entry lookup.  Same slope method, so the ~2166-cycle call overhead
     cancels and what is left is per-pixel cost.  This decides where palette
     lookup lives: the RSP has no per-lane gather, so 16 entries cost ~4
     ops/px whether scalar or as a veq+vmrg tree, against roughly one cached
     load on the CPU. */
  {
    const u32 R = 64;
    u32 a, b, lo, hi;
    c.op = 2; c.rep = 1;
    data_cache_hit_writeback_invalidate(&c, sizeof(c));
    rsp_load_data(&c, sizeof(c), 0);
    a = RSP_TICK(); rsp_run(); lo = RSP_TICK() - a;

    c.rep = 1 + R;
    data_cache_hit_writeback_invalidate(&c, sizeof(c));
    rsp_load_data(&c, sizeof(c), 0);
    b = RSP_TICK(); rsp_run(); hi = RSP_TICK() - b;

    if (hi > lo) {
      u64 px = (u64)EXP_BYTES * 2u * R;
      u32 x100 = (u32)(((u64)(hi - lo) * 2u * 100u) / px);
      debugf("[gpSP]: RSP extract+palette %lu.%02lu cyc/px"
             " (extract alone 1.76) -> palette adds %lu.%02lu\n",
             (unsigned long)(x100 / 100), (unsigned long)(x100 % 100),
             (unsigned long)((x100 - 176) / 100), (unsigned long)((x100 - 176) % 100));
    }
  }


  /* op=3: 4-layer extract + composite, the last unmeasured piece of the
     render pipeline.  Same slope method so the ~2166-cycle call overhead
     cancels.  Cost is quoted per *output* pixel, which is what the frame
     projection needs -- each output pixel costs four layer-pixels of work. */
  {
    const int R = 64;
    const int LAYER_BYTES = 128;          /* 4 layers x 128 B fits DMEM */
    static u8  csrc[4 * 128] __attribute__((aligned(16)));
    static u16 cdst[4 * 128] __attribute__((aligned(16)));
    u32 t0c, t1c, lo, hi, i;

    for (i = 0; i < sizeof(csrc); i++) csrc[i] = (u8)(i * 31 + (i >> 3));
    data_cache_hit_writeback_invalidate(csrc, sizeof(csrc));
    data_cache_hit_writeback_invalidate(cdst, sizeof(cdst));

    c.src = (u32)csrc; c.dst = (u32)cdst;
    c.len = LAYER_BYTES; c.orval = 0; c.op = 3;

    c.rep = 1;
    data_cache_hit_writeback_invalidate(&c, sizeof(c));
    rsp_load_data(&c, sizeof(c), 0);
    t0c = RSP_TICK(); rsp_run(); lo = RSP_TICK() - t0c;

    c.rep = 1 + R;
    data_cache_hit_writeback_invalidate(&c, sizeof(c));
    rsp_load_data(&c, sizeof(c), 0);
    t1c = RSP_TICK(); rsp_run(); hi = RSP_TICK() - t1c;

    if (hi > lo) {
      /* LAYER_BYTES bytes/layer x 2 px/byte = output pixels per iteration */
      u32 outpx = (u32)LAYER_BYTES * 2u * (u32)R;
      u32 cyc_x100 = (u32)(((u64)(hi - lo) * 2u * 100u) / outpx);
      debugf("[gpSP]: RSP 4-layer composite %lu.%02lu cyc per OUTPUT px\n",
             (unsigned long)(cyc_x100 / 100), (unsigned long)(cyc_x100 % 100));
      debugf("[gpSP]:   (CPU equivalent: 4 layers x ~5.81 = ~23.2 cyc/px)\n");
    }
  }

  /* What does feeding the RSP actually cost?
   *
   * The composite kernel wants packed 4bpp bytes laid out contiguously per
   * layer, but real tile rows are scattered through VRAM by tile index, so
   * the data has to be gathered first.  If the CPU does that gather, it
   * pays exactly the scattered-read cost the offload was meant to avoid.
   * Measure it against a realistic access pattern: 32 tiles per layer per
   * scanline, 4 layers, tile indices spread over a 16 KB charblock. */
  {
    extern u8 vram_raw[];
    static u8 gathered[4 * 32 * 4] __attribute__((aligned(16)));
    static u16 tilemap[4][32];
    u32 t0g, t1g, L, i, s;
    u32 lines = 160;

    /* Plausible tilemap: indices scattered, not sequential. */
    for (L = 0; L < 4; L++)
      for (i = 0; i < 32; i++)
        tilemap[L][i] = (u16)((i * 37 + L * 101) & 0x3FF);

    t0g = RSP_TICK();
    for (s = 0; s < lines; s++) {
      u8 *g = gathered;
      for (L = 0; L < 4; L++) {
        const u8 *base = &vram_raw[L * 0x4000 + (s & 7) * 4];
        for (i = 0; i < 32; i++) {
          const u32 *tp = (const u32 *)&base[(tilemap[L][i] & 0x3FF) * 32];
          *(u32 *)g = *tp;         /* one tile row = 4 bytes */
          g += 4;
        }
      }
    }
    t1g = RSP_TICK();
    {
      u32 cyc = (t1g - t0g) * 2;             /* COUNT is CPU/2 */
      debugf("[gpSP]: CPU gather for %lu scanlines: %lu cyc = %lu.%02lu ms/frame\n",
             (unsigned long)lines, (unsigned long)cyc,
             (unsigned long)(cyc / 93750), (unsigned long)((cyc % 93750) * 100 / 93750));
      debugf("[gpSP]:   (composite offload saves 6.78 ms before this cost)\n");
    }

    /* Same bytes, 8x fewer scattered accesses.
     *
     * The per-scanline gather above re-reads 4 bytes from each tile for
     * every one of the 8 scanlines in a tile-row band -- 256 scattered
     * reads per layer per band.  Fetching the 32 whole tiles once per band
     * moves exactly the same 1 KB with 32 scattered reads, each pulling
     * contiguous cache lines.  Cache behaviour, not bytes moved, is what
     * made the first version cost 5.13 ms. */
    t0g = RSP_TICK();
    for (s = 0; s < lines / 8; s++) {
      u8 *g = gathered;
      for (L = 0; L < 4; L++) {
        const u8 *base = &vram_raw[L * 0x4000];
        for (i = 0; i < 32; i++) {
          const u32 *tp = (const u32 *)&base[(tilemap[L][i] & 0x3FF) * 32];
          u32 w;
          /* whole 32-byte tile = 8 words, sequential */
          for (w = 0; w < 8; w++) ((u32 *)g)[w] = tp[w];
          g += 32;
          if (g > gathered + sizeof(gathered) - 32) g = gathered;
        }
      }
    }
    t1g = RSP_TICK();
    {
      u32 cyc = (t1g - t0g) * 2;
      debugf("[gpSP]: CPU band gather (1 fetch per 8 lines): %lu cyc = %lu.%02lu ms/frame\n",
             (unsigned long)cyc,
             (unsigned long)(cyc / 93750), (unsigned long)((cyc % 93750) * 100 / 93750));
    }
  }

  /* op=4: composite with per-tile palette, verified against a CPU model of
     gpSP's INDXCOLOR semantics -- `pval | tilepal`, transparency tested on
     the raw nibble so index 0 of any sub-palette still reads transparent. */
  {
    const int LB = 128;                       /* bytes per layer */
    static u8  src4[4 * 128]  __attribute__((aligned(16)));
    static u16 pal4[4 * 128]  __attribute__((aligned(16)));  /* LEN/4 groups x 8 lanes */
    static u16 dst4[4 * 128]  __attribute__((aligned(16)));
    static u16 ref4[4 * 128];
    u32 i, L, g, lane, bad = 0;

    for (i = 0; i < sizeof(src4); i++) src4[i] = (u8)(i * 53 + (i >> 4));
    /* one palette vector per layer per 4-tile group: [p0,p0,p1,p1,...] */
    for (L = 0; L < 4; L++)
      for (g = 0; g < LB / 16; g++)
        for (lane = 0; lane < 8; lane++)
          pal4[(L * (LB / 16) + g) * 8 + lane] =
            (u16)(((L * 3 + g * 5 + (lane / 2)) & 0xF) << 4);

    data_cache_hit_writeback_invalidate(src4, sizeof(src4));
    data_cache_hit_writeback_invalidate(pal4, sizeof(pal4));
    data_cache_hit_writeback_invalidate(dst4, sizeof(dst4));

    c.src = (u32)src4; c.dst = (u32)dst4; c.palv = (u32)pal4;
    c.len = LB; c.orval = 0; c.op = 4; c.rep = 1;
    data_cache_hit_writeback_invalidate(&c, sizeof(c));
    rsp_load_data(&c, sizeof(c), 0);
    rsp_run();
    data_cache_hit_invalidate(dst4, sizeof(dst4));

    /* CPU reference, same planar layout the kernel writes */
    for (g = 0; g < (u32)LB / 16; g++) {
      for (lane = 0; lane < 8; lane++) {
        u32 plane_ent[4];
        u32 pl;
        for (pl = 0; pl < 4; pl++) {
          u32 acc = 0, have = 0;
          for (L = 0; L < 4; L++) {
            const u8 *lay = &src4[L * LB + g * 16];
            u32 byte = (pl < 2) ? lay[lane * 2] : lay[lane * 2 + 1];
            u32 nib  = (pl & 1) ? ((byte >> 4) & 0xF) : (byte & 0xF);
            u32 pv   = pal4[(L * (LB / 16) + g) * 8 + lane];
            if (L == 0)      { acc = nib | pv; have = 1; }
            else if (nib)    { acc = nib | pv; }
          }
          (void)have;
          plane_ent[pl] = acc;
        }
        for (pl = 0; pl < 4; pl++)
          ref4[pl * (LB / 2) + g * 8 + lane] = (u16)plane_ent[pl];
      }
    }
    for (i = 0; i < (u32)LB * 2; i++) {
      if (dst4[i] != ref4[i]) {
        if (bad++ < 3)
          debugf("[gpSP]: comppal MISMATCH at %lu: rsp=%04x ref=%04x\n",
                 (unsigned long)i, dst4[i], ref4[i]);
      }
    }
    if (!bad)
      debugf("[gpSP]: RSP comppal OK -- 4 layers + per-tile palette verified\n");
    else
      debugf("[gpSP]: RSP comppal %lu mismatches of %lu\n",
             (unsigned long)bad, (unsigned long)(LB * 2));
  }

  /* Verify the RSP blit against the CPU conversion.  This matters more than
     the usual correctness check: the target is real hardware, and ares has
     no RDP backend, so nothing here ever displays the framebuffer.  A wrong
     blit would be completely invisible in the test environment and only
     show up on the console. */
  {
    static u16 bsrc[256] __attribute__((aligned(16)));
    static u16 bref[256] __attribute__((aligned(16)));
    static u16 bdst[256] __attribute__((aligned(16)));
    u32 i, bad = 0;
    for (i = 0; i < 256; i++) { bsrc[i] = (u16)(i * 211 + 7); bdst[i] = 0; }

    /* CPU reference: the same expression n64_video.c uses */
    for (i = 0; i < 256; i++) {
      u32 p = bsrc[i];
      bref[i] = (u16)(((p << 11) & 0xF800) | ((p << 1) & 0x07C0)
                    | ((p >> 9) & 0x003E) | 1);
    }
    data_cache_hit_writeback_invalidate(bsrc, sizeof(bsrc));
    data_cache_hit_writeback_invalidate(bdst, sizeof(bdst));

    /* one "scanline" of 256 px, 4 lines of 64 px so the rect path is used */
    n64_rsp_blit(bsrc, bdst, 128, 4, 128);
    data_cache_hit_invalidate(bdst, sizeof(bdst));

    for (i = 0; i < 256; i++)
      if (bdst[i] != bref[i]) {
        if (bad++ < 3)
          debugf("[gpSP]: RSP blit MISMATCH at %lu: rsp=%04x cpu=%04x\n",
                 (unsigned long)i, bdst[i], bref[i]);
      }
    if (!bad) debugf("[gpSP]: RSP blit OK -- matches CPU conversion\n");
    else      debugf("[gpSP]: RSP blit %lu/%u mismatches\n",
                     (unsigned long)bad, 256u);
  }
}
