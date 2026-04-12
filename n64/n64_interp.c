/* gpSP N64 — C support for MIPS assembly Thumb interpreter
 *
 * Provides:
 * - thumb_asm_handle_opcode: C fallback for opcodes not yet in assembly
 * - thumb_handler_table: 256-entry handler table for dispatch
 */

#include "../common.h"

/* External symbols from mips_interp.S */
extern void thumb_asm_c_fallback(void);

/* Handler table: all entries initially point to C fallback.
 * As assembly handlers are added, their entries are replaced. */
void *thumb_handler_table[256];

void init_thumb_handler_table(void)
{
    int i;
    for (i = 0; i < 256; i++)
        thumb_handler_table[i] = (void*)thumb_asm_c_fallback;

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
void thumb_asm_handle_opcode(u32 opcode)
{
    (void)opcode;
    /* Run the C interpreter for exactly 1 cycle. It will:
     * - Fetch the opcode from reg[REG_PC] (already set by asm dispatch)
     * - Execute the instruction
     * - Update reg[REG_PC], flags, memory state
     * - Return after 1 instruction due to cycle budget
     */
    execute_arm(1);
}
