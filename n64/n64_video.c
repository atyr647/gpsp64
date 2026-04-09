/* gameplaySP - N64 Video Output
 *
 * Renders the GBA's 240x160 16-bit framebuffer centered on the N64's
 * 320x240 display.
 *
 * GBA uses RGB565 internally (FRONTEND_SUPPORTS_RGB565), but we compile
 * with USE_XBGR1555_FORMAT so the GBA core outputs RGB555.
 * N64 uses RGBA5551 natively. Conversion: set the alpha bit to 1.
 *
 * N64 port Copyright (C) 2026
 */

#include <libdragon.h>
#include <string.h>
#include "../common.h"
#include "n64_video.h"

extern u16 *gba_screen_pixels;

void n64_video_init(void)
{
  display_init(RESOLUTION_320x240, DEPTH_16_BPP, 2,
               GAMMA_NONE, FILTERS_RESAMPLE);
}

void n64_video_render_frame(void)
{
  surface_t *disp = display_get();
  if (!disp) return;

  u16 *fb = (u16 *)disp->buffer;
  u16 *src = gba_screen_pixels;

  if (!src) {
    display_show(disp);
    return;
  }

  /* Clear the full framebuffer first (borders) */
  memset(fb, 0, N64_SCREEN_WIDTH * N64_SCREEN_HEIGHT * sizeof(u16));

  /* Blit GBA framebuffer centered into N64 framebuffer.
   * GBA with USE_XBGR1555_FORMAT outputs 0BBBBBGGGGGRRRRR (XBGR1555).
   * N64 RGBA5551 is RRRRRGGGGGBBBBBA.
   * We need to swap R and B channels and set alpha=1. */
  for (int y = 0; y < GBA_SCREEN_HEIGHT; y++) {
    u16 *dst_row = fb + (GBA_OFFSET_Y + y) * N64_SCREEN_WIDTH + GBA_OFFSET_X;
    u16 *src_row = src + y * GBA_SCREEN_PITCH;

    for (int x = 0; x < GBA_SCREEN_WIDTH; x++) {
      u16 pixel = src_row[x];
      /* XBGR1555: 0 BBBBB GGGGG RRRRR */
      u16 r = (pixel)       & 0x1F;
      u16 g = (pixel >> 5)  & 0x1F;
      u16 b = (pixel >> 10) & 0x1F;
      /* RGBA5551: RRRRR GGGGG BBBBB 1 */
      dst_row[x] = (r << 11) | (g << 6) | (b << 1) | 1;
    }
  }

  display_show(disp);
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
  memset(disp->buffer, 0, N64_SCREEN_WIDTH * N64_SCREEN_HEIGHT * sizeof(u16));
  display_show(disp);
}

void n64_video_draw_text(int x, int y, const char *text)
{
  surface_t *disp = display_get();
  if (!disp) return;

  graphics_set_color(0xFFFFFFFF, 0x00000001);
  graphics_draw_text(disp, x, y, text);
  display_show(disp);
}
