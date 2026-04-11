/* gameplaySP - N64 Cache Coherency for VR4300 (MIPS III)
 *
 * The VR4300 has separate I-cache (16KB, 32B lines) and D-cache (8KB, 16B lines).
 * It does NOT have synci (MIPS32R2) so __builtin___clear_cache won't work.
 * Must use explicit cache instructions:
 *   cache 0x15 = D-cache Hit Writeback Invalidate
 *   cache 0x10 = I-cache Hit Invalidate
 */

#ifndef N64_CACHE_H
#define N64_CACHE_H

static inline void n64_flush_cache(void *addr, unsigned int len)
{
  /* Align start down to 16-byte boundary (D-cache line size) */
  unsigned long start = (unsigned long)addr & ~15UL;
  unsigned long end = (unsigned long)addr + len;

  /* Write back all dirty D-cache lines in range */
  for (unsigned long a = start; a < end; a += 16)
    __asm__ volatile("cache 0x15, 0(%0)" :: "r"(a));

  /* Invalidate all I-cache lines in range (32-byte line size) */
  start = (unsigned long)addr & ~31UL;
  for (unsigned long a = start; a < end; a += 32)
    __asm__ volatile("cache 0x10, 0(%0)" :: "r"(a));
}

#endif
