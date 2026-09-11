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

#ifdef N64_AUDIO_VERIFY
u32 prof_audio_samples = 0, prof_audio_calls = 0, prof_audio_zero = 0;
#endif

/* How much mixed audio to keep un-played.  Enough that a frame which runs
 * long cannot starve the DMA, short enough not to be heard lagging the
 * picture: three frames, about 50 ms. */
#define AUDIO_TARGET_FRAMES  (SAMPLES_PER_FRAME * 3)
/* Past this the backlog is not something ±1 sample a frame can walk off --
 * it is boot, or a savestate load, arriving most of a second ahead. */
#define AUDIO_RESYNC_FRAMES  (SAMPLES_PER_FRAME * 16)
/* How far either side of the target to let the backlog wander before
 * correcting.  Wide enough that the ±1 is not applied every frame, narrow
 * enough that a correction lands within ~64 frames -- a dead band of a
 * whole frame's worth takes a thousand frames to cross and reads as drift
 * that never gets caught. */
#define AUDIO_DEADBAND       64

void n64_audio_render_frame(void)
{
  /* Keep the mixer and the DAC locked to each other.
   *
   * The GBA engine mixes 280,896 * 22050 / 16,777,216 = 369.15 stereo
   * frames per emulated frame and this reads 369, so the backlog creeps
   * up by a sixth of a sample a frame.  That is small, and it never stops:
   * left alone it walks the whole 65,536-sample ring and wraps, which is
   * an audible discontinuity for no reason.  Taking one sample more or
   * less depending on which side of the target the backlog sits removes
   * the drift entirely, and a 1-in-369 rate change is 0.27% -- four cents,
   * inaudible, and applied only while correcting.
   */
  u32 want = SAMPLES_PER_FRAME;
  u32 pending = sound_pending_frames();

  if (pending > AUDIO_RESYNC_FRAMES)
    sound_drop_backlog(AUDIO_TARGET_FRAMES);
  else if (pending > AUDIO_TARGET_FRAMES + AUDIO_DEADBAND)
    want = SAMPLES_PER_FRAME + 1;
  else if (pending + AUDIO_DEADBAND < AUDIO_TARGET_FRAMES)
    want = SAMPLES_PER_FRAME - 1;

  /* Read samples from the GBA sound engine */
  u32 samples_read = sound_read_samples(audio_buffer, want);

#ifdef N64_AUDIO_VERIFY
  /* Confirm real PCM is flowing before trusting a frame-time delta as the
     cost of pushing it -- a silently-empty mixer would make audio look
     free rather than actually cheap. */
  prof_audio_calls++;
  prof_audio_samples += samples_read;
  if (samples_read == 0) prof_audio_zero++;
#endif

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
