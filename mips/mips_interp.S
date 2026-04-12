/* gpSP N64 — MIPS Assembly Interpreter for ARM7TDMI (Thumb mode)
 *
 * Modeled after sodium64's approach: persistent register allocation,
 * function table dispatch, delay slot exploitation.
 *
 * Register allocation:
 *   $s0 ($16): reg_base — pointer to reg[] array
 *   $s1 ($17): cycles_remaining
 *   $s2 ($18): pc_address_block — base of current 32KB page
 *   $s3 ($19): GBA PC (kept in sync with reg[REG_PC])
 *   $s4 ($20): n_flag (0 or 1)
 *   $s5 ($21): z_flag (0 or 1)
 *   $s6 ($22): c_flag (0 or 1)
 *   $s7 ($23): v_flag (0 or 1)
 *   $fp ($30): handler table base (thumb_handler_table)
 *
 * Temporaries (caller-saved, used by handlers):
 *   $v0 ($2):  return value / temp
 *   $v1 ($3):  temp
 *   $a0 ($4):  temp / function arg 0
 *   $a1 ($5):  temp / function arg 1
 *   $a2 ($6):  temp / function arg 2
 *   $a3 ($7):  temp / function arg 3
 *   $t0-$t7 ($8-$15): temporaries for instruction handlers
 *   $t8 ($24): temp
 *   $t9 ($25): temp
 *   $at ($1):  assembler temp
 *
 * Convention: each handler ends with the Thumb dispatch sequence:
 *   thumb_dispatch:
 *     andi $t0, $s3, 0x7FFE      # offset within 32KB page
 *     addu $t0, $t0, $s2         # pointer to instruction
 *     lhu  $s3+1, 0($t0)         # load 16-bit opcode (big-endian)
 *     ... decode and dispatch ...
 */

#include "../gpsp_config.h"

.set noreorder
.set noat

/* Register aliases matching our allocation */
#define reg_base   $16
#define reg_cycles $17
#define reg_pcblk  $18
#define reg_pc     $19
#define reg_n      $20
#define reg_z      $21
#define reg_c      $22
#define reg_v      $23
#define reg_tbl    $30

/* reg[] offsets */
.equ REG_R0,    0*4
.equ REG_R1,    1*4
.equ REG_R2,    2*4
.equ REG_R3,    3*4
.equ REG_R4,    4*4
.equ REG_R5,    5*4
.equ REG_R6,    6*4
.equ REG_R7,    7*4
.equ REG_R8,    8*4
.equ REG_R9,    9*4
.equ REG_R10,   10*4
.equ REG_R11,   11*4
.equ REG_R12,   12*4
.equ REG_SP,    13*4
.equ REG_LR,    14*4
.equ REG_PC,    15*4
.equ REG_CPSR,  16*4
.equ CPU_MODE,  17*4
.equ CPU_HALT,  18*4

/* TODO: This is the framework for the assembly interpreter.
 * Implementation will be added incrementally:
 *   Phase 1: Dispatch loop + handler table (call C handlers)
 *   Phase 2: Hot Thumb handlers in assembly (MOV, ADD, SUB, CMP, LDR, STR)
 *   Phase 3: ARM dispatch loop
 *   Phase 4: Hot ARM handlers in assembly
 */

.text
.align 5  /* 32-byte alignment for I-cache */

/* Thumb dispatch: fetch, decode, jump to handler
 * Called after each instruction handler completes.
 * Expects: reg_pc = current GBA PC (already advanced to next instruction)
 *          reg_pcblk = base of current 32KB page
 *          reg_cycles = remaining cycles (checked after handler)
 */
.globl thumb_dispatch
thumb_dispatch:
    /* Check cycle budget */
    blez reg_cycles, thumb_cycle_expired
    nop

    /* Fetch 16-bit Thumb opcode from pre-computed page pointer */
    andi $t0, reg_pc, 0x7FFE       /* offset within 32KB page */
    addu $t0, $t0, reg_pcblk       /* pointer into ROM/RAM */
    lhu  $t1, 0($t0)               /* load opcode (big-endian halfword) */

    /* Byte-swap for little-endian ROM storage */
    srl  $t2, $t1, 8               /* high byte */
    sll  $t1, $t1, 8               /* low byte << 8 */
    andi $t1, $t1, 0xFF00
    or   $t1, $t1, $t2             /* opcode = swapped halfword */

    /* Dispatch: handler_table[(opcode >> 8) & 0xFF] */
    srl  $t0, $t1, 8               /* handler index = opcode >> 8 */
    sll  $t0, $t0, 2               /* word offset */
    addu $t0, $t0, reg_tbl         /* handler_table + index*4 */
    lw   $t0, 0($t0)               /* load handler address */
    jr   $t0                        /* jump to handler */
    addiu reg_pc, reg_pc, 2         /* advance PC (delay slot) */

.align 5
thumb_cycle_expired:
    /* Save state back to reg[] and call update_gba */
    sw   reg_pc, REG_PC(reg_base)
    /* TODO: collapse flags, call update_gba, handle frame completion */
    /* For now, return to C interpreter */
    jr   $ra
    nop

