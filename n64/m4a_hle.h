/* gameplaySP - N64: native substitution for the m4a sound mixer.
 * See m4a_hle.c for what this is and why. */
#ifndef N64_M4A_HLE_H
#define N64_M4A_HLE_H

#include "../common.h"

#ifdef N64_M4A_STUB

/* gpSP keeps IWRAM in the upper half of iwram_raw; the lower half is the
 * dynarec's shadow copy.  Same convention as bios_hle.c. */
static inline u8 *m4a_iwram_base(void)
{
  extern u8 iwram_raw[];
  return iwram_raw + 0x8000;
}

void m4a_hle_frame(void);
extern u32 m4a_hle_entry_pc;

#define M4A_HLE_FRAME() m4a_hle_frame()

#else
#define M4A_HLE_FRAME() do {} while (0)
#endif

#endif
