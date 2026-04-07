/* gameplaySP - N64 Video Output
 *
 * N64 port Copyright (C) 2026
 */

#ifndef N64_VIDEO_H
#define N64_VIDEO_H

#include <libdragon.h>

/* N64 display dimensions */
#define N64_SCREEN_WIDTH   320
#define N64_SCREEN_HEIGHT  240

/* GBA display centered in N64 framebuffer */
#define GBA_OFFSET_X  ((N64_SCREEN_WIDTH  - GBA_SCREEN_WIDTH)  / 2)  /* 40 */
#define GBA_OFFSET_Y  ((N64_SCREEN_HEIGHT - GBA_SCREEN_HEIGHT) / 2)  /* 40 */

void n64_video_init(void);
void n64_video_render_frame(void);
void n64_video_flip(void);
void n64_video_draw_text(int x, int y, const char *text);
void n64_video_clear(void);

#endif
