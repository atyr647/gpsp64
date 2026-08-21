############################################################################
# rsp_gbascan.S -- GBA scanline work offloaded to the RSP.
#
# WHY: rendering is 27.4 ms of a 55 ms frame and is 85% compute (only
# ~4.15 ms/frame of D-cache stall is attributable to it).  So the RSP is
# wanted as a *second execution unit*, not as a cache-miss fix.
#
# WHAT IS OFFLOADED: 4bpp index extraction, which is the bulk of the work.
# render_tile_Nbpp's INDXCOLOR path writes `pval | px_comb` -- an index plus
# flags, with NO palette lookup -- and that runs once per *layer*-pixel (4
# layers here).  Palette lookup happens once per *output* pixel later, at
# merge time.  This matters because per-lane palette lookup does NOT
# vectorise (the RSP has no gather; 16 entries would need 16x veq+vmrg,
# ~4 ops/pixel, a wash against the CPU).  Index extraction does vectorise.
#
# ENTRY 1 (offset 0): selftest -- DMA in, OR a constant, DMA out.
# ENTRY 2 (offset given by EXPAND_PC): expand 4bpp -> u16 indices.
############################################################################

#include <rsp.inc>

    .data
    .align 3

# Explicit numeric DMEM map.  lqv/sqv fold the offset at assembly time
# (the macro does offset/16 inside a .if), so these must be constants --
# a .data label is relocatable and will not do.
#define D_CTRL      0x0000
#define D_VCONST    0x0020   /* CTRL is 5 words = 20 bytes; clear it */
#define D_VFLAG     0x0060
#define D_VPAL      0x0080   /* 16 u16 subpalette */   /* VCONST is 4 vectors = 64 B from 0x20 */
#define D_SRCBUF    0x0400
#define D_DSTBUF    0x0800

    .global CTRL
CTRL:                       # 0x0000
CTRL_SRC:   .word 0
CTRL_DST:   .word 0
CTRL_LEN:   .word 0
CTRL_OR:    .word 0
CTRL_OP:    .word 0         # 0 = selftest, 1 = expand

    .org D_VCONST           # 0x0020
VCONST:
    .half 0x00FF,0x00FF,0x00FF,0x00FF,0x00FF,0x00FF,0x00FF,0x00FF   # byte mask
    .half 0x000F,0x000F,0x000F,0x000F,0x000F,0x000F,0x000F,0x000F   # nibble mask
    .half 0x0100,0x0100,0x0100,0x0100,0x0100,0x0100,0x0100,0x0100   # >>8 via vmudl
    .half 0x1000,0x1000,0x1000,0x1000,0x1000,0x1000,0x1000,0x1000   # >>4 via vmudl

    .org D_VFLAG            # 0x0060
VFLAG:
    .half 0,0,0,0,0,0,0,0

    .org D_VPAL             # 0x0080
VPAL:
    .half 0x1000,0x1001,0x1002,0x1003,0x1004,0x1005,0x1006,0x1007
    .half 0x1008,0x1009,0x100A,0x100B,0x100C,0x100D,0x100E,0x100F

    .text
    .align 3
    .global _start
_start:
    # Dispatch on CTRL_OP rather than entering at a second start_pc:
    # rsp_run_async() always starts at the ucode's start_pc, so selecting
    # work via the control block keeps one ucode and one entry point.
    lw      t9, D_CTRL+16(zero)
    li      t8, 2
    beq     t9, t8, palette_entry
     nop
    bne     t9, zero, expand_entry
     nop

    # ---- selftest: DMA in, OR, DMA out ---------------------------------
    lw      s0, D_CTRL+0(zero)
    lw      t3, D_CTRL+8(zero)
    li      s4, D_SRCBUF
    addi    t0, t3, -1
    jal     DMAIn
     li     t1, 0

    lw      t4, D_CTRL+12(zero)
    li      t5, D_SRCBUF
    lw      t3, D_CTRL+8(zero)
    add     t6, t5, t3
1:  lhu     t7, 0(t5)
    or      t7, t7, t4
    sh      t7, 0(t5)
    addi    t5, t5, 2
    bne     t5, t6, 1b
     nop

    lw      s0, D_CTRL+4(zero)
    lw      t3, D_CTRL+8(zero)
    li      s4, D_SRCBUF
    addi    t0, t3, -1
    jal     DMAOut
     li     t1, 0
    break

    # ---- expand entry: 4bpp bytes -> u16 indices ------------------------
    # In : CTRL_SRC = packed 4bpp bytes, CTRL_LEN = byte count (mult of 16)
    # Out: CTRL_DST = 4 planes of u16, each LEN/2 entries:
    #        plane0 = low nibble of even bytes   plane1 = high nibble of even
    #        plane2 = low nibble of odd bytes    plane3 = high nibble of odd
    # Planar rather than interleaved on purpose: interleaving 4 vectors into
    # pixel order needs per-lane stores (ssv), which would dominate and
    # measure the shuffle instead of the extraction.  See the C side, which
    # applies the identical layout so the comparison is like-for-like.
    .global expand_entry
expand_entry:
    lw      s0, D_CTRL+0(zero)
    lw      t3, D_CTRL+8(zero)
    li      s4, D_SRCBUF
    addi    t0, t3, -1
    jal     DMAIn
     li     t1, 0

    # constants
    lqv     $v10, 0,D_VCONST+0x00, zero      # 0x00FF
    lqv     $v11, 0,D_VCONST+0x10, zero      # 0x000F
    lqv     $v12, 0,D_VCONST+0x20, zero      # 0x0100  (>>8)
    lqv     $v13, 0,D_VCONST+0x30, zero      # 0x1000  (>>4)
    lw      t4, D_CTRL+12(zero)
    sh      t4, D_VFLAG+0(zero)
    sh      t4, D_VFLAG+2(zero)
    sh      t4, D_VFLAG+4(zero)
    sh      t4, D_VFLAG+6(zero)
    sh      t4, D_VFLAG+8(zero)
    sh      t4, D_VFLAG+10(zero)
    sh      t4, D_VFLAG+12(zero)
    sh      t4, D_VFLAG+14(zero)
    lqv     $v14, 0,D_VFLAG, zero

    lw      s7, D_CTRL+20(zero)         # repeat count (op is at +16)
3:                                      # outer repeat loop
    lw      t3, D_CTRL+8(zero)
    li      t5, D_SRCBUF             # input cursor
    li      t6, D_DSTBUF             # plane0 cursor
    # Plane stride in BYTES is LEN, not LEN/2: each pair of input bytes
    # yields one entry per plane, so a plane holds LEN/2 entries of u16,
    # i.e. LEN bytes.  Using LEN/2 made the four planes overlap.
    add     t8, t6, t3                  # plane1
    add     t9, t8, t3                  # plane2
    add     s5, t9, t3                  # plane3
    add     s6, t5, t3                  # input end

2:  lqv     $v01, 0,0x000, t5           # 16 bytes = 8 lanes of (hi:lo)
    vmudl   $v02, $v01, $v12            # v02 = v01 >> 8      (even/high byte)
    vand    $v03, $v01, $v10            # v03 = v01 & 0x00FF  (odd/low byte)
    vand    $v04, $v02, $v11            # even byte, low nibble
    vmudl   $v05, $v02, $v13            # even byte >> 4
    vand    $v05, $v05, $v11            # even byte, high nibble
    vand    $v06, $v03, $v11            # odd byte, low nibble
    vmudl   $v07, $v03, $v13            # odd byte >> 4
    vand    $v07, $v07, $v11            # odd byte, high nibble
    vor     $v04, $v04, $v14
    vor     $v05, $v05, $v14
    vor     $v06, $v06, $v14
    vor     $v07, $v07, $v14
    sqv     $v04, 0,0x000, t6
    sqv     $v05, 0,0x000, t8
    sqv     $v06, 0,0x000, t9
    sqv     $v07, 0,0x000, s5
    addi    t5, t5, 16
    addi    t6, t6, 16
    addi    t8, t8, 16
    addi    t9, t9, 16
    addi    s5, s5, 16
    bne     t5, s6, 2b
     nop
    addi    s7, s7, -1
    bgtz    s7, 3b
     nop

    lw      s0, D_CTRL+4(zero)
    lw      t3, D_CTRL+8(zero)
    sll     t3, t3, 2                   # 2 px per input byte, u16 each = 4*LEN
    li      s4, D_DSTBUF
    addi    t0, t3, -1
    jal     DMAOut
     li     t1, 0
    break


    # ---- op=2: extract + scalar palette lookup --------------------------
    # Prices the step that does NOT vectorise.  The RSP has no per-lane
    # gather, so a 16-entry palette costs ~4 ops/px whether done scalar or
    # as a veq+vmrg tree -- against roughly one cached load on the CPU.  If
    # this comes out much worse than op=1, palette lookup belongs on the CPU
    # and the RSP should emit indices, not colours.
    .global palette_entry
palette_entry:
    lw      s0, D_CTRL+0(zero)
    lw      t3, D_CTRL+8(zero)
    li      s4, D_SRCBUF
    addi    t0, t3, -1
    jal     DMAIn
     li     t1, 0

    lw      s7, D_CTRL+20(zero)         # repeat count
5:
    lw      t3, D_CTRL+8(zero)
    li      t5, D_SRCBUF
    li      t6, D_DSTBUF
    add     s6, t5, t3
6:  lbu     t7, 0(t5)                   # one packed byte = 2 pixels
    andi    t4, t7, 0x0F                # low nibble
    sll     t4, t4, 1
    addu    t4, t4, zero
    lhu     t4, D_VPAL(t4)              # scalar palette load
    sh      t4, 0(t6)
    srl     t4, t7, 4                   # high nibble
    andi    t4, t4, 0x0F
    sll     t4, t4, 1
    lhu     t4, D_VPAL(t4)
    sh      t4, 2(t6)
    addi    t5, t5, 1
    addi    t6, t6, 4
    bne     t5, s6, 6b
     nop
    addi    s7, s7, -1
    bgtz    s7, 5b
     nop

    lw      s0, D_CTRL+4(zero)
    lw      t3, D_CTRL+8(zero)
    sll     t3, t3, 2
    li      s4, D_DSTBUF
    addi    t0, t3, -1
    jal     DMAOut
     li     t1, 0
    break

#include <rsp_dma.inc>
