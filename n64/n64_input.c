/* gameplaySP - N64 Input
 *
 * Maps N64 controller buttons to GBA input.
 *
 * Mapping:
 *   GBA D-Pad   -> N64 D-Pad
 *   GBA A       -> N64 A
 *   GBA B       -> N64 B
 *   GBA L       -> N64 L
 *   GBA R       -> N64 R
 *   GBA Start   -> N64 Start
 *   GBA Select  -> N64 Z
 *
 * N64 port Copyright (C) 2026
 */

#include <libdragon.h>
#include "../common.h"
#include "../gba_memory.h"
#include "../input.h"
#include "n64_input.h"

void n64_input_init(void)
{
  joypad_init();
}

void n64_input_poll(void)
{
  joypad_poll();
}

void n64_input_update(void)
{
  joypad_buttons_t buttons = joypad_get_buttons(JOYPAD_PORT_1);

  /* GBA key register is active-low: 0 = pressed, 1 = released
   * Bits: 0=A, 1=B, 2=Select, 3=Start, 4=Right, 5=Left, 6=Up, 7=Down,
   *       8=R, 9=L */
  u16 key_input = 0x3FF;  /* All released */

  if (buttons.a)     key_input &= ~(1 << 0);  /* A */
  if (buttons.b)     key_input &= ~(1 << 1);  /* B */
  if (buttons.z)     key_input &= ~(1 << 2);  /* Select = Z */
  if (buttons.start) key_input &= ~(1 << 3);  /* Start */
  if (buttons.d_right) key_input &= ~(1 << 4);  /* Right */
  if (buttons.d_left)  key_input &= ~(1 << 5);  /* Left */
  if (buttons.d_up)    key_input &= ~(1 << 6);  /* Up */
  if (buttons.d_down)  key_input &= ~(1 << 7);  /* Down */
  if (buttons.r)     key_input &= ~(1 << 8);  /* R */
  if (buttons.l)     key_input &= ~(1 << 9);  /* L */

  /* Also support analog stick as D-pad */
  joypad_inputs_t inputs = joypad_get_inputs(JOYPAD_PORT_1);
  if (inputs.stick_x > 40)  key_input &= ~(1 << 4);  /* Right */
  if (inputs.stick_x < -40) key_input &= ~(1 << 5);  /* Left */
  if (inputs.stick_y > 40)  key_input &= ~(1 << 6);  /* Up */
  if (inputs.stick_y < -40) key_input &= ~(1 << 7);  /* Down */

  /* Write to GBA key input register */
  write_ioreg(REG_P1, key_input);
}
