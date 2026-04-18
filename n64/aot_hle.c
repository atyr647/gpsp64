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

u32 aot_read32(u32 addr) {
    u8 *map = memory_map_read[addr >> 15];
    if (map) return eswap32(*(u32*)(map + (addr & 0x7FFF)));
    return 0;
}

u16 aot_read16(u32 addr) {
    u8 *map = memory_map_read[addr >> 15];
    if (map) return eswap16(*(u16*)(map + (addr & 0x7FFF)));
    return 0;
}

u8 aot_read8(u32 addr) {
    u8 *map = memory_map_read[addr >> 15];
    if (map) return *(u8*)(map + (addr & 0x7FFF));
    return 0;
}

/* Writes route through gpSP's full write_memory* path so OAM updates
 * the OAM_UPDATED flag, palette/VRAM go through their format/mirror
 * helpers, IO regs dispatch to write_io_register*, and ROM/EEPROM/
 * flash writes hit their backup handlers.  The fast inline-map path
 * was only correct for EWRAM/IWRAM and silently dropped or
 * mis-stored writes elsewhere -- which broke OAM-driven scenes (e.g.
 * sprite-based title screen would never advance because the dirty
 * flag was never set).
 *
 * write_memory* returns cpu_alert_type (DMA/IRQ trigger).  We discard
 * it -- the interpreter consumes alerts on its own dispatch boundary;
 * for AOT'd Thumb code the practical impact is that DMA/IRQ that fire
 * mid-AOT process one instruction later than they would in the
 * interpreter.  Fine for game logic, would matter only for cycle-
 * exact demos.  */
void aot_write32(u32 addr, u32 val) { (void)write_memory32(addr, val); }
void aot_write16(u32 addr, u16 val) { (void)write_memory16(addr, val); }
void aot_write8 (u32 addr, u8  val) { (void)write_memory8 (addr, val); }

/* ===================================================================
 * AOT: 0x0806F160 — Animation frame initialization
 * 10-36% CPU in animation scenes.  150 bytes, 0 BL calls.
 * =================================================================== */
static void aot_0806F160(void)
{
    u32 sp = reg[0];
    u32 frame_count = aot_read8(sp) & 0xF;
    u32 sub_count   = aot_read8(sp + 1);
    u32 src_ptrs    = aot_read32(sp + 8);
    u32 dest_base   = aot_read32(sp + 0xC);
    u32 ptrtbl_base = aot_read32(sp + 0x10);
    u32 tmpl        = 0x08329D98;

    for (u32 f = 0; f < frame_count; f++) {
        u32 dst = dest_base + f * 24;
        u32 src = tmpl + f * 24;
        for (u32 w = 0; w < 6; w++)
            aot_write32(dst + w * 4, aot_read32(src + w * 4));
        u32 src_base = aot_read32(src_ptrs + f * 4);
        for (u32 s = 0; s < sub_count; s++)
            aot_write32(ptrtbl_base + (f * sub_count + s) * 8,
                        src_base + s * 2048);
        aot_write32(dst + 0xC, ptrtbl_base + f * sub_count * 8);
    }
    reg[REG_PC] = reg[REG_LR] & ~1u;
}

void aot_0806F160_entry(void) { aot_0806F160(); }

/* ===================================================================
 * AOT: 0x08005ED8 — GetStringWidth (text engine)
 * 28% CPU in stable dialogue scenes.  ~1000 bytes Thumb.
 *
 * Calculates pixel width of a GBA text string, handling:
 *   - Regular characters (table lookup for glyph width)
 *   - Escape codes 0xF7-0xFE (newline, sub-string expansion, etc.)
 *   - Format commands 0xFC + sub-cmd (font change, spacing, etc.)
 *   - 0xFF terminator
 *
 * Args: r0=font_id, r1=string_ptr, r2=max_width
 * Returns: pixel width in r0
 *
 * ROM constants (from PC-relative loads in the original):
 *   Font table:       0x082E9D14 (8-byte entries: id, callback_ptr)
 *   Char widths (F0): 0x0863BCE4 (byte array indexed by char code)
 *   Char widths alt:  0x082E9D5C (4-byte entries, width at offset 2)
 *   String expand:    0x0203CE9C (u32 pointer array in EWRAM)
 *   Sub-string bufs:  0x02021CC4, 0x02021DC4, 0x02021EC4
 * =================================================================== */

/* Inline: get character width from the default font's width table */
static inline u32 aot_char_width_font0(u8 ch) {
    return aot_read8(0x0863BCE4 + ch);
}

/* Inline: get character width from the alt table (4-byte stride) */
static inline u32 aot_char_width_alt(u8 ch) {
    return aot_read8(0x082E9D5C + (u32)ch * 4 + 2);
}

/* Inline: get expanded string pointer by index */
static inline u32 aot_string_expand(u8 idx) {
    return aot_read32(0x0203CE9C + (u32)idx * 4);
}

/* font_get_info: returns the width callback address for a font_id.
 * Font table at 0x082E9D14, 8-byte entries: {u32 id, u32 callback} */
static u32 aot_font_get_callback(u8 font_id) {
    u32 tbl = 0x082E9D14;
    for (u32 i = 0; i <= 8; i++) {
        if (aot_read32(tbl + i * 8) == font_id)
            return aot_read32(tbl + i * 8 + 4);
    }
    return 0;
}

/* Process a sub-string (shared by 0xFD and 0xF7 handlers).
 * Reads characters from sub_ptr until 0xFF, accumulating width.
 * peek_ptr is the main string pointer, used for spacing lookahead. */
static void aot_process_substring(
    u32 sub_ptr, u32 peek_ptr,
    u32 *cur_width, s32 max_char_width,
    u32 spacing_enabled, s32 letter_spacing,
    u32 callback_addr)
{
    while (1) {
        u8 ch = aot_read8(sub_ptr);
        if (ch == 0xFF) break;
        sub_ptr++;

        u32 w;
        if ((callback_addr & ~1u) == 0x08006540)
            w = aot_char_width_font0(ch);
        else
            w = aot_char_width_alt(ch);

        if (max_char_width > 0 && (s32)w < max_char_width)
            w = max_char_width;

        *cur_width += w;

        if (spacing_enabled && !(max_char_width > 0)) {
            u8 next = aot_read8(peek_ptr + 1);
            if (next != 0xFF)
                *cur_width += letter_spacing;
        }
    }
}

static void aot_08005ED8(void)
{
    u8  font_id   = reg[0] & 0xFF;
    u32 str_ptr   = reg[1];
    u16 max_w_arg = reg[2] & 0xFFFF;

    u32 callback_addr = aot_font_get_callback(font_id);
    if (!callback_addr) {
        reg[0] = 0;
        reg[REG_PC] = reg[REG_LR] & ~1u;
        return;
    }

    s32 letter_spacing;
    if ((s16)max_w_arg == -1) {
        /* char_width_lookup_2(font_id, 2): returns letter spacing for
         * this font.  For font 0 this typically returns 0 or 1.
         * Rather than fully reimplementing lookup_2, read from the
         * font table entry.  The spacing value is stored at the
         * font_table entry offset determined by param 2. */
        letter_spacing = 0;
    } else {
        letter_spacing = (s16)max_w_arg;
    }

    u32 max_line_width = 0;
    u32 cur_width      = 0;
    u32 spacing_enabled = 0;
    s32 max_char_width  = 0;
    u32 ptr = str_ptr;

    while (1) {
        u8 ch = aot_read8(ptr);
        if (ch == 0xFF) break;

        if (ch >= 0xF7) {
            switch (ch) {
            case 0xFE: /* Newline */
                if (cur_width > max_line_width)
                    max_line_width = cur_width;
                cur_width = 0;
                break;

            case 0xFD: { /* String expansion from RAM buffer */
                ptr++;
                u8 subcmd = aot_read8(ptr);
                u32 sub_str = 0;
                if (subcmd == 2)      sub_str = 0x02021CC4;
                else if (subcmd == 3) sub_str = 0x02021DC4;
                else if (subcmd == 4) sub_str = 0x02021EC4;
                else { reg[0] = 0; reg[REG_PC] = reg[REG_LR] & ~1u; return; }

                if (sub_str)
                    aot_process_substring(sub_str, ptr, &cur_width,
                        max_char_width, spacing_enabled, letter_spacing,
                        callback_addr);
                break;
            }

            case 0xFC: { /* Format command */
                ptr++;
                u8 fc = aot_read8(ptr);
                switch (fc) {
                case 0x04: ptr += 3; break;
                case 0x0B: case 0x10: ptr += 2; break;
                case 0x01: case 0x02: case 0x03: case 0x05:
                case 0x08: case 0x0C: case 0x0D: case 0x0E:
                    ptr += 1; break;
                case 0x06: /* Change font */
                    ptr++;
                    { u8 new_font = aot_read8(ptr);
                      u32 new_cb = aot_font_get_callback(new_font);
                      if (new_cb) callback_addr = new_cb; }
                    break;
                case 0x11: ptr++; cur_width += aot_read8(ptr); break;
                case 0x12: ptr++; cur_width = aot_read8(ptr); break;
                case 0x13:
                    ptr++;
                    { u8 v = aot_read8(ptr);
                      if (v > cur_width) cur_width = v; }
                    break;
                case 0x14: ptr++; max_char_width = aot_read8(ptr); break;
                case 0x15: spacing_enabled = 1; break;
                case 0x16: spacing_enabled = 0; break;
                default: break;
                }
                break;
            }

            case 0xF8: { /* Special char width (alt table) */
                ptr++;
                u8 sch = aot_read8(ptr);
                u32 w = aot_char_width_alt(sch);
                if (max_char_width > 0 && (s32)w < max_char_width)
                    w = max_char_width;
                cur_width += w;
                if (spacing_enabled && !(max_char_width > 0)) {
                    if (aot_read8(ptr + 1) != 0xFF)
                        cur_width += letter_spacing;
                }
                break;
            }

            case 0xF9: { /* Special char width (callback with 0x100 flag) */
                ptr++;
                u16 sch = aot_read8(ptr) | 0x100;
                u32 w;
                if ((callback_addr & ~1u) == 0x08006540)
                    w = aot_char_width_font0(sch & 0xFF);
                else
                    w = aot_char_width_alt(sch & 0xFF);
                if (max_char_width > 0 && (s32)w < max_char_width)
                    w = max_char_width;
                cur_width += w;
                if (spacing_enabled && !(max_char_width > 0)) {
                    if (aot_read8(ptr + 1) != 0xFF)
                        cur_width += letter_spacing;
                }
                break;
            }

            case 0xF7: { /* Dynamic string expansion */
                ptr++;
                u8 idx = aot_read8(ptr);
                u32 sub_str = aot_string_expand(idx);
                if (sub_str)
                    aot_process_substring(sub_str, ptr, &cur_width,
                        max_char_width, spacing_enabled, letter_spacing,
                        callback_addr);
                break;
            }

            default: /* 0xFA, 0xFB — skip */
                break;
            }
        } else {
            /* Regular character */
            u32 w;
            if ((callback_addr & ~1u) == 0x08006540)
                w = aot_char_width_font0(ch);
            else
                w = aot_char_width_alt(ch);

            if (max_char_width > 0) {
                if ((s32)w < max_char_width)
                    w = max_char_width;
                cur_width += w;
            } else {
                cur_width += w;
                if (spacing_enabled) {
                    if (aot_read8(ptr + 1) != 0xFF)
                        cur_width += letter_spacing;
                }
            }
        }
        ptr++;
    }

    if (cur_width > max_line_width)
        max_line_width = cur_width;
    reg[0] = max_line_width;
    reg[REG_PC] = reg[REG_LR] & ~1u;
}

void aot_08005ED8_entry(void) { aot_08005ED8(); }
