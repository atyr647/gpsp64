#ifndef N64_CACHE_H
#define N64_CACHE_H

/* Flush JIT code from D-cache to RDRAM and invalidate I-cache.
 * __builtin___clear_cache handles both D-cache writeback and
 * I-cache invalidation on MIPS targets. */
static inline void n64_flush_cache(void *addr, unsigned int len)
{
  __builtin___clear_cache((char*)addr, (char*)addr + len);
}

#endif
