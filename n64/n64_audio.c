/* gameplaySP - N64 Audio Output
 *
 * Uses libdragon's audio subsystem to output GBA audio.
 * GBA generates audio at 64 KHz internally; we resample down to 22050 Hz
 * for N64 output to save CPU cycles.
 *
 * N64 port Copyright (C) 2026
 */

#include <libdragon.h>
#include <string.h>
#include "../common.h"
#include "../sound.h"
#include "n64_audio.h"

/* Audio buffer for resampled output */
static s16 audio_buffer[N64_AUDIO_BUFFER_SAMPLES * 2];  /* stereo */

/* Samples per GBA frame at our output rate */
/* GBA runs at ~59.73 FPS, so samples_per_frame = 22050 / 59.73 = ~369 */
#define SAMPLES_PER_FRAME  369

void n64_audio_init(void)
{
  audio_init(N64_AUDIO_FREQUENCY, 4);
}

void n64_audio_render_frame(void)
{
  /* Read samples from the GBA sound engine */
  u32 samples_read = sound_read_samples(audio_buffer, SAMPLES_PER_FRAME);

  if (samples_read == 0)
    return;

#ifndef N64_AUDIO_NOPUSH
  /* Push samples to N64 audio output (non-blocking) */
  audio_push(audio_buffer, samples_read, false);
#endif
}

void n64_audio_shutdown(void)
{
  audio_close();
}
