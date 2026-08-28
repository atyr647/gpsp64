/* gameplaySP - N64: read ares's own hardware counters from inside the ROM
 *
 * Every performance number in this port is a wall-clock frame time plus a
 * guess about what caused it, and the guesses have been wrong often
 * enough to be worth retiring.  -O2 beats -O3 *and* -Os and nobody knows
 * why.  execute_arm costs ~200 VR4300 cycles per interpreted instruction
 * where an interpreter should manage 20-40.  Deleting a quarter of the
 * interpreted instructions changed nothing.  Two builds differing only in
 * profiling counters measured 66 ms and 53 ms.  Every one of those is a
 * memory-behaviour question, and none of them can be answered by timing
 * the whole frame harder.
 *
 * ares models the VR4300's caches properly -- a 16 KB direct-mapped
 * I-cache and 8 KB D-cache, 48 cycles to fill a line, counted in both the
 * interpreter and the recompiler -- and exposes the counters through its
 * "emux" extension instructions.  A Makefile note says those counters
 * "report nothing because it recompiles and never runs the accounting
 * path"; that is not true of this build.  recompiler-ipu.cpp emits calls
 * to CPU::XPROF and CPU::XPROFREAD exactly as the interpreter does.
 *
 * So: read the global counters before and after a region, and the
 * difference is that region's cache behaviour, measured by the same model
 * that produces the frame time.
 *
 * Requires ares run with --setting Developer/HomebrewMode=true.  Without
 * it the instructions are decoded and do nothing, so the counters read
 * zero rather than misbehaving.
 *
 * Encoding: these live in COP0 space with the CO bit set, function 0x28
 * (XPROF) and 0x29 (XPROFREAD).  The register fields are NOT in the usual
 * MIPS positions -- ares reads rd from bits 24:20 and rt from bits 19:15
 * (see XRdn/XRtn in ares/n64/cpu/interpreter.cpp and recompiler-fpu.cpp)
 * -- so the words are assembled by hand against fixed registers.
 *
 * N64 port Copyright (C) 2026
 */

#ifndef N64_EMUX_PROF_H
#define N64_EMUX_PROF_H

/* Counter selectors, from CPU::XPROFREAD. */
#define EMUX_CPU_CYCLES      0x0000
#define EMUX_CPU_CYCLES_EXC  0x0001
#define EMUX_ICACHE_HITS     0x0010
#define EMUX_ICACHE_MISSES   0x0011
#define EMUX_ICACHE_WB       0x0012
#define EMUX_DCACHE_HITS     0x0020
#define EMUX_DCACHE_MISSES   0x0021
#define EMUX_DCACHE_WB       0x0022
#define EMUX_RSP_CYCLES      0x0200
#define EMUX_RDRAM_TOTAL     0x0300
#define EMUX_RDRAM_READS     0x0301
#define EMUX_RDRAM_WRITES    0x0302
#define EMUX_RDRAM_ICACHE    0x0310
#define EMUX_RDRAM_DCACHE    0x0320
#define EMUX_RDRAM_UNCACHED  0x0330
#define EMUX_RDRAM_SP_DMA    0x0340

/* Slot -1 reads the global (always-accumulating) counters, so a region
 * can be measured by differencing without any start/stop bookkeeping. */
#define EMUX_SLOT_GLOBAL (-1)

#if defined(N64) && defined(N64_EMUX_PROF)

/* rd = $t0 (r8), rt = $t1 (r9). */
#define EMUX_XPROFREAD_WORD (0x42000029u | (8u << 20) | (9u << 15))

static inline unsigned long emux_read(int slot, unsigned code)
{
  register unsigned long _s __asm__("$8") = (unsigned long)slot;
  register unsigned long _v __asm__("$9") = (unsigned long)code;
  __asm__ volatile(".word %[w]"
                   : "+r"(_v)
                   : "r"(_s), [w] "i"(EMUX_XPROFREAD_WORD)
                   : "memory");
  return _v;
}

#define EMUX_GLOBAL(code) emux_read(EMUX_SLOT_GLOBAL, (code))

#else
static inline unsigned long emux_read(int slot, unsigned code)
{ (void)slot; (void)code; return 0; }
#define EMUX_GLOBAL(code) 0UL
#endif

#endif /* N64_EMUX_PROF_H */
