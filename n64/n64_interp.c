/* gpSP N64 — C support for MIPS assembly Thumb interpreter
 *
 * Provides:
 * - thumb_asm_handle_opcode: C fallback for opcodes not yet in assembly
 * - thumb_handler_table: 256-entry handler table for dispatch
 */

#include "../common.h"

/* External symbols from mips_interp.S */
extern void thumb_inner_c_fallback(void);

/* Handler table: all entries initially point to C fallback.
 * As assembly handlers are added, their entries are replaced. */
void *thumb_handler_table[256];

void init_thumb_handler_table(void)
{
    int i;
    for (i = 0; i < 256; i++)
        thumb_handler_table[i] = (void*)thumb_inner_c_fallback;

    /* TODO: Replace entries with assembly handler addresses as they're
     * implemented. Example:
     * extern void thumb_asm_mov_imm(void);
     * for (i = 0x20; i <= 0x27; i++)
     *     thumb_handler_table[i] = (void*)thumb_asm_mov_imm;
     */
}

/* C fallback handler: execute one Thumb instruction using the existing
 * interpreter infrastructure. Called from mips_interp.S when an opcode
 * doesn't have an assembly handler yet.
 *
 * The assembly dispatch has already saved PC and flags to reg[]/CPSR.
 * We just need to execute the instruction and return.
 */
/* Execute one Thumb instruction using the existing C interpreter.
 * This is the slow path — used only for opcodes without assembly handlers.
 * The assembly dispatch saves PC/flags to reg[]/CPSR before calling this. */
/* Execute one Thumb instruction. PC and CPSR are already in reg[].
 * This is the slow fallback path — only for opcodes without asm handlers.
 * Must NOT call update_gba or enter the interpreter's main loop. */
void thumb_asm_handle_opcode(u32 opcode)
{
    (void)opcode;

    /* Execute exactly one instruction by running the interpreter
     * with a large cycle budget but breaking after one instruction.
     * We use a trick: set a special flag that tells the interpreter
     * to return after one instruction. Since we can't easily do that
     * with the current interpreter structure, we'll use the
     * clear_gamepak_stickybits + execute_arm approach but with
     * the halt state set to force an immediate return path.
     *
     * TODO: For now, just advance PC as a NOP. This is WRONG but
     * lets us test the dispatch framework. Replace with real
     * instruction execution once inline asm handlers cover hot paths.
     */
    reg[REG_PC] += 2;
}
