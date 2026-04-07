/* gameplaySP - N64 Audio Output
 *
 * N64 port Copyright (C) 2026
 */

#ifndef N64_AUDIO_H
#define N64_AUDIO_H

/* N64 audio output frequency - use 22050 Hz to save CPU */
#define N64_AUDIO_FREQUENCY  22050

/* Small buffer to minimize memory usage */
#define N64_AUDIO_BUFFER_SAMPLES  2048

void n64_audio_init(void);
void n64_audio_render_frame(void);
void n64_audio_shutdown(void);

#endif
