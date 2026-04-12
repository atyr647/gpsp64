/* gpSP N64 — MIPS Assembly Thumb Inner Loop
 *
 * Replaces ONLY the inner do-while loop of the C Thumb interpreter.
 * Does NOT call update_gba — the C caller handles events.
 *
 * Returns when: cycles expire or mode change to ARM.
 * Return value: remaining cycles (in $v0)
 *
 * Register allocation (persistent across dispatch iterations):
 *   $s0 ($16): reg_base — pointer to reg[] array
 *   $s1 ($17): cycles_remaining
 *   $s2 ($18): pc_address_block — base of current 32KB page
 *   $s3 ($19): GBA PC
 *   $s4-$s7 ($20-$23): N/Z/C/V flags
 *   $fp ($30): handler table base
 */

#include "../gpsp_config.h"

.set noreorder
.set noat
.set mips3

#define reg_base    $16
#define reg_cycles  $17
#define reg_pcblk   $18
#define reg_pc      $19
#define reg_n       $20
#define reg_z       $21
#define reg_c       $22
#define reg_v       $23
#define reg_tbl     $30

.equ REG_PC,     15*4
.equ REG_CPSR,   16*4
.equ GP_SAVE,    30*4

.text
.align 5

/*=========================================================================
 * execute_thumb_inner(u32 cycles, u32 *reg_ptr, void **handler_table)
 *
 * Runs Thumb instructions until cycles expire. Returns remaining cycles.
 * Caller handles update_gba and event processing.
 *========================================================================*/
.globl execute_thumb_inner
.type execute_thumb_inner, %function
execute_thumb_inner:
    addiu $sp, $sp, -96
    sd    $s0,  0($sp)
    sd    $s1,  8($sp)
    sd    $s2, 16($sp)
    sd    $s3, 24($sp)
    sd    $s4, 32($sp)
    sd    $s5, 40($sp)
    sd    $s6, 48($sp)
    sd    $s7, 56($sp)
    sd    $fp, 64($sp)
    sd    $ra, 72($sp)
    sd    $gp, 80($sp)

    move  reg_base, $a1
    sll   reg_base, reg_base, 0
    move  reg_cycles, $a0
    sll   reg_cycles, reg_cycles, 0
    move  reg_tbl, $a2
    sll   reg_tbl, reg_tbl, 0

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

thumb_inner_reload_page:
    srl   $t0, reg_pc, 15
    sll   $t0, $t0, 2
    .set at
    la    $t1, memory_map_read
    .set noat
    addu  $t0, $t0, $t1
    lw    reg_pcblk, 0($t0)
    bnez  reg_pcblk, thumb_inner_dispatch
    nop
    /* Page is NULL — load from ROM */
    srl   $a0, reg_pc, 15
    andi  $a0, $a0, 0x3FF
    ld    $gp, 80($sp)
    jal   load_gamepak_page
    nop
    move  reg_pcblk, $v0

    b     thumb_inner_dispatch
    nop

.align 5
thumb_inner_dispatch:
    andi  $t0, reg_pc, 0x7FFE
    addu  $t0, $t0, reg_pcblk
    lhu   $t1, 0($t0)

    srl   $t2, $t1, 8
    sll   $t1, $t1, 8
    andi  $t1, $t1, 0xFF00
    or    $a0, $t1, $t2

    andi  $t0, $t2, 0xFF
    sll   $t0, $t0, 2
    addu  $t0, $t0, reg_tbl
    lw    $t0, 0($t0)
    sll   $t0, $t0, 0
    jr    $t0
    nop

.align 5
.globl thumb_inner_next
thumb_inner_next:
    addiu reg_cycles, reg_cycles, -1
    bgtz  reg_cycles, thumb_inner_dispatch
    nop

thumb_inner_exit:
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

    move  $v0, reg_cycles

    ld    $gp, 80($sp)
    ld    $ra, 72($sp)
    ld    $fp, 64($sp)
    ld    $s7, 56($sp)
    ld    $s6, 48($sp)
    ld    $s5, 40($sp)
    ld    $s4, 32($sp)
    ld    $s3, 24($sp)
    ld    $s2, 16($sp)
    ld    $s1,  8($sp)
    ld    $s0,  0($sp)
    jr    $ra
    addiu $sp, $sp, 96

/* C fallback for unimplemented handlers */
.align 5
.globl thumb_inner_c_fallback
thumb_inner_c_fallback:
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

    ld    $gp, 80($sp)
    jal   thumb_c_execute_one
    nop

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

    andi  $t0, $t0, 0x20
    beqz  $t0, thumb_inner_exit
    nop

    b     thumb_inner_reload_page
    nop
