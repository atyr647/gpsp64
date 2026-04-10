/* gameplaySP - N64 Video Output (RDP-accelerated)
 *
 * Uses libdragon's rdpq to hardware-blit the GBA framebuffer.
 * The GBA core outputs XBGR1555 (with USE_XBGR1555_FORMAT).
 * We wrap it in a surface_t and use rdpq_tex_blit for the copy,
 * which offloads the pixel shuffle to the RDP hardware.
 *
 * N64 port Copyright (C) 2026
 */

#include <libdragon.h>
#include <string.h>
#include "../common.h"
#include "n64_video.h"

extern u16 *gba_screen_pixels;

/* Pre-converted GBA framebuffer in RGBA5551 format */
static u16 *rgba_buf = NULL;

void n64_video_init(void)
{
  display_init(RESOLUTION_320x240, DEPTH_16_BPP, 2,
               GAMMA_NONE, FILTERS_RESAMPLE);
  rdpq_init();
  rgba_buf = (u16 *)malloc(GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT * sizeof(u16));
}

void n64_video_render_frame(void)
{
  surface_t *disp = display_get();
  if (!disp || !gba_screen_pixels || !rgba_buf) {
    if (disp) display_show(disp);
    return;
  }

  /* Convert XBGR1555 -> RGBA5551 into a staging buffer.
   * This is still CPU work but the blit to framebuffer is RDP. */
  u16 *src = gba_screen_pixels;
  u16 *dst = rgba_buf;
  for (int i = 0; i < GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT; i++) {
    u16 pixel = src[i];
    u16 r = (pixel)       & 0x1F;
    u16 g = (pixel >> 5)  & 0x1F;
    u16 b = (pixel >> 10) & 0x1F;
    dst[i] = (r << 11) | (g << 6) | (b << 1) | 1;
  }

  /* Wrap the converted buffer as a surface for rdpq */
  surface_t gba_surf = surface_make_linear(rgba_buf,
    FMT_RGBA16, GBA_SCREEN_WIDTH, GBA_SCREEN_HEIGHT);

  /* Use RDP to blit the GBA surface centered on the N64 display */
  rdpq_attach(disp, NULL);
  rdpq_set_mode_fill(RGBA16(0, 0, 0, 1));
  rdpq_fill_rectangle(0, 0, N64_SCREEN_WIDTH, N64_SCREEN_HEIGHT);
  rdpq_set_mode_copy(false);
  rdpq_tex_blit(&gba_surf, GBA_OFFSET_X, GBA_OFFSET_Y, NULL);
  rdpq_detach_show();
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
