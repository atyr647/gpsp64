/* gpSP N64 — Ahead-of-Time High-Level Emulation (AOT HLE)
 *
 * Replaces hot GBA ARM/Thumb functions with native C equivalents,
 * compiled by GCC at build time.  Avoids interpreter dispatch
 * overhead (~150-175 cyc/insn) for the replaced functions.
 *
 * Approach proven by mvs64 NeoGeo emulator (+13% avg, +30% peak):
 *   1. Profile to identify hot function entry PCs
 *   2. Disassemble and understand the GBA function
 *   3. Write equivalent C that operates on reg[] and GBA memory
 *   4. At runtime: if PC matches, run native version, set PC=LR
 *
 * Unlike dynarec: static compilation (by GCC), no runtime codegen,
 * no cache coherency issues, no big-endian bugs.
 */

#include "../common.h"
#include "../cpu.h"
#include "../gba_memory.h"

/* Fast memory access via memory_map (same backing arrays as the
 * interpreter).  These go through the page table for safety;
 * returns 0 on unmapped pages rather than crashing. */

static inline u32 aot_read32(u32 addr) {
    u8 *map = memory_map_read[addr >> 15];
    if (map) return eswap32(*(u32*)(map + (addr & 0x7FFF)));
    return 0;
}

static inline u16 aot_read16(u32 addr) {
    u8 *map = memory_map_read[addr >> 15];
    if (map) return eswap16(*(u16*)(map + (addr & 0x7FFF)));
    return 0;
}

static inline u8 aot_read8(u32 addr) {
    u8 *map = memory_map_read[addr >> 15];
    if (map) return *(u8*)(map + (addr & 0x7FFF));
    return 0;
}

static inline void aot_write32(u32 addr, u32 val) {
    u8 *map = memory_map_read[addr >> 15];
    if (map) *(u32*)(map + (addr & 0x7FFF)) = eswap32(val);
}

/* ===================================================================
 * AOT: 0x0806F160 — Animation frame initialization
 *
 * Original: ~150 bytes Thumb, 2 nested loops, 0 BL calls.
 * Profiled at 10-36% of CPU in animation/transition scenes.
 *
 * Disassembly analysis (0x0806F160 - 0x0806F1F6):
 *
 * struct anim {
 *   u8 flags;           // +0x00: lower nibble = frame_count
 *   u8 sub_count;       // +0x01: sub-frames per frame
 *   u8 pad[6];          // +0x02..0x07
 *   u32 *src_ptrs;      // +0x08: array of source base addresses
 *   u8 *dest;           // +0x0C: destination (24-byte stride entries)
 *   u8 *ptr_table;      // +0x10: pointer table (8-byte stride)
 * };
 *
 * The function:
 *   for frame in 0..frame_count:
 *     memcpy(dest[frame*24], template[frame*24], 24)  // from ROM
 *     for sub in 0..sub_count:
 *       ptr_table[(frame*sub_count+sub)*8] = src_ptrs[frame] + sub*2048
 *     dest[frame*24 + 0xC] = &ptr_table[frame*sub_count*8]
 *
 * Template ROM address: 0x08329D98 (PC-relative constant at 0x0806F1F8)
 * =================================================================== */
static void aot_0806F160(void)
{
    u32 sp = reg[0];  /* struct pointer passed in r0 */

    u32 frame_count = aot_read8(sp) & 0xF;
    u32 sub_count   = aot_read8(sp + 1);
    u32 src_ptrs    = aot_read32(sp + 8);
    u32 dest_base   = aot_read32(sp + 0xC);
    u32 ptrtbl_base = aot_read32(sp + 0x10);
    u32 template    = 0x08329D98;

    for (u32 f = 0; f < frame_count; f++) {
        u32 dst = dest_base + f * 24;
        u32 src = template  + f * 24;

        /* Copy 24 bytes (6 words) from ROM template to dest entry */
        for (u32 w = 0; w < 6; w++)
            aot_write32(dst + w * 4, aot_read32(src + w * 4));

        /* Fill pointer table entries for this frame's sub-frames */
        u32 src_base = aot_read32(src_ptrs + f * 4);
        for (u32 s = 0; s < sub_count; s++) {
            u32 tbl_addr = ptrtbl_base + (f * sub_count + s) * 8;
            aot_write32(tbl_addr, src_base + s * 2048);
        }

        /* Link dest entry's field_0C to this frame's ptr_table slice */
        aot_write32(dst + 0xC, ptrtbl_base + f * sub_count * 8);
    }

    /* Return: PC = LR (Thumb bit stripped) */
    reg[REG_PC] = reg[REG_LR] & ~1u;
}

/* Entry point wrappers — called directly from inline checks in cpu.cc.
 * Each wraps an AOT function so it can be called without exposing the
 * static implementation to the caller. */
void aot_0806F160_entry(void)
{
    aot_0806F160();
}
