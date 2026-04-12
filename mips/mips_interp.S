/* gpSP N64 — MIPS Assembly Thumb Interpreter
 *
 * Hand-tuned dispatch loop for VR4300. Persistent register allocation,
 * centralized dispatch, inline handlers for hot opcodes.
 *
 * Register allocation:
 *   $s0 ($16): reg_base — pointer to reg[] array
 *   $s1 ($17): cycles_remaining
 *   $s2 ($18): pc_address_block — base of current 32KB page
 *   $s3 ($19): GBA PC (mirrored in reg[REG_PC])
 *   $s4 ($20): n_flag (0 or 1)
 *   $s5 ($21): z_flag (0 or 1)
 *   $s6 ($22): c_flag (0 or 1)
 *   $s7 ($23): v_flag (0 or 1)
 *   $fp ($30): thumb handler table base
 *   $gp ($28): saved C GP (for C function calls)
 *
 * Handler convention:
 *   Entry: $a0 = opcode, $s0-$s7/$fp set, reg[] accessible via $s0
 *   Exit: goto thumb_asm_next (cycle check + re-dispatch)
 *   For C fallback: goto thumb_asm_c_fallback
 */

#include "../gpsp_config.h"

.set noreorder
.set noat
.set mips3

/* Register aliases */
#define reg_base    $16   /* s0 */
#define reg_cycles  $17   /* s1 */
#define reg_pcblk   $18   /* s2 */
#define reg_pc      $19   /* s3 */
#define reg_n       $20   /* s4 */
#define reg_z       $21   /* s5 */
#define reg_c       $22   /* s6 */
#define reg_v       $23   /* s7 */
#define reg_tbl     $30   /* fp */

/* reg[] array offsets */
.equ REG_PC,     15*4
.equ REG_CPSR,   16*4
.equ CPU_HALT,   18*4
.equ GP_SAVE,    30*4

/* Memory map is at reg_base - 0x8000 */

.text
.align 5   /* 32-byte I-cache line alignment */

/*=========================================================================
 * execute_thumb_fast: entry point from C code
 * Args: $a0 = cycles, $a1 = pointer to reg[], $a2 = handler table
 *========================================================================*/
.globl execute_thumb_fast
.type execute_thumb_fast, %function
execute_thumb_fast:
    /* Save callee-saved registers (64-bit for MIPS III) */
    addiu $sp, $sp, -80
    sd    $s0,  0*8($sp)
    sd    $s1,  1*8($sp)
    sd    $s2,  2*8($sp)
    sd    $s3,  3*8($sp)
    sd    $s4,  4*8($sp)
    sd    $s5,  5*8($sp)
    sd    $s6,  6*8($sp)
    sd    $s7,  7*8($sp)
    sd    $fp,  8*8($sp)
    sd    $ra,  9*8($sp)

    /* Set up persistent registers */
    move  reg_base, $a1               /* reg[] pointer */
    sll   reg_base, reg_base, 0       /* sign-extend MIPS III */
    move  reg_cycles, $a0             /* cycle budget */
    sll   reg_cycles, reg_cycles, 0
    move  reg_tbl, $a2                /* handler table */
    sll   reg_tbl, reg_tbl, 0

    /* Save C GP for later restore during C function calls */
    sw    $gp, GP_SAVE(reg_base)

    /* Load PC from reg[] */
    lw    reg_pc, REG_PC(reg_base)

    /* Extract flags from CPSR */
    lw    $t0, REG_CPSR(reg_base)
    srl   reg_n, $t0, 31
    andi  reg_n, reg_n, 1
    srl   reg_z, $t0, 30
    andi  reg_z, reg_z, 1
    srl   reg_c, $t0, 29
    andi  reg_c, reg_c, 1
    srl   reg_v, $t0, 28
    andi  reg_v, reg_v, 1

    /* Load pc_address_block from memory_map_read[pc >> 15] */
    srl   $t0, reg_pc, 15
    sll   $t0, $t0, 2
    .set at
    la    $t1, memory_map_read
    .set noat
    addu  $t0, $t0, $t1
    lw    reg_pcblk, 0($t0)

    /* Enter dispatch loop */
    b     thumb_asm_dispatch
    nop

/*=========================================================================
 * Thumb dispatch: fetch opcode, decode, jump to handler
 * Called from: handler epilogues, entry point
 *========================================================================*/
.align 5
thumb_asm_dispatch:
    /* Fetch 16-bit Thumb opcode */
    andi  $t0, reg_pc, 0x7FFE        /* offset within 32KB page */
    addu  $t0, $t0, reg_pcblk        /* pointer into ROM/RAM */
    lhu   $t1, 0($t0)                /* load opcode (big-endian) */

    /* Byte-swap for LE ROM storage */
    srl   $t2, $t1, 8
    sll   $t1, $t1, 8
    andi  $t1, $t1, 0xFF00
    or    $a0, $t1, $t2              /* a0 = swapped opcode */

    /* Dispatch: handler_table[(opcode >> 8) & 0xFF] */
    andi  $t0, $t2, 0xFF             /* t2 already has opcode >> 8 */
    sll   $t0, $t0, 2                /* word offset */
    addu  $t0, $t0, reg_tbl          /* table + index*4 */
    lw    $t0, 0($t0)                /* load handler address */
    sll   $t0, $t0, 0                /* sign-extend for MIPS III */
    jr    $t0                         /* jump to handler */
    nop

/*=========================================================================
 * thumb_asm_next: cycle check after each instruction, re-dispatch
 *========================================================================*/
.align 5
thumb_asm_next:
    /* Subtract sequential cycle cost (simplified: 1 cycle per insn) */
    addiu reg_cycles, reg_cycles, -1

    /* Check if cycles expired */
    bgtz  reg_cycles, thumb_asm_dispatch
    nop

    /* Cycles expired — save state and call update_gba */
thumb_asm_expired:
    sw    reg_pc, REG_PC(reg_base)

    /* Collapse flags into CPSR */
    lw    $t0, REG_CPSR(reg_base)
    andi  $t0, $t0, 0xFF             /* keep mode bits */
    sll   $t1, reg_n, 31
    or    $t0, $t0, $t1
    sll   $t1, reg_z, 30
    or    $t0, $t0, $t1
    sll   $t1, reg_c, 29
    or    $t0, $t0, $t1
    sll   $t1, reg_v, 28
    or    $t0, $t0, $t1
    sw    $t0, REG_CPSR(reg_base)

    /* Call update_gba(cycles_remaining) */
    move  $a0, reg_cycles
    lw    $gp, GP_SAVE(reg_base)      /* restore C GP */
    jal   update_gba
    nop

    /* Check return value: bit 31 = frame complete */
    bltz  $v0, thumb_asm_frame_done
    nop

    /* Update cycle count */
    andi  reg_cycles, $v0, 0x7FFF

    /* Extract flags from CPSR (may have changed during update_gba) */
    lw    $t0, REG_CPSR(reg_base)
    srl   reg_n, $t0, 31
    andi  reg_n, reg_n, 1
    srl   reg_z, $t0, 30
    andi  reg_z, reg_z, 1
    srl   reg_c, $t0, 29
    andi  reg_c, reg_c, 1
    srl   reg_v, $t0, 28
    andi  reg_v, reg_v, 1

    /* Reload PC (may have changed) */
    lw    reg_pc, REG_PC(reg_base)

    /* Check if mode changed to ARM */
    andi  $t0, $t0, 0x20
    beqz  $t0, thumb_asm_mode_change  /* bit 5 clear = ARM mode, exit */
    nop

    /* Reload pc_address_block for new PC region */
    srl   $t0, reg_pc, 15
    sll   $t0, $t0, 2
    .set at
    la    $t1, memory_map_read         /* use actual symbol address */
    .set noat
    addu  $t0, $t0, $t1
    lw    reg_pcblk, 0($t0)

    /* Continue dispatch */
    b     thumb_asm_dispatch
    nop

/*=========================================================================
 * Exit paths
 *========================================================================*/
thumb_asm_frame_done:
    /* Frame complete: $v0 already has update_gba return (bit 31 set) */
    /* Save final state */
    sw    reg_pc, REG_PC(reg_base)
    lw    $t0, REG_CPSR(reg_base)
    andi  $t0, $t0, 0xFF
    sll   $t1, reg_n, 31
    or    $t0, $t0, $t1
    sll   $t1, reg_z, 30
    or    $t0, $t0, $t1
    sll   $t1, reg_c, 29
    or    $t0, $t0, $t1
    sll   $t1, reg_v, 28
    or    $t0, $t0, $t1
    sw    $t0, REG_CPSR(reg_base)
    b     thumb_asm_return
    nop

thumb_asm_mode_change:
    /* Switched to ARM mode: return with cycles remaining (no bit 31) */
    sw    reg_pc, REG_PC(reg_base)
    lw    $t0, REG_CPSR(reg_base)
    andi  $t0, $t0, 0xFF
    sll   $t1, reg_n, 31
    or    $t0, $t0, $t1
    sll   $t1, reg_z, 30
    or    $t0, $t0, $t1
    sll   $t1, reg_c, 29
    or    $t0, $t0, $t1
    sll   $t1, reg_v, 28
    or    $t0, $t0, $t1
    sw    $t0, REG_CPSR(reg_base)
    move  $v0, reg_cycles            /* return cycles remaining */
    b     thumb_asm_return
    nop

thumb_asm_return:
    /* Restore callee-saved registers */
    ld    $s0,  0*8($sp)
    ld    $s1,  1*8($sp)
    ld    $s2,  2*8($sp)
    ld    $s3,  3*8($sp)
    ld    $s4,  4*8($sp)
    ld    $s5,  5*8($sp)
    ld    $s6,  6*8($sp)
    ld    $s7,  7*8($sp)
    ld    $fp,  8*8($sp)
    ld    $ra,  9*8($sp)
    jr    $ra
    addiu $sp, $sp, 80

/*=========================================================================
 * C fallback: save state, call C handler, restore state
 * Used for complex/uncommon opcodes not yet implemented in asm
 *========================================================================*/
.align 5
.globl thumb_asm_c_fallback
thumb_asm_c_fallback:
    /* Save PC and flags to reg[] */
    sw    reg_pc, REG_PC(reg_base)
    lw    $t0, REG_CPSR(reg_base)
    andi  $t0, $t0, 0xFF
    sll   $t1, reg_n, 31
    or    $t0, $t0, $t1
    sll   $t1, reg_z, 30
    or    $t0, $t0, $t1
    sll   $t1, reg_c, 29
    or    $t0, $t0, $t1
    sll   $t1, reg_v, 28
    or    $t0, $t0, $t1
    sw    $t0, REG_CPSR(reg_base)

    /* Call thumb_asm_handle_opcode(opcode) — C function that executes
       one Thumb instruction using the existing interpreter macros */
    lw    $gp, GP_SAVE(reg_base)
    jal   thumb_asm_handle_opcode
    nop

    /* Restore state from reg[] */
    lw    reg_pc, REG_PC(reg_base)
    lw    $t0, REG_CPSR(reg_base)
    srl   reg_n, $t0, 31
    andi  reg_n, reg_n, 1
    srl   reg_z, $t0, 30
    andi  reg_z, reg_z, 1
    srl   reg_c, $t0, 29
    andi  reg_c, reg_c, 1
    srl   reg_v, $t0, 28
    andi  reg_v, reg_v, 1

    /* Check if we switched to ARM mode */
    andi  $t0, $t0, 0x20
    beqz  $t0, thumb_asm_return
    nop

    /* Reload page pointer */
    srl   $t0, reg_pc, 15
    sll   $t0, $t0, 2
    subu  $t1, reg_base, 0x8000
    addu  $t0, $t0, $t1
    lw    reg_pcblk, 0($t0)

    /* Subtract cycles for this instruction (approximate) */
    addiu reg_cycles, reg_cycles, -1

    b     thumb_asm_next
    nop

/*=========================================================================
 * TODO: Inline assembly handlers for hot Thumb opcodes
 *
 * Phase 1 targets (most common):
 *   0x20-0x27: MOV Rd, #imm8
 *   0x28-0x2F: CMP Rd, #imm8
 *   0x30-0x37: ADD Rd, #imm8
 *   0x38-0x3F: SUB Rd, #imm8
 *   0x00-0x07: LSL Rd, Rs, #imm5
 *   0xD0-0xDD: B<cond> label
 *   0xE0-0xE7: B label
 *
 * Each handler: ~8-12 instructions including dispatch return.
 * Estimated savings: ~30 cycles/insn → ~60 cyc/i target
 *========================================================================*/

