############################################################################
# rsp_gbascan.S -- GBA scanline work offloaded to the RSP.
#
# WHY THE RSP: rendering is 27.4 ms of a 55 ms frame and is 85% compute,
# not memory (measured: only 4.15 ms/frame of D-cache stall is attributable
# to rendering).  So the RSP is wanted as a *second execution unit* running
# concurrently with the VR4300, not as a cache-miss fix -- an earlier plan
# had that backwards.
#
# STAGE 1 (this file): infrastructure only.  DMA a buffer in, transform it,
# DMA it back.  The point is to prove the whole path works -- n64.mk
# assembling rsp*.S as ucode, rsp_load/rsp_run, DMA in both directions, and
# completion sync -- before any renderer is written against it.  Getting
# this wrong silently is easy, and debugging it underneath a half-written
# renderer would be miserable.
############################################################################

#include <rsp.inc>

    .data
    .align 3

# Control block, written from the CPU with rsp_load_data() before rsp_run().
    .global CTRL
CTRL:
CTRL_SRC:   .word 0         # RDRAM source address
CTRL_DST:   .word 0         # RDRAM destination address
CTRL_LEN:   .word 0         # byte count (multiple of 8, <= BUFSZ)
CTRL_OR:    .word 0         # value OR'd into every 16-bit lane (the test op)

    .align 3
#define BUFSZ 1024
BUF:        .space BUFSZ

    .text
    .align 3

    .global _start
_start:
    # --- DMA the source block RDRAM -> DMEM -------------------------------
    lw      s0, %lo(CTRL_SRC)(zero)
    lw      t3, %lo(CTRL_LEN)(zero)
    li      s4, %lo(BUF)
    addi    t0, t3, -1                  # DMA_SIZE(width,1) == width-1
    jal     DMAIn
     li     t1, 0

    # --- transform: OR a constant into each 16-bit lane -------------------
    # Scalar on purpose.  Stage 1 is about proving the path, not speed; the
    # vector unit comes in with the real renderer.
    lw      t4, %lo(CTRL_OR)(zero)
    li      t5, %lo(BUF)
    lw      t3, %lo(CTRL_LEN)(zero)
    add     t6, t5, t3                  # end pointer
1:
    lhu     t7, 0(t5)
    or      t7, t7, t4
    sh      t7, 0(t5)
    addi    t5, t5, 2
    bne     t5, t6, 1b
     nop

    # --- DMA the result DMEM -> RDRAM -------------------------------------
    lw      s0, %lo(CTRL_DST)(zero)
    lw      t3, %lo(CTRL_LEN)(zero)
    li      s4, %lo(BUF)
    addi    t0, t3, -1
    jal     DMAOut
     li     t1, 0

    break                               # halt; CPU's rsp_wait() returns

#include <rsp_dma.inc>
