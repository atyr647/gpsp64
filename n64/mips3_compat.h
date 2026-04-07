/* gameplaySP - MIPS III Compatibility Layer for N64 (VR4300)
 *
 * The VR4300 implements MIPS III, which lacks:
 *   - movz/movn (MIPS IV conditional moves)
 *   - ext/ins/seb/seh/rotr (MIPS32 R2 - already handled by existing fallbacks)
 *
 * This header provides movz/movn replacements using branch sequences.
 * Each replacement emits 3 instructions instead of 1, which means
 * branch offsets in surrounding code must be adjusted.
 *
 * The approach: define macros that expand to branch sequences.
 * Callers that use hardcoded branch offsets must add
 * MOVZ_EXTRA_INSTRS for each movz/movn in the skipped region.
 *
 * N64 port Copyright (C) 2026
 */

#ifndef MIPS3_COMPAT_H
#define MIPS3_COMPAT_H

#ifdef N64

/* Number of extra instructions a movz/movn emits vs native (3 - 1 = 2) */
#define MOVZ_EXTRA_INSTRS 2

/*
 * movz rd, rs, rt  - "if (rt == 0) rd = rs"
 *
 * Replaced with:
 *   bne  rt, $zero, 1f   ; skip if rt != 0
 *   nop                  ; branch delay slot
 *   addu rd, rs, $zero   ; rd = rs (only reached if rt == 0)
 * 1:
 *
 * Note: bne offset=1 means skip 1 instruction after the delay slot,
 * which skips the addu.
 */
#define mips_emit_movz(rd, rs, rt)                                             \
  mips_emit_b(bne, rt, reg_zero, 1);                                           \
  mips_emit_nop();                                                             \
  mips_emit_addu(rd, rs, reg_zero)

/*
 * movn rd, rs, rt  - "if (rt != 0) rd = rs"
 *
 * Replaced with:
 *   beq  rt, $zero, 1f   ; skip if rt == 0
 *   nop                  ; branch delay slot
 *   addu rd, rs, $zero   ; rd = rs (only reached if rt != 0)
 * 1:
 */
#define mips_emit_movn(rd, rs, rt)                                             \
  mips_emit_b(beq, rt, reg_zero, 1);                                           \
  mips_emit_nop();                                                             \
  mips_emit_addu(rd, rs, reg_zero)

#endif /* N64 */

#endif /* MIPS3_COMPAT_H */
