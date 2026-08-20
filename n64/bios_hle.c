/* gpSP N64 — native (HLE) implementations of GBA BIOS software interrupts.
 *
 * Every SWI normally vectors to 0x00000008 and the BIOS handler is then
 * executed by the ARM interpreter.  Measured on ares with the PROF SWI
 * histogram, Pokemon Emerald issues exactly one SWI 0x0B (CpuSet) per
 * frame, and that single call accounts for ~17% of all interpreted
 * instructions -- roughly 1850 interpreted ARM instructions per call,
 * because the BIOS copies one unit at a time in an ARM loop.
 *
 * Replacing it with a native loop removes that work outright.  The
 * behaviour here is ported directly from the BIOS we actually ship
 * (bios/source/softwareinterrupts.c, swi_CpuSet) rather than from
 * documentation, so the semantics match what the interpreted path did:
 * the source-region rejection test, the 4-byte alignment forced in
 * 32-bit mode, and the open-bus values returned for reads past
 * 0x0EFFFFFF are all preserved.
 *
 * Build with -DBIOS_HLE_DISABLE to fall back to the interpreted BIOS.
 */

#include "../common.h"
#include "../gba_memory.h"
#include <string.h>

#define SWI_CPUSET 0x0B

/* Resolve a destination address to a directly-writable pointer, plus how
 * many bytes remain addressable there.  Only regions that are plain
 * linear memory qualify: palette needs format conversion and OAM needs a
 * dirty flag, so those fall back to the generic per-unit path.  Returns
 * NULL when no fast path applies. */
static u8 *dst_linear(u32 addr, u32 *avail)
{
  switch ((addr >> 24) & 0xF) {
  case 0x02: { u32 off = addr & 0x3FFFF;
               *avail = (1024 * 256) - off;  return ewram_raw + off; }
  case 0x03: { u32 off = (addr & 0x7FFF) + 0x8000;
               *avail = (1024 * 32 * 2) - off; return iwram_raw + off; }
  case 0x06: { u32 a = addr & 0x1FFFF; if (a >= 0x18000) a -= 0x8000;
               *avail = (1024 * 96) - a;     return vram_raw + a; }
  default: return NULL;
  }
}

/* Source side: memory_map_read gives a pointer valid to the end of its
 * 32 KB page, so copies are chunked to that granularity. */
static u8 *src_linear(u32 addr, u32 *avail)
{
  u8 *map = memory_map_read[addr >> 15];
  if (!map) return NULL;
  u32 off = addr & 0x7FFF;
  *avail = 0x8000 - off;
  return map + off;
}

static u32 hle_cpuset(void)
{
  u32 source = reg[0], dest = reg[1], cnt = reg[2];

  /* Reject sources the BIOS refuses to read (notably the BIOS region
   * itself); matches swi_CpuSet's first test exactly. */
  if (((source & 0xe000000) == 0) ||
      ((source + (((cnt << 11) >> 9) & 0x1fffff)) & 0xe000000) == 0)
    return 0;

  u32 count = cnt & 0x1FFFFF;

  if ((cnt >> 26) & 1) {              /* 32-bit units */
    source &= 0xFFFFFFFC;
    dest   &= 0xFFFFFFFC;
    if ((cnt >> 24) & 1) {            /* fill */
      u32 value = (source > 0x0EFFFFFF) ? 0x1CAD1CAD : read_memory32(source);
      while (count--) { write_memory32(dest, value); dest += 4; }
    } else {                          /* copy */
      /* Both sides store GBA memory in the same little-endian layout, so
       * a bulk byte copy is exactly equivalent to the per-word loop --
       * and avoids a pair of generic memory dispatches per word, which
       * is what made the naive version no faster than the BIOS itself. */
      while (count) {
        u32 savail = 0, davail = 0;
        u8 *sp = (source > 0x0EFFFFFF) ? NULL : src_linear(source, &savail);
        u8 *dp = dst_linear(dest, &davail);
        if (!sp || !dp) break;                 /* fall through to slow path */
        u32 bytes = count * 4;
        if (bytes > savail) bytes = savail & ~3u;
        if (bytes > davail) bytes = davail & ~3u;
        if (!bytes) break;
        memcpy(dp, sp, bytes);
        source += bytes; dest += bytes; count -= bytes / 4;
      }
      while (count--) {                        /* remainder / slow regions */
        u32 value = (source > 0x0EFFFFFF) ? 0x1CAD1CAD : read_memory32(source);
        write_memory32(dest, value);
        dest += 4; source += 4;
      }
    }
  } else {                            /* 16-bit units */
    source &= 0xFFFFFFFE;
    dest   &= 0xFFFFFFFE;
    if ((cnt >> 24) & 1) {            /* fill */
      u16 value = (source > 0x0EFFFFFF) ? 0x1CAD : (u16)read_memory16(source);
      while (count--) { write_memory16(dest, value); dest += 2; }
    } else {                          /* copy */
      while (count) {
        u32 savail = 0, davail = 0;
        u8 *sp = (source > 0x0EFFFFFF) ? NULL : src_linear(source, &savail);
        u8 *dp = dst_linear(dest, &davail);
        if (!sp || !dp) break;
        u32 bytes = count * 2;
        if (bytes > savail) bytes = savail & ~1u;
        if (bytes > davail) bytes = davail & ~1u;
        if (!bytes) break;
        memcpy(dp, sp, bytes);
        source += bytes; dest += bytes; count -= bytes / 2;
      }
      while (count--) {
        u16 value = (source > 0x0EFFFFFF) ? 0x1CAD : (u16)read_memory16(source);
        write_memory16(dest, value);
        dest += 2; source += 2;
      }
    }
  }

  /* Charge emulated time for the work, so replacing the BIOS loop does
   * not silently hand the game extra cycles per frame and shift its
   * timing.  The BIOS spends roughly three ARM instructions per unit
   * copied, which is what this approximates; it is deliberately a
   * model, not a cycle-exact figure. */
  return (cnt & 0x1FFFFF) * 3u + 60u;
}

/* Returns 1 if the SWI was handled natively (caller just advances PC and
 * stays in its current mode), 0 to vector to the interpreted BIOS. */
int bios_hle_swi(u32 swi_num, u32 *cycles)
{
#ifdef BIOS_HLE_DISABLE
  (void)swi_num; (void)cycles;
  return 0;
#else
  switch (swi_num) {
  case SWI_CPUSET:
    *cycles = hle_cpuset();
    return 1;
  default:
    return 0;
  }
#endif
}
