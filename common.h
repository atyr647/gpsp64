/* gameplaySP
 *
 * Copyright (C) 2006 Exophase <exophase@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef COMMON_H
#define COMMON_H

#define ror(dest, value, shift)                                               \
  dest = ((value) >> (shift)) | ((value) << (32 - (shift)))                   \

#define MAX(a,b)  ((a) > (b) ? (a) : (b))
#define MIN(a,b)  ((a) < (b) ? (a) : (b))

#if defined(_WIN32)
  #define PATH_SEPARATOR "\\"
  #define PATH_SEPARATOR_CHAR '\\'
#else
  #define PATH_SEPARATOR "/"
  #define PATH_SEPARATOR_CHAR '/'
#endif

/* On x86 we pass arguments via registers instead of stack */
#ifdef X86_ARCH
  #define function_cc __attribute__((regparm(2)))
#else
  #define function_cc
#endif

#ifdef ARM_ARCH

#define _BSD_SOURCE // sync
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdarg.h>
#include <time.h>
#include <sys/time.h>

#endif /* ARM_ARCH */

// Huge thanks to pollux for the heads up on using native file I/O
// functions on PSP for vastly improved memstick performance.

#ifdef PSP
  #include <pspkernel.h>
  #include <pspdebug.h>
  #include <pspctrl.h>
  #include <pspgu.h>
  #include <pspaudio.h>
  #include <pspaudiolib.h>
  #include <psprtc.h>
  #include <time.h>
#elif defined(N64)
  #include <stdint.h>
  #include <stdbool.h>
  typedef uint8_t u8;
  typedef int8_t s8;
  typedef uint16_t u16;
  typedef int16_t s16;
  typedef uint32_t u32;
  typedef int32_t s32;
  typedef uint64_t u64;
  typedef int64_t s64;
  /* Stub libretro constants needed by serial/rfu code */
  #define RETRO_NETPACKET_BROADCAST 0xFFFF
#else
  typedef unsigned char u8;
  typedef signed char s8;
  typedef unsigned short int u16;
  typedef signed short int s16;
  typedef unsigned int u32;
  typedef signed int s32;
  typedef unsigned long long int u64;
  typedef signed long long int s64;
#endif

#if defined(USE_XBGR1555_FORMAT)
  #define convert_palette(value)  \
    (value & 0x7FFF)
#else
  #define convert_palette(value) \
    (((value & 0x1F) << 11) | ((value & 0x03E0) << 1) | ((value >> 10) & 0x1F))
#endif

#define GBA_SCREEN_WIDTH  (240)
#define GBA_SCREEN_HEIGHT (160)
#define GBA_SCREEN_PITCH  (240)

// The buffer is 16 bit color depth.
// We reserve extra memory at the end for extra effects (winobj rendering).
#define GBA_SCREEN_BUFFER_SIZE  \
  (GBA_SCREEN_PITCH * (GBA_SCREEN_HEIGHT + 1) * sizeof(uint16_t))

#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  #define netorder32(value) (value)
#else
  #define netorder32(value) __builtin_bswap32(value)
#endif

typedef u32 fixed16_16;
typedef u32 fixed8_24;

#define float_to_fp16_16(value)                                               \
  (fixed16_16)((value) * 65536.0)                                             \

#define fp16_16_to_float(value)                                               \
  (float)((value) / 65536.0)                                                  \

#define u32_to_fp16_16(value)                                                 \
  ((value) << 16)                                                             \

#define fp16_16_to_u32(value)                                                 \
  ((value) >> 16)                                                             \

#define fp16_16_fractional_part(value)                                        \
  ((value) & 0xFFFF)                                                          \

#define float_to_fp8_24(value)                                                \
  (fixed8_24)((value) * 16777216.0)                                           \

#define fp8_24_fractional_part(value)                                         \
  ((value) & 0xFFFFFF)                                                        \

#define fixed_div(numerator, denominator, bits)                               \
  (((numerator * (1 << bits)) + (denominator / 2)) / denominator)             \

/*
 * Memory access macros.
 *
 * N64 big-endian strategy: GBA data is stored "word-swapped" — each 32-bit
 * word is in native byte order. ROM/BIOS data is bswap32'd at load time.
 *
 * - 32-bit access: unchanged (native lw/sw just works)
 * - 16-bit access: XOR offset with 2 (selects correct halfword within word)
 * - 8-bit access:  XOR offset with 3 (selects correct byte within word)
 *
 * eswap becomes identity since data is already in native byte order.
 * All GBA memory MUST be accessed through these macros, never directly.
 */
#if defined(N64)
  #define address8(base, offset)                                              \
    *((u8 *)((u8 *)(base) + ((offset) ^ 3)))
  #define address16(base, offset)                                             \
    *((u16 *)((u8 *)(base) + ((offset) ^ 2)))
  #define address32(base, offset)                                             \
    *((u32 *)((u8 *)(base) + (offset)))

  #define eswap8(value)  (value)
  #define eswap16(value) (value)
  #define eswap32(value) (value)

  /* Dereference a u16 or u8 pointer into word-swapped GBA memory.
     XORs the pointer address to select the correct halfword or byte. */
  #define gba_deref16(ptr)  (*(u16*)((uintptr_t)(ptr) ^ 2))
  #define gba_deref8(ptr)   (*(u8*)((uintptr_t)(ptr) ^ 3))
  /* 32-bit deref: no XOR needed */
  #define gba_deref32(ptr)  (*(u32*)(ptr))

  /* Word-swap a buffer in-place after loading from file */
  static inline void wordswap_buffer(void *buf, unsigned int len) {
    u32 *p = (u32 *)buf;
    for (unsigned int i = 0; i < len / 4; i++)
      p[i] = __builtin_bswap32(p[i]);
  }
#else
  #define address8(base, offset)                                              \
    *((u8 *)((u8 *)(base) + (offset)))
  #define address16(base, offset)                                             \
    *((u16 *)((u8 *)(base) + (offset)))
  #define address32(base, offset)                                             \
    *((u32 *)((u8 *)(base) + (offset)))

  #define eswap8(value)  (value)
  #if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    #define eswap16(value) __builtin_bswap16(value)
    #define eswap32(value) __builtin_bswap32(value)
  #else
    #define eswap16(value) (value)
    #define eswap32(value) (value)
  #endif

  #define gba_deref16(ptr)  eswap16(*(ptr))
  #define gba_deref8(ptr)   (*(ptr))
  #define gba_deref32(ptr)  eswap32(*(u32*)(ptr))
#endif

/* Native (non-GBA) 16-bit access: never XOR'd, for derived tables */
#define native16(base, offset) *((u16 *)((u8 *)(base) + (offset)))

#define  readaddress8(base, offset) eswap8( address8( base, offset))
#define readaddress16(base, offset) eswap16(address16(base, offset))
#define readaddress32(base, offset) eswap32(address32(base, offset))

/* I/O register access: MUST go through address16 for XOR on N64 */
#define read_ioreg(regnum)  address16(io_registers, (regnum) * 2)
#define write_ioreg(regnum, val) (address16(io_registers, (regnum) * 2) = (val))
#define read_ioreg32(regnum) (read_ioreg(regnum) | (read_ioreg((regnum)+1) << 16))

#define read_dmareg(regnum, dmachan) \
  address16(io_registers, ((regnum) + (dmachan) * 6) * 2)
#define write_dmareg(regnum, dmachan, val) \
  (address16(io_registers, ((regnum) + (dmachan) * 6) * 2) = (val))

#include <unistd.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "cpu.h"
#include "gba_memory.h"
#include "savestate.h"
#include "video.h"
#include "input.h"
#include "sound.h"
#include "main.h"
#include "cheats.h"
#include "serial.h"

#endif
