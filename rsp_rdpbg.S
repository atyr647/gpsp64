############################################################################
# rsp_rdpbg.S -- RDP TexRect generation offloaded to the RSP, as an rspq
# overlay coexisting with rdpq (which also runs as RSP ucode -- this is
# the coexistence mechanism the old rsp_gbascan.S could not use, since it
# loaded its own ucode with the raw rsp_init/rsp_load API, which conflicts
# with rdpq for the whole RSP).
#
# WHY: n64_rdpbg_flush's per-tile loop is ~2.44 ms/frame (~9% of the
# frame) and, per docs/DYNAREC.md, 95% of that is inside the loop body
# itself -- not the sort, not the TMEM upload, not the command-buffer
# writeback.  Static disassembly showed the compiler tail-duplicated the
# loop into ~12 copies, which is why instruction-level attribution on the
# CPU side kept disagreeing with itself.  Moving the computation to the
# RSP sidesteps that measurement problem: it does not matter why the loop
# is slow on the VR4300 if it runs somewhere else entirely, in parallel
# with CPU emulation.
#
# WHAT THIS DOES: takes a contiguous batch of rdpbg_draw_t records (the
# CPU has already sorted these into one (slice,palette) run -- see
# n64_rdp_bg.c) and reproduces the RDPBG_RECT macro exactly: per-tile
# clip against the 0..240 screen width, flip handling, and the
# 10.2/10.5/s5.10 fixed-point encoding of the RDP's TEXTURE_RECTANGLE
# (0xE4) command.  Tiles that clip away entirely (x0>=x1) are dropped,
# same as the CPU path.
#
# This is scalar RSP code, not vector: the per-tile work is a handful of
# branchy integer compares and a couple of shifts, not something that
# benefits from lane-parallel SIMD, and the RSP's scalar integer core
# runs it the same way the CPU's does -- the win is a second core doing
# it, not doing it wider.
#
# STATUS: correctness-verification stage, not live.  See n64/n64_rsp2.c.
############################################################################

#include <rsp_queue.inc>

    .set noat

    .data
    .align 4

    RSPQ_BeginOverlayHeader
        RSPQ_DefineCommand RDPBGCmd_Batch, 16   # command 0: 4 args (a0-a3)
        RSPQ_DefineCommand RDPBGCmd_Walk,  16   # command 1: 4 args (a0-a3)
    RSPQ_EndOverlayHeader

    RSPQ_EmptySavedState

    .bss
    .align 4
# Input: up to RDPBG_RSP_MAXBATCH records of 8 bytes each (s16 x, s16 y,
# u8 yy, u8 pf, u16 vt -- exactly rdpbg_draw_t's layout).  128 records =
# 1024 bytes in; worst case every record survives clipping, 16 bytes
# each = 2048 bytes out.  1024 + 2048 + 8 (META) = ~3.1 KB, comfortably
# inside the DMEM an overlay has left after RSPQ's own reserved area.
#define RDPBG_RSP_MAXBATCH 128
IN_BUF:
    .ds.b (RDPBG_RSP_MAXBATCH * 8)
OUT_BUF:
    .ds.b (RDPBG_RSP_MAXBATCH * 16)
    .align 3
META:
    .word 0                          # output byte count, DMA'd out always

# The walk command's buffers alias the batch command's.  rspq runs one
# command to completion before starting the next, so the two can never be
# live at once -- and they have to share, because the overlay's .bss
# already reaches 0xE34 of the 4KB DMEM and there are 460 bytes free.
#
#   NZBUF   384  vram_tile_nz, one bit per 32-byte tile
#   MAPBUF  128  one tilemap row: 32 entries, or 64 when the map is 512 wide
#   PARM     48  the parameter block
#   RECBUF  2048 staged output records, 256 of 8 bytes
#define NZBUF   (IN_BUF)
#define MAPBUF  (IN_BUF + 384)
#define PARM    (IN_BUF + 512)
#define RECBUF  (OUT_BUF)
#define RECMAX  256

    .text
    .align 3

# RDPBGCmd_Batch -- a0: src RDRAM addr (rdpbg_draw_t[count])
#                   a1: count (<= RDPBG_RSP_MAXBATCH)
#                   a2: dst RDRAM addr for output RDP words
#                   a3: RDRAM addr of a single u32 to receive the output
#                       byte count (may be 0 if every tile in the batch
#                       clipped away -- the destination buffer is then
#                       untouched, matching the CPU path's "continue"
#                       with no output for that tile)
    .func RDPBGCmd_Batch
RDPBGCmd_Batch:
    # DMAIn/DMAOut destroy t2 and $at only (rsp_dma.inc), but this ucode's
    # own convention (matching rsp_gbascan.S) is to stage call-spanning
    # values in s-registers regardless, rather than lean on an
    # undocumented clobber list.
    move    s5, a2                  # s5 = dst RDRAM addr (for the bulk DMA out)
    move    s6, a1                  # s6 = count, becomes the loop counter
    move    s7, a3                  # s7 = RDRAM addr for the output byte count

    sll     t3, a1, 3               # t3 = count * 8 = input byte length
    move    s0, a0                  # DMAIn: s0 = src RDRAM addr
    li      s4, %lo(IN_BUF)         # DMAIn: s4 = DMEM dest
    addi    t0, t3, -1
    jal     DMAIn
     li     t1, 0

    li      s2, %lo(IN_BUF)         # s2 = read cursor
    li      s3, %lo(OUT_BUF)        # s3 = write cursor

RDPBG_tile_loop:
    beqz    s6, RDPBG_tile_done
     nop
    addi    s6, s6, -1

    lh      t0, 0(s2)               # t0 = x  (s16)
    lh      t1, 2(s2)               # t1 = y  (s16)
    lbu     t2, 4(s2)               # t2 = yy
    lbu     t3, 5(s2)               # t3 = pf
    lhu     t4, 6(s2)               # t4 = vt
    addi    s2, s2, 8

    andi    t5, t2, 15              # t5 = y0f = yy & 15
    srl     t6, t2, 4               # t6 = y1f = yy >> 4
    srl     t7, t3, 4               # t7 = flip = pf >> 4

    move    t8, t0                  # t8 = x0 = x
    addi    t9, t0, 8                # t9 = x1 = x + 8
    add     t0, t1, t5               # t0 = y0 = y + y0f
    add     t1, t1, t6               # t1 = y1 = y + y1f

    andi    t2, t4, 31               # t2 = vt & 31
    sll     t2, t2, 3                # t2 = (vt&31)*8

    li      v0, 0                    # v0 = s0, unflipped
    add     v1, t2, t5               # v1 = t0v = (vt&31)*8 + y0f, unflipped
    li      a0, 0x0400                # a0 = dsdx16, unflipped (+1024)
    li      a1, 0x0400                # a1 = dtdy16, unflipped (+1024)

    andi    t3, t7, 1
    beqz    t3, RDPBG_noflipx
     nop
    li      v0, 7                    # flip&1: s0 = 7
    li      a0, 0xFC00                 # dsdx16 = -1024 (16-bit field)
RDPBG_noflipx:

    andi    t3, t7, 2
    beqz    t3, RDPBG_noflipy
     nop
    li      t3, 7
    subu    t3, t3, t5                # t3 = 7 - y0f
    add     v1, t2, t3                # v1 = (vt&31)*8 + (7 - y0f)
    li      a1, 0xFC00                 # dtdy16 = -1024
RDPBG_noflipy:

    # Clip x0 against 0.  x0/x1 are small (caller emits x from -7 to
    # 248), so plain integer arithmetic is exact -- no wraparound risk.
    bgez    t8, RDPBG_noclipleft
     nop
    # x0 < 0.  s0 -= x0 if dsdx>0, else s0 += x0.  Re-derive "flip&1" from
    # t7 (still valid; nothing between its computation and here touches
    # it) instead of comparing a0 against an immediate -- MIPS has no
    # register-vs-immediate branch instruction, so `bne reg,imm,label`
    # is a GAS pseudo-op that needs a scratch register to expand, and
    # this file runs under .set noat.  A real register-vs-zero branch
    # sidesteps the question entirely.
    andi    t3, t7, 1
    bnez    t3, RDPBG_clipleft_flipped
     nop
    subu    v0, v0, t8                # dsdx positive: s0 -= x0 (x0<0, so s0 grows)
    b       RDPBG_clipleft_done
     nop
RDPBG_clipleft_flipped:
    add     v0, v0, t8                # dsdx negative: s0 += x0 (x0<0, so s0 shrinks)
RDPBG_clipleft_done:
    li      t8, 0                     # x0 = 0
RDPBG_noclipleft:

    li      t3, 240
    slt     t4, t3, t9                # t4 = (240 < x1) ? 1 : 0
    beqz    t4, RDPBG_noclipright
     nop
    move    t9, t3                    # x1 = 240
RDPBG_noclipright:

    slt     t4, t8, t9                # t4 = (x0 < x1) ? 1 : 0
    bnez    t4, RDPBG_keep
     nop
    # x0 >= x1: rather than skip output entirely (which the CPU path does
    # via "continue"), force a zero-width rect and fall through to the
    # SAME emission code.  This makes every tile's output exactly 16
    # bytes, always -- the CPU driving this asynchronously needs to know
    # each batch's total output size to place the NEXT batch immediately
    # after it in rdpbg_cmds, without waiting for this one to finish and
    # report back how many tiles it actually kept.  A zero-width
    # TEXTURE_RECTANGLE draws zero columns: the same "nothing visible"
    # result as the CPU's continue, at the cost of one harmless RDP
    # command instead of none.
    move    t9, t8

RDPBG_keep:
    # word0 = 0xE4000000 | ((X1)<<14) | ((Y1)<<2), X1 = x1+40, Y1 = y1+40
    addi    t2, t9, 40                 # t2 = X1
    addi    t3, t1, 40                 # t3 = Y1
    sll     t2, t2, 14
    sll     t3, t3, 2
    or      t2, t2, t3
    lui     t4, 0xE400
    or      t2, t2, t4
    sw      t2, 0(s3)

    # word1 = (X0<<14) | (Y0<<2), X0 = x0+40, Y0 = y0+40
    addi    t2, t8, 40                 # t2 = X0
    addi    t3, t0, 40                 # t3 = Y0
    sll     t2, t2, 14
    sll     t3, t3, 2
    or      t2, t2, t3
    sw      t2, 4(s3)

    # word2 = (s0<<21) | ((t0v<<5) & 0xFFFF)
    sll     t2, v0, 21
    sll     t3, v1, 5
    andi    t3, t3, 0xFFFF
    or      t2, t2, t3
    sw      t2, 8(s3)

    # word3 = ((dsdx16&0xFFFF)<<16) | (dtdy16&0xFFFF)
    andi    t2, a0, 0xFFFF
    sll     t2, t2, 16
    andi    t3, a1, 0xFFFF
    or      t2, t2, t3
    sw      t2, 12(s3)

    addi    s3, s3, 16
    b       RDPBG_tile_loop
     nop

RDPBG_tile_done:
    # s3 - IN_BUF's counterpart: bytes actually written to OUT_BUF.
    li      t0, %lo(OUT_BUF)
    subu    s1, s3, t0               # s1 = output byte count

    # Always report the count back, even if zero.  For BOTH DMAIn and
    # DMAOut the convention is s0 = RDRAM address, s4 = DMEM address --
    # direction is selected only by the sign of t2, which DMAOut sets
    # internally (see rsp_dma.inc).
    sw      s1, %lo(META)

    move    s0, s7                    # RDRAM dest = the CPU's count pointer (a3)
    li      s4, %lo(META)             # DMEM src = the word we just stored
    li      t0, 3                     # 4 bytes - 1
    jal     DMAOut
     li     t1, 0

    beqz    s1, RDPBG_no_bulk_out
     nop
    move    s0, s5                    # RDRAM dest = the CPU's output pointer (a2)
    li      s4, %lo(OUT_BUF)          # DMEM src
    addi    t0, s1, -1
    jal     DMAOut
     li     t1, 0
RDPBG_no_bulk_out:

    j       RSPQ_Loop
     nop
    .endfunc

############################################################################
# RDPBGCmd_Walk -- the tilemap walk, moved off the VR4300.
#
# WHY: with the sort and the gather gone (see n64/n64_rdp_bg.c's bucketing)
# what is left of the renderer on the CPU is almost entirely this: read a
# tilemap entry, derive the absolute tile number, test one bit to see
# whether the tile is blank, and if not write an 8-byte draw record.  2,485
# times a frame, at about 15 instructions and a hundred-odd cycles each.
# It is the largest single item left in the frame and none of it needs the
# main CPU.
#
# WHAT IT DOES: walks one background layer's visible 21x31 tiles and writes
# the surviving draw records, in tilemap order, to RDRAM.  The CPU buckets
# them afterwards, which is the cheap half.  Records are exactly the
# rdpbg_draw_t the rest of the renderer already uses.
#
# WHAT IT DOES NOT DO: the per-tile-row scan for which screen rows the RDP
# owns.  When every row is owned -- which is what this workload gets, 160
# of 160 -- the run for a tile row collapses to clamping [0,8) against the
# screen, and that is all this implements.  The caller checks the row mask
# is uniform and keeps its own walk for when it is not; that path has to
# exist anyway for the frames the RDP declines.
#
# a0: parameter block in RDRAM (see PARM layout below)
# a1: RDRAM destination for the records
# a2: RDRAM destination for the u32 record count
# a3: unused
#
# PARM, all 32-bit:
#   0   map0     physical address of the layer's tilemap base
#   4   hofs     horizontal scroll
#   8   vofs     vertical scroll
#   12  mw       map width in pixels, 256 or 512
#   16  mh       map height in pixels, 256 or 512
#   20  cb512    char base already multiplied by 512
#   24  nz       physical address of vram_tile_nz
############################################################################
    .func RDPBGCmd_Walk
RDPBGCmd_Walk:
    move    s5, a1                   # s5 = RDRAM record destination (advances)
    move    s6, a2                   # s6 = RDRAM count destination

    # Parameters, then the blank-tile bitmap.  Both are read many times and
    # neither changes during the walk, so they come in once.
    move    s0, a0
    li      s4, %lo(PARM)
    jal     DMAIn
     li     t0, 28 - 1

    lw      k0, %lo(PARM) + 24       # k0 = nz bitmap address in RDRAM
    move    s0, k0
    li      s4, %lo(NZBUF)
    jal     DMAIn
     li     t0, 384 - 1

    lw      s1, %lo(PARM) + 0        # s1 = map0
    lw      s2, %lo(PARM) + 4        # s2 = hofs
    lw      s3, %lo(PARM) + 8        # s3 = vofs
    lw      s7, %lo(PARM) + 12       # s7 = mw
    lw      k1, %lo(PARM) + 16       # k1 = mh
    lw      gp, %lo(PARM) + 20       # gp = cb512

    # espan: entries across the map, 32 or 64.  ent0: the entry under
    # tx == 0, which is the scroll's tile column within the map.
    li      t8, 32                   # t8 = espan
    li      t9, 256
    bne     s7, t9, RDPBG_w_wide
     nop
    b       RDPBG_w_espan_done
     nop
RDPBG_w_wide:
    li      t8, 64
RDPBG_w_espan_done:
    addi    t0, s7, -1
    and     t1, s2, t0               # hofs & (mw-1)
    srl     fp, t1, 3                # fp = ent0

    andi    v1, s3, 7                # v1 = ysub
    andi    a0, s2, 7                # a0 = xsub

    li      s8, %lo(RECBUF)          # s8 = staging write cursor
    li      k0, 0                    # k0 = records staged
    li      ra, 0                    # ra = records written in total

    li      a1, 0                    # a1 = ty
RDPBG_w_row:
    li      t0, 21
    beq     a1, t0, RDPBG_w_done
     nop

    # sy = ty*8 - ysub, and the run of rows this tile contributes:
    # r0 = max(0, -sy), r1 = min(8, 160 - sy).  Skipped entirely when the
    # tile row lies off the screen.
    sll     a2, a1, 3
    subu    a2, a2, v1               # a2 = sy
    li      a3, 0
    bgez    a2, RDPBG_w_r0_done
     nop
    subu    a3, $0, a2               # sy < 0: start at -sy
RDPBG_w_r0_done:
    li      t1, 160
    subu    t1, t1, a2               # 160 - sy
    li      t2, 8
    slt     t3, t1, t2
    beqz    t3, RDPBG_w_r1_done
     nop
    move    t2, t1
RDPBG_w_r1_done:
    slt     t3, a3, t2               # r0 < r1 ?
    beqz    t3, RDPBG_w_next_row
     nop
    sll     t4, t2, 4
    or      t4, t4, a3               # t4 = yy = r0 | (r1 << 4)
    andi    t4, t4, 0xFF

    # The tilemap row.  vy selects it; a map taller than 256 pixels keeps
    # its lower half in the next 2KB screen block, and a map wider than 256
    # keeps each row's right half 2KB further on again, which is why a wide
    # map needs two transfers to make the row contiguous in DMEM.
    sll     t5, a1, 3
    li      t6, ~7
    and     t6, s3, t6
    addu    t5, t5, t6
    addi    t6, k1, -1
    and     t5, t5, t6               # t5 = vy
    li      t6, 256
    slt     t7, t5, t6
    li      t6, 0
    bnez    t7, RDPBG_w_rowblk_done
     nop
    li      t6, 2048                 # vy >= 256: next screen block
    li      t7, 512
    beq     s7, t7, RDPBG_w_rowblk_done
     nop
    li      t6, 2048
RDPBG_w_rowblk_done:
    li      t7, 512
    bne     s7, t7, RDPBG_w_rowblk_have
     nop
    li      t7, 256
    slt     t7, t5, t7
    bnez    t7, RDPBG_w_rowblk_have
     nop
    li      t6, 4096                 # wide map, lower half: two blocks on
RDPBG_w_rowblk_have:
    andi    t7, t5, 255
    srl     t7, t7, 3
    sll     t7, t7, 6                # (vy & 255) >> 3, times 64 bytes
    addu    t6, t6, t7
    addu    s0, s1, t6               # s0 = RDRAM address of the row
    li      s4, %lo(MAPBUF)
    jal     DMAIn
     li     t0, 64 - 1

    li      t7, 64
    bne     t8, t7, RDPBG_w_map_done
     nop
    addi    s0, s0, 2048             # wide map: the row's right half
    li      s4, %lo(MAPBUF) + 64
    jal     DMAIn
     li     t0, 64 - 1
RDPBG_w_map_done:

    move    t7, fp                   # t7 = entry index, walks with tx
    li      t0, 0                    # t0 = tx
RDPBG_w_tile:
    li      t1, 31
    beq     t0, t1, RDPBG_w_next_row
     nop

    sll     t1, t7, 1
    addi    t1, t1, %lo(MAPBUF)
    lhu     t2, 0(t1)                # tilemap entry, GBA byte order
    srl     t3, t2, 8
    andi    t2, t2, 0xFF
    sll     t2, t2, 8
    or      t2, t2, t3               # t2 = entry, byte-swapped

    andi    t3, t2, 0x3FF
    addu    t3, t3, gp
    andi    t3, t3, 0x7FF            # t3 = vt

    srl     t5, t3, 3
    addi    t5, t5, %lo(NZBUF)
    lbu     t5, 0(t5)
    andi    t6, t3, 7
    srlv    t5, t5, t6
    andi    t5, t5, 1
    beqz    t5, RDPBG_w_skip         # blank tile: draws nothing at all
     nop

    # x = tx*8 - xsub, y = sy, pf = palette | (flip << 4)
    sll     t6, t0, 3
    subu    t6, t6, a0
    sh      t6, 0(s8)
    sh      a2, 2(s8)
    sb      t4, 4(s8)
    srl     t6, t2, 12               # palette
    srl     t1, t2, 10
    andi    t1, t1, 3                # flip
    sll     t1, t1, 4
    or      t6, t6, t1
    sb      t6, 5(s8)
    sh      t3, 6(s8)
    addi    s8, s8, 8
    addi    k0, k0, 1

    # Staging is finite; hand it over when it fills and start again.
    li      t1, RECMAX
    bne     k0, t1, RDPBG_w_skip
     nop
    move    s0, s5
    li      s4, %lo(RECBUF)
    jal     DMAOut
     li     t0, (RECMAX * 8) - 1
    addi    s5, s5, RECMAX * 8
    addu    ra, ra, k0
    li      k0, 0
    li      s8, %lo(RECBUF)

RDPBG_w_skip:
    addi    t7, t7, 1
    bne     t7, t8, RDPBG_w_tile_next
     nop
    li      t7, 0                    # wrapped round the map
RDPBG_w_tile_next:
    b       RDPBG_w_tile
     addi   t0, t0, 1

RDPBG_w_next_row:
    b       RDPBG_w_row
     addi   a1, a1, 1

RDPBG_w_done:
    beqz    k0, RDPBG_w_no_tail
     nop
    move    s0, s5
    li      s4, %lo(RECBUF)
    sll     t0, k0, 3
    jal     DMAOut
     addi   t0, t0, -1
    addu    ra, ra, k0
RDPBG_w_no_tail:

    sw      ra, %lo(META)
    move    s0, s6
    li      s4, %lo(META)
    jal     DMAOut
     li     t0, 3

    j       RSPQ_Loop
     nop
    .endfunc
