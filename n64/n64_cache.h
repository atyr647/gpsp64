#ifndef N64_CACHE_H
#define N64_CACHE_H

#ifdef N64
/* Use libdragon's tested cache management functions.
 * These are known to work correctly in ares and on real hardware. */
#include <n64sys.h>

static inline void n64_flush_cache(void *addr, unsigned int len)
{
  /* Writeback dirty D-cache lines to RDRAM */
  data_cache_hit_writeback_invalidate(addr, len);
  /* Invalidate I-cache so it fetches fresh code from RDRAM */
  inst_cache_hit_invalidate(addr, len);
}
#else
static inline void n64_flush_cache(void *addr, unsigned int len) {
  (void)addr; (void)len;
}
#endif

#endif
