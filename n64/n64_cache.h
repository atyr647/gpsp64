#ifndef N64_CACHE_H
#define N64_CACHE_H

/* Flush JIT code: writeback D-cache and invalidate I-cache.
 * D-cache line size on VR4300: 16 bytes
 * I-cache line size on VR4300: 32 bytes */
static inline void n64_flush_cache(void *addr, unsigned int len)
{
  unsigned char *p;
  unsigned char *end = (unsigned char*)addr + len;

  /* Write back dirty D-cache lines to RDRAM */
  for (p = (unsigned char*)((unsigned long)addr & ~15UL); p < end; p += 16)
    __asm__ volatile("cache 0x15, 0(%0)" :: "r"(p));

  /* Invalidate I-cache lines so CPU fetches new code from RDRAM */
  for (p = (unsigned char*)((unsigned long)addr & ~31UL); p < end; p += 32)
    __asm__ volatile("cache 0x10, 0(%0)" :: "r"(p));
}

#endif
