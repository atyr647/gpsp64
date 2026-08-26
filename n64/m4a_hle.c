/* gameplaySP - N64: native substitution inside the m4a sound mixer
 *
 * Profiling Pokemon Emerald under the interpreter (native/bench.sh with
 * -DPROFILE_REGIONS) says something startling about where the emulated
 * CPU time goes:
 *
 *     interpreted instructions by region:
 *       00 BIOS         75/frame    0.3%
 *       03 IWRAM     19820/frame   68.3%   ARM 19553  Thumb 267
 *       08 ROM0       9113/frame   31.4%   ARM     2  Thumb 9112
 *
 *     IWRAM entry points (PC arriving from outside IWRAM):
 *       0x03001aa8       1.0 entries/frame
 *
 * Two thirds of every emulated instruction belongs to a single routine,
 * entered exactly once per frame, that the game copies into IWRAM at
 * boot.  Disassembling it identifies it beyond doubt: it is
 * SoundMainRAM from Nintendo's m4a sound driver (asm/m4a_1.s in the
 * pokeemerald decomp).  The evidence:
 *
 *   - IWRAM 0x03007FF0 holds SOUND_INFO_PTR; the struct it points at
 *     starts with m4a's ident, 0x68736D53, and reports
 *     pcmSamplesPerVBlank = 224 and pcmFreq = 13379 Hz.  Its pcmBuffer
 *     sits at +0x350 and its channel array at +0x50, both confirmed
 *     against the code that indexes them.
 *   - The mixer's inner loop reads the right channel accumulator at
 *     [r5, #1584] -- 1584 is exactly PCM_DMA_BUF_SIZE.
 *   - The loops are m4a's: an 8-bit sample fetch with linear
 *     interpolation, four samples packed per accumulator word, plus the
 *     reverb pass that sums four channels and shifts right by 9.
 *
 * There are two ways to make that cheaper, and this file can do either.
 *
 * N64_M4A_STUB skips the mixer outright.  It is correct only because
 * this port never calls n64_audio_render_frame(), so the PCM the mixer
 * produces is computed and thrown away; measured +42.2% frame rate.
 *
 * N64_M4A_NATIVE keeps the driver running and replaces only its hot
 * loops with native code.  The per-64-byte-block profile is lopsided:
 *
 *     0x03001dc0  9416/frame  47.5%    resampling mix loop
 *     0x03001ac0  3361/frame  17.0%    reverb loop
 *     0x03001e00  2964/frame  15.0%    resampling mix loop, tail
 *     0x03001cc0  1498/frame   7.6%    unity-rate mix loop
 *     0x03001c80   961/frame   5.3%
 *     0x03001d80   926/frame   4.7%
 *
 * Everything else -- the envelope state machine, the compressed and
 * reverse-playback paths, the loop-point handling when a sample runs
 * out -- is under 1% and stays interpreted, so it stays correct without
 * being reimplemented.  Each hot loop is replaced by an ARM SWI in the
 * copied code; gpSP already routes SWIs through bios_hle_swi, so the
 * handler runs the loop natively, writes back the registers the loop
 * would have left, and points the PC at wherever that loop exits.  A
 * loop that hits an edge case it does not model returns control to the
 * interpreter at exactly the instruction that handles it.
 *
 * Emulated time is charged for every instruction the loop would have
 * executed, so the game's timing does not shift; only host time is
 * saved.
 *
 * N64 port Copyright (C) 2026
 */

#include "../common.h"
#include "../gba_memory.h"
#include "m4a_hle.h"

#if defined(N64_M4A_STUB) || defined(N64_M4A_NATIVE)

/* m4a's SoundInfo::ident, "Smsh" little-endian.  SoundMainRAM zeroes it
 * on entry as a re-entrancy guard and restores it on exit. */
#define M4A_IDENT       0x68736D53u
#define SOUND_INFO_PTR  0x03007FF0u

/* First 16 bytes of SoundMainRAM.  In Thumb:
 *     ldrb r3, [r0, #5]     ; soundInfo->reverb
 *     cmp  r3, #0
 *     beq  +0x58            ; skip the reverb pass
 *     add  r1, pc, #4
 *     bx   r1               ; ...and switch to ARM
 * followed by the ARM word 0xE3540002 (cmp r4, #2) it lands on. */
static const u8 m4a_entry_sig[16] = {
  0x43, 0x79, 0x00, 0x2b, 0x2c, 0xd0, 0x01, 0xa1,
  0x08, 0x47, 0x00, 0x00, 0x02, 0x00, 0x54, 0xe3
};

/* The epilogue, 0x3A2 bytes further on, back in Thumb:
 *     ldr r0, [sp, #24]     ; soundInfo
 *     ldr r3, [pc, #16]     ; M4A_IDENT
 *     str r3, [r0]
 *     add sp, #28
 *     pop {r0-r7}
 *     mov r8, r0            ; ...r11, r3, then bx r3 */
static const u8 m4a_epilogue_sig[12] = {
  0x06, 0x98, 0x04, 0x4b, 0x03, 0x60, 0x07, 0xb0,
  0xff, 0xbc, 0x80, 0x46
};
#define M4A_EPILOGUE_DELTA 0x3A2

/* IWRAM offset of the routine entry, 0 = driver not found yet. */
static u32 m4a_entry_off = 0;
u32 m4a_hle_entry_pc = 0;

/* A full IWRAM scan is ~16K iterations, so it must not run every frame
 * forever on a game whose driver we do not recognise.  The ident gate
 * below already keeps it from running before m4a starts; this bounds
 * the case where m4a is present but its code does not match. */
static u32 m4a_scan_attempts = 0;
#define M4A_MAX_SCANS 240

/* gpSP keeps live IWRAM in the upper half of iwram_raw. */
#define IW(off) (iwram_raw + 0x8000 + (off))

static u32 m4a_rd32(const u8 *p)
{
  return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static void m4a_wr32(u8 *p, u32 v)
{
  p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}

static int m4a_match(const u8 *base, u32 off, const u8 *sig, u32 len)
{
  for (u32 i = 0; i < len; i++)
    if (base[off + i] != sig[i]) return 0;
  return 1;
}

/* ------------------------------------------------------------------ */

#ifdef N64_M4A_STUB

/* Unconditional Thumb branch from the entry to the epilogue.  The Thumb
 * PC reads as instruction+4, so the encoded offset is (delta - 4) / 2. */
#define M4A_BRANCH_HW \
  (0xE000u | ((((M4A_EPILOGUE_DELTA - 4) >> 1)) & 0x7FFu))

static int m4a_install(u8 *iw, u32 off)
{
  iw[off]     = (u8)(M4A_BRANCH_HW & 0xFF);
  iw[off + 1] = (u8)(M4A_BRANCH_HW >> 8);
  return 1;
}

static int m4a_installed(const u8 *iw, u32 off)
{
  return iw[off]     == (u8)(M4A_BRANCH_HW & 0xFF) &&
         iw[off + 1] == (u8)(M4A_BRANCH_HW >> 8);
}

#else   /* N64_M4A_NATIVE */

/* Each hot loop is identified by its exact code, so a driver build that
 * differs anywhere in the loop is simply left interpreted rather than
 * mis-executed.  Offsets are from the routine entry. */

/* The reverb pass, 0x03001AC4 in the observed build.  Per sample it
 * sums the current and next-block samples of both channels, scales by
 * soundInfo->reverb / 512, and writes the result to both:
 *
 *     ldrsb r0, [r5, r6]        ; right, this block
 *     ldrsb r1, [r5]            ; left,  this block
 *     add   r0, r0, r1
 *     ldrsb r1, [r7, r6]        ; right, next block
 *     add   r0, r0, r1
 *     ldrsb r1, [r7], #1        ; left,  next block
 *     add   r0, r0, r1
 *     mul   r1, r0, r3
 *     mov   r0, r1, asr #9
 *     tst   r0, #0x80
 *     addne r0, r0, #1
 *     strb  r0, [r5, r6]
 *     strb  r0, [r5], #1
 *     subs  r4, r4, #1
 *     bgt   loop                */
#define M4A_REVERB_OFF   0x01C
#define M4A_REVERB_EXIT  0x058
static const u32 m4a_reverb_code[] = {
  0xE19500D6, 0xE1D510D0, 0xE0800001, 0xE19710D6, 0xE0800001,
  0xE0D710D1, 0xE0800001, 0xE0010390, 0xE1A004C1, 0xE3100080,
  0x12800001, 0xE7C50006, 0xE4C50001, 0xE2544001, 0xCAFFFFF0
};

/* The resampling mix loop, 0x03001DB4.  r9 is a 9.23 fixed-point
 * position, r4 the step; each output sample is linearly interpolated
 * between two 8-bit source samples and accumulated into a word holding
 * four output samples, one per byte, rotated into place:
 *
 *   loop:
 *     ldr   r6, [r5]            ; left accumulator word
 *     ldr   r7, [r5, #1584]     ; right accumulator word
 *     mul   lr, r9, r1          ; fraction * delta
 *     add   lr, r0, lr, asr #23 ; interpolated sample
 *     mul   r12, r10, lr        ; * (envelopeVolumeRight << 16)
 *     bic   r12, r12, #0x00FF0000
 *     add   r6, r12, r6, ror #8
 *     mul   r12, r11, lr        ; * (envelopeVolumeLeft << 16)
 *     bic   r12, r12, #0x00FF0000
 *     add   r7, r12, r7, ror #8
 *     add   r9, r9, r4          ; advance position
 *     movs  lr, r9, lsr #23     ; whole samples crossed
 *     beq   pack
 *     bic   r9, r9, #0x3F800000
 *     subs  r2, r2, lr          ; source samples remaining
 *     ble   out_of_samples
 *     subs  lr, lr, #1
 *     addeq r0, r0, r1
 *     ldrsbne r0, [r3, lr]!
 *     ldrsb r1, [r3, #1]!
 *     sub   r1, r1, r0
 *   pack:
 *     adds  r5, r5, #0x40000000 ; four-sample group counter in the
 *     blo   loop                ;   unused top bits of the pointer
 *     str   r7, [r5, #1584]
 *     str   r6, [r5], #4
 *     subs  r8, r8, #4
 *     bgt   loop                */
#define M4A_RESAMP_OFF        0x30C
#define M4A_RESAMP_EXIT       0x378   /* sub r3, r3, #1 ; pop {r4,r12} */
#define M4A_RESAMP_EXHAUSTED  0x28C   /* loop-point / end-of-sample path */
static const u32 m4a_resamp_code[] = {
  0xE5956000, 0xE5957630, 0xE00E0199, 0xE080EBCE, 0xE00C0E9A,
  0xE3CCC8FF, 0xE08C6466, 0xE00C0E9B, 0xE3CCC8FF, 0xE08C7467,
  0xE0899004, 0xE1B0EBA9, 0x0A000007, 0xE3C995FE, 0xE052200E,
  0xDAFFFFCF, 0xE25EE001, 0x00800001, 0x11B300DE, 0xE1F310D1,
  0xE0411000, 0xE2955101, 0x3AFFFFEA, 0xE5857630, 0xE4856004,
  0xE2588004, 0xCAFFFFE4
};

/* The unity-rate mix loops, 0x03001CA8 and 0x03001CEC.  Used when the
 * sample rate needs no resampling: one source byte per output sample,
 * no interpolation, otherwise the same four-per-word accumulation.
 *
 *     ldr   r6, [r5]
 *     ldr   r7, [r5, #1584]
 *   inner:
 *     ldrsb r0, [r3], #1
 *     mul   r1, r10, r0
 *     bic   r1, r1, #0x00FF0000
 *     add   r6, r1, r6, ror #8
 *     mul   r1, r11, r0
 *     bic   r1, r1, #0x00FF0000
 *     add   r7, r1, r7, ror #8
 *    [subs  r2, r2, #1          ; second variant only
 *     beq   out_of_samples]
 *     adds  r5, r5, #0x40000000
 *     blo   inner
 *     str   r7, [r5, #1584]
 *     str   r6, [r5], #4
 *     subs  r8, r8, #4
 *     bgt   loop
 *
 * The first variant is entered when the driver has already proved there
 * are enough source samples for the whole block, so it omits the count;
 * the second carries it and bails to the loop-point code. */
#define M4A_UNITY_OFF        0x200
#define M4A_UNITY_EXIT       0x23C   /* adds r8, r8, r9 ; beq ... */
static const u32 m4a_unity_code[] = {
  0xE5956000, 0xE5957630, 0xE0D300D1, 0xE001009A, 0xE3C118FF,
  0xE0816466, 0xE001009B, 0xE3C118FF, 0xE0817467, 0xE2955101,
  0x3AFFFFF6, 0xE5857630, 0xE4856004, 0xE2588004, 0xCAFFFFF0
};

#define M4A_UNITYC_OFF       0x244
#define M4A_UNITYC_EXIT      0x288   /* b <end of channel> */
#define M4A_UNITYC_EXHAUSTED 0x2BC   /* loop-point / end-of-sample path */
static const u32 m4a_unityc_code[] = {
  0xE5956000, 0xE5957630, 0xE0D300D1, 0xE001009A, 0xE3C118FF,
  0xE0816466, 0xE001009B, 0xE3C118FF, 0xE0817467, 0xE2522001,
  0x0A000012, 0xE2955101, 0x3AFFFFF4, 0xE5857630, 0xE4856004,
  0xE2588004, 0xCAFFFFD2
};

/* ARM `swi 0xFn0000`.  gpSP dispatches on (opcode >> 16) & 0xFF and, when
 * bios_hle_swi reports the call handled, steps the PC on by one
 * instruction -- so a handler that wants to resume somewhere else just
 * sets reg[REG_PC] to (target - 4). */
#define M4A_SWI(n) (0xEF000000u | ((u32)(n) << 16))

struct m4a_loop {
  u32 off, nwords;
  const u32 *code;
  u8 swi;
};
static const struct m4a_loop m4a_loops[] = {
  { M4A_REVERB_OFF,  sizeof(m4a_reverb_code) / 4,  m4a_reverb_code,  M4A_SWI_REVERB },
  { M4A_RESAMP_OFF,  sizeof(m4a_resamp_code) / 4,  m4a_resamp_code,  M4A_SWI_RESAMP },
  { M4A_UNITY_OFF,   sizeof(m4a_unity_code) / 4,   m4a_unity_code,   M4A_SWI_UNITY  },
  { M4A_UNITYC_OFF,  sizeof(m4a_unityc_code) / 4,  m4a_unityc_code,  M4A_SWI_UNITYC },
};
#define M4A_NLOOPS (sizeof(m4a_loops) / sizeof(m4a_loops[0]))

static int m4a_install(u8 *iw, u32 off)
{
  /* Verify every loop before touching any of them, so a driver we only
   * partly recognise is left completely alone. */
  for (u32 l = 0; l < M4A_NLOOPS; l++) {
    const struct m4a_loop *lp = &m4a_loops[l];
    for (u32 i = 0; i < lp->nwords; i++)
      if (m4a_rd32(iw + off + lp->off + i * 4) != lp->code[i] &&
          m4a_rd32(iw + off + lp->off + i * 4) != M4A_SWI(lp->swi))
        return 0;
  }
  for (u32 l = 0; l < M4A_NLOOPS; l++)
    m4a_wr32(iw + off + m4a_loops[l].off, M4A_SWI(m4a_loops[l].swi));
  return 1;
}

static int m4a_installed(const u8 *iw, u32 off)
{
  for (u32 l = 0; l < M4A_NLOOPS; l++)
    if (m4a_rd32(iw + off + m4a_loops[l].off) != M4A_SWI(m4a_loops[l].swi))
      return 0;
  return 1;
}

/* ---- the loops themselves ---------------------------------------- */

/* Both loops address only the PCM accumulator and the sample data the
 * driver already resolved, and on this game both live in IWRAM.  Anything
 * else is refused and left to the interpreter. */
static u8 *m4a_ptr(u32 addr, u32 len)
{
  if ((addr >> 24) != 0x03) return 0;
  u32 off = addr & 0x7FFF;
  if (off + len > 0x8000) return 0;
  return IW(off);
}

static s32 m4a_s8(const u8 *p) { return (s32)(s8)*p; }

/* Charged per ARM instruction the replaced loop would have executed, so
 * the game sees the same emulated time it always did. */
static u32 m4a_cyc(u32 insns)
{
  return insns * (u32)ws_cyc_seq[3][1];
}

static u32 m4a_run_reverb(u32 entry, u32 *cycles)
{
  u32 n = reg[4];
  u32 pl = reg[5], pn = reg[7], stride = reg[6];
  s32 rev = (s32)reg[3];

  if ((s32)n <= 0) { *cycles = 0; return entry + M4A_REVERB_EXIT; }

  u8 *l = m4a_ptr(pl, n);
  u8 *r = m4a_ptr(pl + stride, n);
  u8 *nl = m4a_ptr(pn, n);
  u8 *nr = m4a_ptr(pn + stride, n);
  if (!l || !r || !nl || !nr) return 0;    /* let the interpreter do it */

  s32 v = 0, last = 0;
  for (u32 i = 0; i < n; i++) {
    s32 sum = m4a_s8(&r[i]) + m4a_s8(&l[i]) + m4a_s8(&nr[i]) + m4a_s8(&nl[i]);
    last = m4a_s8(&nl[i]);
    v = (sum * rev) >> 9;
    if (v & 0x80) v += 1;
    r[i] = (u8)v;
    l[i] = (u8)v;
  }

  /* Leave the scratch registers holding what the loop would have left,
   * rather than relying on them being dead after the exit. */
  reg[0] = (u32)v;
  reg[1] = (u32)last;
  reg[4] = 0;
  reg[5] = pl + n;
  reg[7] = pn + n;
  *cycles = m4a_cyc(n * 15);
  return entry + M4A_REVERB_EXIT;
}

/* Source samples live in the cart, so this needs the same page-aware
 * resolution the rest of the port uses -- ROM_BUFFER_SIZE=2 means most
 * of the cart is not resident.  The window is re-resolved only when the
 * read pointer leaves it, which for a monotonically advancing sample
 * pointer is once per 32 KB. */
struct m4a_win { const u8 *base; u32 lo, hi; };

static int m4a_src_window(struct m4a_win *w, u32 addr)
{
  u32 region = addr >> 24;
  if (region == 0x03) {
    w->lo = addr & ~0x7FFFu;
    w->hi = w->lo + 0x8000;
    w->base = IW(0) - w->lo;
    return 1;
  }
  if (region == 0x02) {
    w->lo = addr & ~0x3FFFFu;
    w->hi = w->lo + 0x40000;
    w->base = ewram_raw - w->lo;
    return 1;
  }
  if (region >= 0x08 && region <= 0x0D) {
    u8 *map = memory_map_read[addr >> 15];
    if (!map) map = load_gamepak_page((addr >> 15) & 0x3FF);
    if (!map) return 0;
    w->lo = addr & ~0x7FFFu;
    w->hi = w->lo + 0x8000;
    w->base = map - w->lo;
    return 1;
  }
  return 0;
}

/* Resolve the whole span a call can touch before it starts.  A read that
 * failed halfway through would leave the accumulators and the channel
 * pointers half-updated with nowhere safe to resume, so the span is
 * checked up front and the call declined if it does not fit in one
 * window -- the interpreter then runs that stretch itself. */
static int m4a_src_span(struct m4a_win *w, u32 addr, u32 bytes)
{
  if (!m4a_src_window(w, addr)) return 0;
  return (addr + bytes) <= w->hi;
}

static inline s32 m4a_src_at(const struct m4a_win *w, u32 addr)
{
  return (s32)(s8)w->base[addr];
}

/* The right-hand accumulator sits a fixed PCM_DMA_BUF_SIZE above the
 * left one; it is an immediate in the code, not a register. */
#define M4A_PCM_STRIDE 1584

static u32 m4a_run_resamp(u32 entry, u32 *cycles)
{
  u32 pcm   = reg[5];           /* byte address in bits 0-29, group count
                                 * in bits 30-31, exactly as the original */
  u32 fw    = reg[9], step = reg[4];
  u32 envr  = reg[10], envl = reg[11];
  s32 cur   = (s32)reg[0], delta = (s32)reg[1];
  s32 remain = (s32)reg[2];
  u32 src   = reg[3];
  s32 outleft = (s32)reg[8];
  u32 acc6 = reg[6], acc7 = reg[7];
  u32 insns = 0;
  struct m4a_win win = { 0, 1, 0 };

  if (((pcm & 0x3F000000u) != 0x03000000u) || (s32)outleft <= 0)
    return 0;                   /* not a shape we model */

  /* Each output sample advances the source by step >> 23, and the loop
   * stops after outleft of them or when the source runs out. */
  { u64 adv = ((u64)(u32)outleft * (u64)step) >> 23;
    u32 span = (u32)(adv > (u64)(u32)remain ? (u32)remain : adv) + 2;
    if (!m4a_src_span(&win, src, span)) return 0; }

  /* The write pointer advances four bytes per group and stops after
   * outleft output samples, so the whole destination span is known too. */
  { u32 addr = pcm & 0x3FFFFFFFu;
    if (!m4a_ptr(addr, (u32)outleft) ||
        !m4a_ptr(addr + M4A_PCM_STRIDE, (u32)outleft)) return 0; }

  for (;;) {
    u32 addr = pcm & 0x3FFFFFFFu;
    u8 *a6 = IW(addr & 0x7FFF);
    u8 *a7 = a6 + M4A_PCM_STRIDE;
    acc6 = m4a_rd32(a6);
    acc7 = m4a_rd32(a7);
    insns += 2;

    for (;;) {
      s32 s = cur + ((s32)(fw * (u32)delta) >> 23);
      u32 p;

      p = (envr * (u32)s) & ~0x00FF0000u;
      acc6 = p + ((acc6 >> 8) | (acc6 << 24));
      p = (envl * (u32)s) & ~0x00FF0000u;
      acc7 = p + ((acc7 >> 8) | (acc7 << 24));

      fw += step;
      insns += 9;

      u32 whole = fw >> 23;
      if (whole) {
        insns += 3;
        fw &= ~0x3F800000u;
        remain -= (s32)whole;
        if (remain <= 0) {
          /* The source ran out mid-group.  Hand back the exact register
           * state the `ble` would have left and let the driver's own
           * loop-point code take it from here -- accumulators included,
           * because they have not been stored yet. */
          reg[0] = (u32)cur;   reg[1] = (u32)delta;
          reg[2] = (u32)remain; reg[3] = src;
          reg[5] = pcm;
          reg[6] = acc6;       reg[7] = acc7;
          reg[8] = (u32)outleft;
          reg[9] = fw;         reg[14] = whole;
          *cycles = m4a_cyc(insns);
          return entry + M4A_RESAMP_EXHAUSTED;
        }
        insns += 4;
        if (whole == 1) {
          cur = cur + delta;
        } else {
          src += whole - 1;
          cur = m4a_src_at(&win, src);
        }
        src += 1;
        delta = m4a_src_at(&win, src) - cur;
      }

      /* Four output samples per accumulator word, counted in the unused
       * top bits of the pointer exactly as the original does: the fourth
       * add carries out, and `blo` stops looping. */
      u32 before = pcm;
      pcm += 0x40000000u;
      insns += 2;
      if (pcm < before) break;          /* carry out: group complete */
    }

    m4a_wr32(a7, acc7);
    m4a_wr32(a6, acc6);
    pcm += 4;
    outleft -= 4;
    insns += 4;
    if (outleft <= 0) break;
  }

  reg[0] = (u32)cur;  reg[1] = (u32)delta;
  reg[2] = (u32)remain;
  reg[3] = src;                 /* the exit instruction does the -1 */
  reg[5] = pcm;
  reg[6] = acc6;      reg[7] = acc7;
  reg[8] = (u32)outleft;
  reg[9] = fw;
  *cycles = m4a_cyc(insns);
  return entry + M4A_RESAMP_EXIT;
}

/* Both unity-rate variants, distinguished by whether the source-sample
 * count is carried (and so whether the loop can run out mid-block). */
static u32 m4a_run_unity(u32 entry, int counted, u32 *cycles)
{
  u32 pcm  = reg[5];
  u32 envr = reg[10], envl = reg[11];
  s32 remain = (s32)reg[2];
  u32 src  = reg[3];
  s32 outleft = (s32)reg[8];
  u32 acc6, acc7;
  u32 insns = 0;
  s32 lastsample = 0;
  struct m4a_win win = { 0, 1, 0 };

  if (((pcm & 0x3F000000u) != 0x03000000u) || outleft <= 0)
    return 0;

  /* One source byte per output sample, so the span is exact. */
  { u32 span = (u32)outleft;
    if (counted && (u32)remain < span) span = (u32)remain;
    if (!m4a_src_span(&win, src, span + 1)) return 0; }

  { u32 addr = pcm & 0x3FFFFFFFu;
    if (!m4a_ptr(addr, (u32)outleft) ||
        !m4a_ptr(addr + M4A_PCM_STRIDE, (u32)outleft)) return 0; }

  for (;;) {
    u32 addr = pcm & 0x3FFFFFFFu;
    u8 *a6 = IW(addr & 0x7FFF);
    u8 *a7 = a6 + M4A_PCM_STRIDE;
    acc6 = m4a_rd32(a6);
    acc7 = m4a_rd32(a7);
    insns += 2;

    for (;;) {
      s32 s;
      u32 p;
      s = m4a_src_at(&win, src);
      src += 1;
      lastsample = s;

      p = (envr * (u32)s) & ~0x00FF0000u;
      acc6 = p + ((acc6 >> 8) | (acc6 << 24));
      p = (envl * (u32)s) & ~0x00FF0000u;
      acc7 = p + ((acc7 >> 8) | (acc7 << 24));
      insns += 7;

      if (counted) {
        insns += 2;
        if (--remain == 0) {
          /* Accumulators stay in registers: the loop-point code rotates
           * and stores them itself. */
          reg[2] = 0;    reg[3] = src;
          reg[5] = pcm;
          reg[6] = acc6; reg[7] = acc7;
          reg[8] = (u32)outleft;
          *cycles = m4a_cyc(insns);
          return entry + M4A_UNITYC_EXHAUSTED;
        }
      }

      u32 before = pcm;
      pcm += 0x40000000u;
      insns += 2;
      if (pcm < before) break;
    }

    m4a_wr32(a7, acc7);
    m4a_wr32(a6, acc6);
    pcm += 4;
    outleft -= 4;
    insns += 4;
    if (outleft <= 0) break;
  }

  reg[0] = (u32)lastsample;
  reg[2] = (u32)remain;
  reg[3] = src;
  reg[5] = pcm;
  reg[6] = acc6; reg[7] = acc7;
  reg[8] = (u32)outleft;
  *cycles = m4a_cyc(insns);
  return entry + (counted ? M4A_UNITYC_EXIT : M4A_UNITY_EXIT);
}

/* Returns 1 if the loop was executed natively.  reg[REG_PC] is set so
 * that gpSP's `arm_pc_offset(4)` after a handled SWI lands on the
 * instruction the loop would have reached. */
/* Hand the loop back to the interpreter without having changed anything.
 * Returning 0 here would be wrong in a way that is easy to miss: gpSP
 * would treat the call as a real BIOS SWI and vector to 0x00000008 with
 * a number the BIOS does not implement.  Instead put the original
 * instruction back and re-run it; m4a_hle_frame() notices the loop is no
 * longer patched and re-patches it at the next frame boundary. */
static int m4a_unpatch_and_rerun(u32 swi_num)
{
  u32 pc = reg[REG_PC];
  if ((pc >> 24) != 0x03) return 0;
  for (u32 l = 0; l < M4A_NLOOPS; l++) {
    if (m4a_loops[l].swi != swi_num) continue;
    m4a_wr32(IW(pc & 0x7FFF), m4a_loops[l].code[0]);
    reg[REG_PC] = pc - 4;       /* gpSP adds 4 to step over the SWI */
    return 1;
  }
  return 0;
}

/* Returns 1 if the SWI was consumed here.  reg[REG_PC] is set so that
 * gpSP's `arm_pc_offset(4)` after a handled SWI lands on the instruction
 * execution should continue at. */
int m4a_hle_arm_swi(u32 swi_num, u32 *cycles)
{
  const struct m4a_loop *lp = 0;
  u32 target = 0, entry;

  for (u32 l = 0; l < M4A_NLOOPS; l++)
    if (m4a_loops[l].swi == swi_num) { lp = &m4a_loops[l]; break; }
  if (!lp) return 0;

  /* Derive the routine's base from the PC of the SWI itself rather than
   * from whatever the IWRAM scan last stored.
   *
   * A savestate restores IWRAM with these patches already in it, so on
   * the very first frame after a load the SWI fires before
   * m4a_hle_frame() has had a chance to re-find the driver -- and a
   * stored base of zero sent the jump to 0x03000058, into IWRAM data,
   * where the game spun at 280,000 instructions a frame.  It only showed
   * up in savestates captured with this feature enabled, which is why the
   * one benchmark state that predated it kept loading cleanly.
   *
   * The SWI's own address is authoritative: it is only there because this
   * file wrote it at exactly entry + lp->off. */
  entry = reg[REG_PC] - lp->off;

  *cycles = 0;
  switch (swi_num) {
  case M4A_SWI_REVERB: target = m4a_run_reverb(entry, cycles); break;
  case M4A_SWI_RESAMP: target = m4a_run_resamp(entry, cycles); break;
  case M4A_SWI_UNITY:  target = m4a_run_unity(entry, 0, cycles); break;
  case M4A_SWI_UNITYC: target = m4a_run_unity(entry, 1, cycles); break;
  default: return 0;
  }
  if (!target) {
    *cycles = 0;
    return m4a_unpatch_and_rerun(swi_num);
  }

  reg[REG_PC] = target - 4;
  return 1;
}

#endif  /* N64_M4A_NATIVE */

/* ------------------------------------------------------------------ */

u32 m4a_entry_pc(void)
{
  return 0x03000000u + m4a_entry_off;
}

/* Called once per emulated frame.  Before the driver is found this scans
 * IWRAM; afterwards it is a handful of compares, which also repairs the
 * patch if the game ever re-copies the driver. */
void m4a_hle_frame(void)
{
  u8 *iw = IW(0);

  if (m4a_entry_off) {
    if (m4a_installed(iw, m4a_entry_off)) return;
    m4a_entry_off = 0;          /* driver was reloaded; re-find it */
  }

  if (m4a_scan_attempts >= M4A_MAX_SCANS) return;

  /* Cheap gate: no m4a, nothing to do. */
  {
    u32 sip = m4a_rd32(IW(SOUND_INFO_PTR & 0x7FFF));
    if ((sip >> 24) != 0x03) return;
    if (m4a_rd32(IW(sip & 0x7FFF)) != M4A_IDENT) return;
  }

  m4a_scan_attempts++;

  /* SoundMainRAM is copied to a game-chosen IWRAM address, so find it by
   * its code rather than by a hardcoded pointer. */
  for (u32 off = 0; off + M4A_EPILOGUE_DELTA + sizeof(m4a_epilogue_sig)
                    <= 0x8000; off += 2) {
    if (!m4a_match(iw, off, m4a_entry_sig, sizeof(m4a_entry_sig)))
      continue;
    if (!m4a_match(iw, off + M4A_EPILOGUE_DELTA, m4a_epilogue_sig,
                   sizeof(m4a_epilogue_sig)))
      continue;
    if (!m4a_install(iw, off))
      return;                   /* recognised the driver, not its guts */
    m4a_entry_off = off;
    m4a_hle_entry_pc = 0x03000000u + off;
    return;
  }
}

#endif  /* N64_M4A_STUB || N64_M4A_NATIVE */
