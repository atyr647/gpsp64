/* gameplaySP - N64 Cache Coherency Utilities
 *
 * The VR4300 has separate I-cache (16 KB, 32-byte lines) and
 * D-cache (8 KB, 16-byte lines).
 *
 * After writing JIT code to the translation cache, we must:
 *   1. Write back ALL dirty D-cache lines
 *   2. Invalidate ALL I-cache lines
 *
 * Using full index-based flush because ares reports that hit-based
 * writeback doesn't properly reach RDRAM.
 */

#ifndef N64_CACHE_H
#define N64_CACHE_H

#include <libdragon.h>

/* Full D-cache writeback + I-cache invalidate.
   Brute-force: iterate ALL cache lines by index. */
static inline void n64_flush_cache(void *addr, unsigned int len)
{
  (void)addr; (void)len;

  /* Writeback and invalidate ALL D-cache lines (8KB, 16-byte lines = 512 lines) */
  for (unsigned i = 0; i < 8192; i += 16)
    asm volatile("cache 0x01, 0(%0)" :: "r"(0x80000000 + i));

  /* Invalidate ALL I-cache lines (16KB, 32-byte lines = 512 lines) */
  for (unsigned i = 0; i < 16384; i += 32)
    asm volatile("cache 0x00, 0(%0)" :: "r"(0x80000000 + i));
}

#endif
