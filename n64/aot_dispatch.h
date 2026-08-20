/* Lookup tables that map a GBA PC to its AOT-translated function.
 *
 * Shared between the generated code in n64/aot_generated.c (which
 * defines the tables) and cpu.cc (which inlines the lookup into the
 * Thumb interpreter loop).  Layout and rationale: see emit_dispatch()
 * in tools/thumb2c.py.
 */
#ifndef N64_AOT_DISPATCH_H
#define N64_AOT_DISPATCH_H

/* A translated function takes its entry PC (several entry points can
 * share one function body) and returns the GBA cycles it consumed. */
typedef u32 (*aot_fn_t)(u32 pc);

struct aot_page_ent {
  const aot_fn_t *fns;   /* distinct translated functions on this page */
  const u8       *slots; /* [(pc & 0xFFF) >> 1] -> 1-based fns index, 0 = none.
                          * NULL when the page holds no AOT code. */
};

/* Indexed by (pc >> 12) & 0x1FFF — the mask alone bounds it, so the hot
 * path needs no compare.  Entries for pages without AOT code are zero. */
extern const struct aot_page_ent aot_page_tab[8192];

#endif
