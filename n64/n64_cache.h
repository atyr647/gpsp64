#ifndef N64_CACHE_H
#define N64_CACHE_H

/* Invalidate I-cache for JIT code range.
 * With KSEG1 writes, D-cache writeback is unnecessary — data is already
 * in RDRAM. We only need to invalidate I-cache so the CPU fetches the
 * new code from RDRAM instead of using stale I-cache lines.
 * I-cache line size on VR4300: 32 bytes. */
static inline void n64_flush_cache(void *addr, unsigned int len)
{
  unsigned char *p;
  unsigned char *end = (unsigned char*)addr + len;
  /* Invalidate I-cache lines covering the range */
  for (p = (unsigned char*)((unsigned long)addr & ~31UL); p < end; p += 32)
    __asm__ volatile("cache 0x10, 0(%0)" :: "r"(p));  /* I-cache Hit Invalidate */
}

#endif
