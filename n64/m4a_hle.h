/* gameplaySP - N64: native substitution inside the m4a sound mixer.
 * See m4a_hle.c for what this is and why. */
#ifndef N64_M4A_HLE_H
#define N64_M4A_HLE_H

#include "../common.h"

#if defined(N64_M4A_STUB) || defined(N64_M4A_NATIVE)

void m4a_hle_frame(void);
u32  m4a_entry_pc(void);
extern u32 m4a_hle_entry_pc;

#define M4A_HLE_FRAME() m4a_hle_frame()

#else
#define M4A_HLE_FRAME() do {} while (0)
#endif

#ifdef N64_M4A_NATIVE
/* SWI numbers used to mark the replaced loops.  The GBA BIOS defines
 * 0x00-0x2B, so the 0xF0 block cannot collide with a real call. */
#define M4A_SWI_REVERB 0xF0
#define M4A_SWI_RESAMP 0xF1
#define M4A_SWI_UNITY  0xF2
#define M4A_SWI_UNITYC 0xF3
int m4a_hle_arm_swi(u32 swi_num, u32 *cycles);
#endif

#endif
