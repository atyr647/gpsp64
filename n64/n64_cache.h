/* gameplaySP - N64 Cache Coherency Utilities
 *
 * The VR4300 has separate I-cache (16 KB) and D-cache (8 KB).
 * After writing JIT code to the translation cache, we must:
 *   1. Write back D-cache lines containing the new code
 *   2. Invalidate I-cache lines so the CPU fetches the new code
 *
 * N64 port Copyright (C) 2026
 */

#ifndef N64_CACHE_H
#define N64_CACHE_H

#include <libdragon.h>

/* Flush the data cache and invalidate the instruction cache
 * for the given address range. Must be called after writing
 * JIT code to the translation cache. */
static inline void n64_flush_cache(void *addr, unsigned int len)
{
  /* Write back dirty D-cache lines */
  data_cache_hit_writeback(addr, len);
  /* Invalidate I-cache so CPU sees new code */
  inst_cache_hit_invalidate(addr, len);
}

#endif
