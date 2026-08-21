/* RSP offload for GBA scanline work -- stage 1: infrastructure self-test.
 *
 * Rendering is 27.4 ms of a 55 ms frame and is 85% compute (only ~4.15 ms
 * of per-frame D-cache stall is attributable to it), so the RSP is wanted
 * as a second execution unit running alongside the VR4300.  Before writing
 * a renderer, prove the path: assemble, load, DMA in, compute, DMA out,
 * sync -- and verify the result on the CPU.
 */
#include <libdragon.h>
#include <rsp.h>
#include <string.h>
#include "../common.h"

DEFINE_RSP_UCODE(rsp_gbascan);

/* Mirrors the CTRL block at DMEM offset 0 in rsp_gbascan.S */
typedef struct {
  u32 src;
  u32 dst;
  u32 len;
  u32 orval;
} rsp_ctrl_t;

static bool rsp_ready = false;

void n64_rsp_init(void)
{
  rsp_init();
  rsp_load(&rsp_gbascan);
  rsp_ready = true;
}

/* Returns true if the RSP produced exactly what the CPU would have. */
bool n64_rsp_selftest(void)
{
  static u16 src[256] __attribute__((aligned(16)));
  static u16 dst[256] __attribute__((aligned(16)));
  /* Static and 16-byte aligned, not stack locals: these are handed to DMA,
     so they need cache-line-aligned writeback.  The first attempt used a
     stack local for ctrl and rsp_load_data transferred stale zeros --
     len came through as 0 and the ucode hung on a size -1 DMA. */
  static rsp_ctrl_t ctrl __attribute__((aligned(16)));
  static rsp_ctrl_t back __attribute__((aligned(16)));
  unsigned i;

  if (!rsp_ready) return false;

  for (i = 0; i < 256; i++) { src[i] = (u16)(i * 7 + 1); dst[i] = 0xDEAD; }

  /* The RSP DMAs from RDRAM, so the CPU's writes must be in RDRAM, not
     sitting dirty in the D-cache -- and its result must not be shadowed by
     stale cached lines on the way back. */
  data_cache_hit_writeback_invalidate(src, sizeof(src));
  data_cache_hit_writeback_invalidate(dst, sizeof(dst));

  ctrl.src   = (u32)src;
  ctrl.dst   = (u32)dst;
  ctrl.len   = sizeof(src);
  ctrl.orval = 0x0001;              /* same shape as the RGBA5551 alpha bit */

  /* Verify where the control block actually lands before trusting it.
     The first attempt assumed .data starts at DMEM offset 0 and the ucode
     hung with t0 = 0xFFFFFFFF -- i.e. it read CTRL_LEN as 0 and issued a
     DMA with size -1.  Read DMEM back and check. */
  data_cache_hit_writeback_invalidate(&ctrl, sizeof(ctrl));
  rsp_load_data(&ctrl, sizeof(ctrl), 0);
  {
    memset(&back, 0, sizeof(back));
    data_cache_hit_writeback_invalidate(&back, sizeof(back));
    rsp_read_data(&back, sizeof(back), 0);
    data_cache_hit_invalidate(&back, sizeof(back));
    debugf("[gpSP]: RSP ctrl wrote src=%08lx dst=%08lx len=%lu or=%lu\n",
           (unsigned long)ctrl.src, (unsigned long)ctrl.dst,
           (unsigned long)ctrl.len, (unsigned long)ctrl.orval);
    debugf("[gpSP]: RSP dmem[0] read src=%08lx dst=%08lx len=%lu or=%lu\n",
           (unsigned long)back.src, (unsigned long)back.dst,
           (unsigned long)back.len, (unsigned long)back.orval);
    if (back.len != ctrl.len) {
      debugf("[gpSP]: RSP ctrl block is NOT at DMEM 0 -- aborting selftest\n");
      return false;
    }
  }
  rsp_run();                        /* blocks until the ucode breaks */

  data_cache_hit_invalidate(dst, sizeof(dst));

  for (i = 0; i < 256; i++) {
    u16 want = (u16)((i * 7 + 1) | 0x0001);
    if (dst[i] != want) {
      debugf("[gpSP]: RSP selftest FAILED at %u: got %04x want %04x\n",
             i, dst[i], want);
      return false;
    }
  }
  debugf("[gpSP]: RSP selftest OK (ucode loaded, DMA in/out, %u bytes)\n",
         (unsigned)sizeof(src));
  return true;
}
