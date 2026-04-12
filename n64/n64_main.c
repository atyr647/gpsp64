/* gameplaySP - N64 port
 *
 * Copyright (C) 2006 Exophase <exophase@gmail.com>
 * N64 port Copyright (C) 2026
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

#include <libdragon.h>
#include <malloc.h>
#include <string.h>
#include <stdio.h>

#include "../common.h"
#include "../gba_memory.h"
#include "../gba_cc_lut.h"

#include "n64_video.h"
#include "n64_audio.h"
#include "n64_input.h"
#include "n64_storage.h"

/* Global state required by the emulator core */
u32 skip_next_frame = 0;
u32 num_skipped_frames = 0;
int dynarec_enable = 0;
boot_mode selected_boot_mode = boot_game;
int sprite_limit = 1;
u32 netplay_num_clients = 0, netplay_client_id = 0;

u32 idle_loop_target_pc = 0xFFFFFFFF;
u32 translation_gate_target_pc[MAX_TRANSLATION_GATES];
u32 translation_gate_targets = 0;

static bool emulator_running = false;

/* Simple frameskip state */
static u32 frameskip_counter = 0;
#define FRAMESKIP_INTERVAL 1  /* Skip every other frame */

void error_msg(const char *text)
{
  debugf("[gpSP error]: %s\n", text);
}

void info_msg(const char *text)
{
  debugf("[gpSP]: %s\n", text);
}

/* These are stubs for features not needed on N64 */
void set_fastforward_override(bool fastforward) { (void)fastforward; }
/* write_rumble, rumble_frame_reset, rumble_active_pct are in gba_memory.c */
void netpacket_poll_receive(void) {}
void netpacket_send(uint16_t client_id, const void *buf, size_t len) {
  (void)client_id; (void)buf; (void)len;
}

/* ROM file browser - simple text-based menu */
static bool select_rom(char *path_out, size_t path_size)
{
  return n64_storage_browse_roms(path_out, path_size);
}

static bool load_rom_and_bios(const char *rom_path)
{
  /* Try to load official BIOS from SD card first, then DFS, then built-in */
  bool bios_ok = false;
  if (load_bios("sd:/gba_bios.bin") == 0 && bios_rom_raw[0] == 0x18) {
    bios_ok = true;
  } else if (load_bios("rom:/gba_bios.bin") == 0 && bios_rom_raw[0] == 0x18) {
    bios_ok = true;
  }
  if (!bios_ok) {
    info_msg("Using built-in BIOS");
    memcpy(bios_rom_raw, open_gba_bios_rom, sizeof(bios_rom_raw));
  }

  /* Clear backup memory */
  memset(gamepak_backup, 0xff, sizeof(gamepak_backup));

  /* Load the ROM from SD card via the core load_gamepak */
  if (load_gamepak(rom_path, FEAT_AUTODETECT, FEAT_AUTODETECT, 0) != 0) {
    error_msg("Could not load game file");
    return false;
  }

  /* Reset the GBA system */
  reset_gba();
  return true;
}

static void run_frame(void)
{
  /* Simple frameskip: skip rendering every other frame */
  skip_next_frame = (frameskip_counter % (FRAMESKIP_INTERVAL + 1)) != 0;
  frameskip_counter++;


  /* Run the CPU for one frame */
#ifdef HAVE_DYNAREC
  if (dynarec_enable)
    execute_arm_translate(execute_cycles);
  else
#endif
  {
    clear_gamepak_stickybits();
    execute_arm(execute_cycles);
  }
}

int main(void)
{
  /* Initialize N64 hardware subsystems via libdragon */
  debug_init_isviewer();
  debug_init_usblog();

  /* Initialize subsystems */
  dfs_init(DFS_DEFAULT_LOCATION);
  n64_video_init();
  n64_audio_init();
  n64_input_init();
  n64_storage_init();

  info_msg("gpSP N64 - GBA Emulator");
  info_msg("Initializing...");

  /* Static GBA screen buffer — avoids heap address issues with rdpq */
  extern u16 *gba_screen_pixels;
  static u16 __attribute__((aligned(16))) screen_buf[GBA_SCREEN_PITCH * (GBA_SCREEN_HEIGHT + 1)];
  gba_screen_pixels = screen_buf;
  memset(gba_screen_pixels, 0, sizeof(screen_buf));

  /* Initialize sound */
  init_sound();

  /* Initialize emulator core memory */
  init_gamepak_buffer();

  /* ROM selection screen */
  char rom_path[512];
  while (1) {
    /* Draw ROM browser */
    if (!select_rom(rom_path, sizeof(rom_path))) {
      /* Draw "No ROMs found" message and wait */
      n64_video_draw_text(80, 100, "No GBA ROMs found on SD card.");
      n64_video_draw_text(80, 116, "Place .gba files in sd://gba/");
      n64_video_flip();
      while (1) {
        n64_input_poll();
        /* Wait for reset */
      }
    }

    /* Load the selected ROM */
    n64_video_draw_text(80, 120, "Loading ROM...");
    n64_video_flip();

    if (!load_rom_and_bios(rom_path)) {
      n64_video_draw_text(80, 136, "Failed to load ROM!");
      n64_video_flip();
      /* Wait a few seconds then go back to browser */
      wait_ms(3000);
      continue;
    }

    /* Main emulation loop */
    emulator_running = true;
    {
      /* Profiling counters (VR4300 COUNT register, ticks at CPU/2 = 46.875 MHz) */
      u32 prof_emu = 0, prof_blit = 0, prof_total = 0;
      u32 prof_frames = 0;
      extern u32 prof_ppu_ticks;
      #define PROF_TICK() ({ u32 _t; __asm__ volatile("mfc0 %0, $9" : "=r"(_t)); _t; })

      while (emulator_running) {
        u32 t_frame_start = PROF_TICK();

        /* Poll input */
        n64_input_poll();
        n64_input_update();

        /* Run one GBA frame (CPU + PPU) */
        u32 t0 = PROF_TICK();
        run_frame();
        u32 t1 = PROF_TICK();
        prof_emu += t1 - t0;

        /* Output video (blit GBA framebuffer to N64 display) */
        u32 tb0 = PROF_TICK();
        if (!skip_next_frame) {
          n64_video_render_frame();
        }
        u32 tb1 = PROF_TICK();
        prof_blit += tb1 - tb0;

        u32 t_frame_end = PROF_TICK();
        prof_total += t_frame_end - t_frame_start;
        prof_frames++;

        /* Print profiling every 60 frames */
        if (prof_frames == 60) {
          u32 emu_pct = (prof_emu * 100) / prof_total;
          u32 blit_pct = (prof_blit * 100) / prof_total;
          u32 ppu_pct = prof_emu ? (prof_ppu_ticks * 100) / prof_emu : 0;
          u32 cpu_pct = 100 - ppu_pct;  /* CPU = emulation minus PPU */
          u32 ms_per_frame = prof_total / (46875 * 60);
          extern u32 prof_arm_insns, prof_thumb_insns;
          u32 total_insns = prof_arm_insns + prof_thumb_insns;
          u32 total_ms = ms_per_frame * 60;
          u32 kips = total_ms ? (total_insns / total_ms) : 0;
          u32 cyc_per_insn = total_insns ? (u32)((u64)prof_total * 2 / total_insns) : 0;
          debugf("PROF: CPU%lu%% PPU%lu%% %lums/f | %luK insns %luKIPS ~%lu cyc/i\n",
                 (unsigned long)cpu_pct,
                 (unsigned long)ppu_pct,
                 (unsigned long)ms_per_frame,
                 (unsigned long)(total_insns / 1000),
                 (unsigned long)kips,
                 (unsigned long)cyc_per_insn);
          prof_emu = prof_blit = prof_total = prof_frames = 0;
          prof_ppu_ticks = 0;
          prof_arm_insns = prof_thumb_insns = 0;
        }
      }
    }
  }

  return 0;
}
