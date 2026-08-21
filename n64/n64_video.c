/* gameplaySP - N64 Video Output (RDP-accelerated)
 *
 * Uses libdragon's rdpq to hardware-blit the GBA framebuffer.
 * The GBA core outputs XBGR1555 (with USE_XBGR1555_FORMAT).
 * We wrap it in a surface_t and use rdpq_tex_blit for the copy,
 * which offloads the pixel shuffle to the RDP hardware.
 *
 * Format conversion (XBGR1555 -> RGBA5551) processes two pixels
 * per iteration via 32-bit loads/stores.  A 65536-entry lookup
 * table was considered but ruled out: at 128 KB it would thrash
 * VR4300's 8 KB D-cache, costing more in misses than the inline
 * math costs in arithmetic.  The 32-bit pair-at-a-time approach
 * keeps everything in registers and halves the loop overhead.
 *
 * N64 port Copyright (C) 2026
 */

#include <libdragon.h>
#include <string.h>
#include <malloc.h>
#include "../common.h"
#include "n64_video.h"

extern u16 *gba_screen_pixels;

/* Pre-converted GBA framebuffer in RGBA5551 format.
 * Aligned to 8 bytes so 64-bit accesses (if the compiler chooses
 * them) don't trap. */
static u16 *rgba_buf = NULL;

/* Track whether we have already cleared each of the two display
 * buffers.  Borders (40px L+R, 40px T+B around the GBA area) never
 * get written by the per-frame blit; one-shot clear is enough.
 * libdragon's display_init creates 2 buffers; we count down. */
static int initial_clear_remaining = 2;

void n64_video_init(void)
{
  display_init(RESOLUTION_320x240, DEPTH_16_BPP, 2,
               GAMMA_NONE, FILTERS_RESAMPLE);
#ifndef N64_CPU_BLIT
  rdpq_init();
#endif
  /* memalign for 8-byte alignment so u32/u64 accesses are safe */
  rgba_buf = (u16 *)memalign(8,
              GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT * sizeof(u16));
  /* Report where the blit's big buffers actually live.  D-cache miss
     histograms attributed most render-time misses to two anonymous regions
     (64KB buckets 0x1c and 0x3c); these are heap allocations, so they have
     no symbol in the map and cannot be identified from the ELF alone. */
  debugf("[gpSP]: rgba_buf=%p (%d KB)\n", (void*)rgba_buf,
         (GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT * 2) / 1024);
  { extern u16 *gba_screen_pixels; 
    debugf("[gpSP]: gba_screen_pixels=%p\n", (void*)gba_screen_pixels); }
}

/* Per-pair format conversion: handles 2 pixels per 32-bit word.
 * Input  bits per half-word: B[14:10] G[9:5] R[4:0]
 * Output bits per half-word: R[15:11] G[10:6] B[5:1] A[0]=1
 * The masks operate on both halves of the u32 simultaneously,
 * exploiting MIPS's 32-bit ALU.  Safe under big-endian: each
 * 16-bit half stays in the same byte position. */
static inline u32 xbgr_pair_to_rgba_pair(u32 p) {
  return ((p << 11) & 0xF800F800u)   /* R bits to 11-15 */
       | ((p << 1)  & 0x07C007C0u)   /* G bits to 6-10  */
       | ((p >> 9)  & 0x003E003Eu)   /* B bits to 1-5   */
       | 0x00010001u;                /* A bit = 1       */
}

void n64_video_render_frame(void)
{
  surface_t *disp = display_get();
  { static int _once = 0;
    if (!_once && disp) { _once = 1;
      debugf("[gpSP]: framebuffer=%p stride=%d (%d KB each)\n",
             (void*)disp->buffer, (int)disp->stride,
             (int)(disp->stride * N64_SCREEN_HEIGHT / 1024)); } }
  if (!disp || !gba_screen_pixels || !rgba_buf) {
    if (disp) display_show(disp);
    return;
  }

  /* Convert XBGR1555 -> RGBA5551, two pixels per iteration.
   * 38400 total pixels = 19200 pairs. */
  const u32 *src = (const u32 *)gba_screen_pixels;
  u32       *dst = (u32 *)rgba_buf;
  const int pairs = (GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT) / 2;
  for (int i = 0; i < pairs; i++)
    dst[i] = xbgr_pair_to_rgba_pair(src[i]);

#ifdef N64_CPU_BLIT
  /* CPU-side fallback blit -- bypasses RDP entirely.  Used only for
   * testing under emulators with no RDP backend (e.g. ares without
   * Vulkan/paraLLEl-RDP, which silently no-ops rdpq_tex_blit).  Real
   * hardware always has RDP; this path is not used there. */
  if (initial_clear_remaining > 0) {
    memset(disp->buffer, 0, disp->stride * N64_SCREEN_HEIGHT);
    initial_clear_remaining--;
  }
  {
    u8 *dstrow = (u8 *)disp->buffer + GBA_OFFSET_Y * disp->stride
                 + GBA_OFFSET_X * 2;
    const u8 *srcrow = (const u8 *)rgba_buf;
    for (int y = 0; y < GBA_SCREEN_HEIGHT; y++) {
      memcpy(dstrow, srcrow, GBA_SCREEN_WIDTH * 2);
      dstrow += disp->stride;
      srcrow += GBA_SCREEN_WIDTH * 2;
    }
  }
  display_show(disp);
#else
  /* Wrap the converted buffer as a surface for rdpq */
  surface_t gba_surf = surface_make_linear(rgba_buf,
    FMT_RGBA16, GBA_SCREEN_WIDTH, GBA_SCREEN_HEIGHT);

  /* Use RDP to blit the GBA surface centered on the N64 display.
   * Borders are cleared to black ONCE per buffer at init -- the
   * per-frame fill_rectangle is unnecessary because nothing else
   * ever writes the border region. */
  rdpq_attach(disp, NULL);
  if (initial_clear_remaining > 0) {
    rdpq_set_mode_fill(RGBA16(0, 0, 0, 1));
    rdpq_fill_rectangle(0, 0, N64_SCREEN_WIDTH, N64_SCREEN_HEIGHT);
    initial_clear_remaining--;
  }
  rdpq_set_mode_copy(false);
  rdpq_tex_blit(&gba_surf, GBA_OFFSET_X, GBA_OFFSET_Y, NULL);
  rdpq_detach_show();
#endif
}

void n64_video_flip(void)
{
  surface_t *disp = display_get();
  if (disp)
    display_show(disp);
}

void n64_video_clear(void)
{
  surface_t *disp = display_get();
  if (!disp) return;
  rdpq_attach(disp, NULL);
  rdpq_set_mode_fill(RGBA16(0, 0, 0, 1));
  rdpq_fill_rectangle(0, 0, N64_SCREEN_WIDTH, N64_SCREEN_HEIGHT);
  rdpq_detach_show();
}

void n64_video_draw_text(int x, int y, const char *text)
{
  surface_t *disp = display_get();
  if (!disp) return;

  graphics_set_color(0xFFFFFFFF, 0x00000001);
  graphics_draw_text(disp, x, y, text);
  display_show(disp);
}
