/* gameplaySP - N64 Video Output
 *
 * Renders the GBA's 240x160 16-bit framebuffer centered on the N64's
 * 320x240 display.
 *
 * GBA uses RGB555 (or RGB565 via libretro), N64 uses RGBA5551.
 * Conversion: set the alpha bit to 1.
 *
 * N64 port Copyright (C) 2026
 */

#include <libdragon.h>
#include <string.h>
#include "../common.h"
#include "n64_video.h"

extern u16 *gba_screen_pixels;

static display_context_t disp = 0;

void n64_video_init(void)
{
  /* 320x240, 16-bit color, double-buffered */
  display_init(RESOLUTION_320x240, DEPTH_16_BPP, 2,
               GAMMA_NONE, FILTERS_RESAMPLE);
}

/* Convert a single GBA RGB565 pixel to N64 RGBA5551 */
static inline u16 rgb565_to_rgba5551(u16 rgb565)
{
  /* RGB565: RRRRRGGGGGGBBBBB
   * RGBA5551: RRRRRGGGGGBBBBB1
   *
   * R stays the same (bits 15-11)
   * G loses 1 bit (bits 10-5 -> bits 10-6)
   * B stays the same (bits 4-0 -> bits 5-1)
   * Alpha bit set to 1 (bit 0) */
  u16 r = (rgb565 >> 11) & 0x1F;
  u16 g = (rgb565 >> 6)  & 0x1F;  /* drop lowest green bit */
  u16 b = (rgb565)       & 0x1F;
  return (r << 11) | (g << 6) | (b << 1) | 1;
}

void n64_video_render_frame(void)
{
  /* Wait for a display context to become available */
  while (!(disp = display_get()))
    ;

  /* Get pointer to the N64 framebuffer */
  u16 *fb = (u16 *)__get_buffer(disp);

  /* Clear the border areas (top/bottom/left/right) */
  /* Only need to do this once really, but it's cheap at 320x240 */

  u16 *src = gba_screen_pixels;
  if (!src) {
    display_show(disp);
    return;
  }

  /* Blit GBA framebuffer centered into N64 framebuffer */
  for (int y = 0; y < GBA_SCREEN_HEIGHT; y++) {
    u16 *dst_row = fb + (GBA_OFFSET_Y + y) * N64_SCREEN_WIDTH + GBA_OFFSET_X;
    u16 *src_row = src + y * GBA_SCREEN_PITCH;

    for (int x = 0; x < GBA_SCREEN_WIDTH; x++) {
      dst_row[x] = rgb565_to_rgba5551(src_row[x]);
    }
  }

  /* Clear border regions */
  /* Top border */
  memset(fb, 0, GBA_OFFSET_Y * N64_SCREEN_WIDTH * sizeof(u16));
  /* Bottom border */
  memset(fb + (GBA_OFFSET_Y + GBA_SCREEN_HEIGHT) * N64_SCREEN_WIDTH, 0,
         GBA_OFFSET_Y * N64_SCREEN_WIDTH * sizeof(u16));
  /* Left and right borders */
  for (int y = GBA_OFFSET_Y; y < GBA_OFFSET_Y + GBA_SCREEN_HEIGHT; y++) {
    memset(fb + y * N64_SCREEN_WIDTH, 0, GBA_OFFSET_X * sizeof(u16));
    memset(fb + y * N64_SCREEN_WIDTH + GBA_OFFSET_X + GBA_SCREEN_WIDTH, 0,
           GBA_OFFSET_X * sizeof(u16));
  }

  display_show(disp);
}

void n64_video_flip(void)
{
  if (disp) {
    display_show(disp);
    disp = 0;
  }
  while (!(disp = display_get()))
    ;
  display_show(disp);
}

void n64_video_clear(void)
{
  while (!(disp = display_get()))
    ;
  u16 *fb = (u16 *)__get_buffer(disp);
  memset(fb, 0, N64_SCREEN_WIDTH * N64_SCREEN_HEIGHT * sizeof(u16));
  display_show(disp);
}

void n64_video_draw_text(int x, int y, const char *text)
{
  /* Simple text rendering using libdragon's graphics */
  while (!(disp = display_get()))
    ;

  graphics_set_color(0xFFFFFFFF, 0x00000000);
  graphics_draw_text(disp, x, y, text);

  /* Don't show yet - caller decides when to flip */
}
