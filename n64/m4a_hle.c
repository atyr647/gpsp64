/* gameplaySP - N64: native substitution for the m4a sound mixer
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
 *     pcmSamplesPerVBlank = 224 and pcmFreq = 13379 Hz.
 *   - The mixer's inner loop reads the right channel accumulator at
 *     [r5, #1584] -- 1584 is exactly PCM_DMA_BUF_SIZE.
 *   - The loop body is m4a's 8-bit sample fetch with linear
 *     interpolation (ldrsb / sub / mla) unrolled four ways, plus the
 *     reverb pass that averages four channels and shifts right by 9.
 *
 * The routine's whole output is 2 x 1584 bytes of PCM in the DMA buffer.
 * gpSP resamples that for playback -- except this port never calls
 * n64_audio_render_frame(), so the samples are computed and discarded.
 * We are spending two thirds of the emulated CPU on silence.
 *
 * This module skips it.  The mixer is entered in Thumb at a
 * signature-matched address and left through an epilogue that unwinds a
 * stack frame the ROM-side caller built, so we cannot simply return: we
 * patch the entry to branch directly to that epilogue.  Everything the
 * epilogue does -- restore soundInfo->ident, add sp, pop r0-r11, bx r3
 * -- still runs, so the caller sees an ordinary return.
 *
 * What still runs is just as important.  MPlayMain (in ROM, reached
 * through soundInfo->func) is the sequencer: it advances tracks, starts
 * and stops notes, and maintains the channel state that game code polls
 * through IsFanfareTaskInactive and friends.  Only the PCM mixing --
 * pure signal generation with no game-visible side effects -- is
 * skipped.
 *
 * This is the first half of a native port of the mixer.  The second half
 * is to implement SoundMainRAM in C against the same SoundInfo layout,
 * which restores audio at roughly a twentieth of the emulated cost; the
 * hook point and the state layout established here are what that needs.
 *
 * N64 port Copyright (C) 2026
 */

#include "../common.h"
#include "m4a_hle.h"

#ifdef N64_M4A_STUB

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

/* Unconditional Thumb branch from the entry to the epilogue.  The Thumb
 * PC reads as instruction+4, so the encoded offset is (delta - 4) / 2. */
#define M4A_BRANCH_HW \
  (0xE000u | ((((M4A_EPILOGUE_DELTA - 4) >> 1)) & 0x7FFu))

/* IWRAM offset of the patched halfword, 0 = not installed. */
static u32 m4a_patch_off = 0;
u32 m4a_hle_entry_pc = 0;

/* A full IWRAM scan is ~16K iterations, so it must not run every frame
 * forever on a game whose driver we do not recognise.  The ident gate
 * below already keeps it from running before m4a starts; this bounds
 * the case where m4a is present but its code does not match. */
static u32 m4a_scan_attempts = 0;
#define M4A_MAX_SCANS 240

static u32 m4a_read32(const u8 *iw, u32 off)
{
  return (u32)iw[off] | ((u32)iw[off + 1] << 8) |
         ((u32)iw[off + 2] << 16) | ((u32)iw[off + 3] << 24);
}

static int m4a_match(const u8 *iw, u32 off, const u8 *sig, u32 len)
{
  for (u32 i = 0; i < len; i++)
    if (iw[off + i] != sig[i]) return 0;
  return 1;
}

/* Called once per emulated frame.  Before the driver is found this scans
 * IWRAM; afterwards it is a single halfword compare, which also repairs
 * the patch if the game ever re-copies the driver. */
void m4a_hle_frame(void)
{
  u8 *iw = m4a_iwram_base();

  if (m4a_patch_off) {
    if (iw[m4a_patch_off]     == (u8)(M4A_BRANCH_HW & 0xFF) &&
        iw[m4a_patch_off + 1] == (u8)(M4A_BRANCH_HW >> 8))
      return;
    m4a_patch_off = 0;    /* driver was reloaded; re-find it */
  }

  if (m4a_scan_attempts >= M4A_MAX_SCANS) return;

  /* Cheap gate: no m4a, nothing to do. */
  {
    u32 sip = m4a_read32(iw, SOUND_INFO_PTR & 0x7FFF);
    if ((sip >> 24) != 0x03) return;
    if (m4a_read32(iw, sip & 0x7FFF) != M4A_IDENT) return;
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
    iw[off]     = (u8)(M4A_BRANCH_HW & 0xFF);
    iw[off + 1] = (u8)(M4A_BRANCH_HW >> 8);
    m4a_patch_off = off;
    m4a_hle_entry_pc = 0x03000000u + off;
    return;
  }
}

#endif  /* N64_M4A_STUB */
