/* gameplaySP - N64: savestate glue
 *
 * savestate.c calls into the input layer to save and restore the
 * previous key state.  That layer is input.c, which is the libretro
 * frontend binding and is not part of this port (nor of the native
 * benchmark harness), so the three hooks live here instead, over this
 * port's own key state.
 *
 * N64 port Copyright (C) 2026
 */

#include "../common.h"

static u32 old_key = 0;

bool input_check_savestate(const u8 *src)
{
  const u8 *p = bson_find_key(src, "input");
  return (p && bson_contains_key(p, "prevkey", BSON_TYPE_INT32));
}

bool input_read_savestate(const u8 *src)
{
  const u8 *p = bson_find_key(src, "input");
  if (p)
    return bson_read_int32(p, "prevkey", &old_key);
  return false;
}

unsigned input_write_savestate(u8 *dst)
{
  u8 *wbptr1, *startp = dst;
  bson_start_document(dst, "input", wbptr1);
  bson_write_int32(dst, "prevkey", old_key);
  bson_finish_document(dst, wbptr1);
  return (unsigned int)(dst - startp);
}
