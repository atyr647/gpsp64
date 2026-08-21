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
/* gpSP's MIPS dynarec.  Off by default: the backend is largely ported to
 * VR4300 (MIPS III fallbacks for movz/movn/madd, no JAL patching, I-cache
 * invalidation) but was abandoned mid-debug against a crash.  Build with
 * -DN64_DYNAREC_DEFAULT=1 to work on it. */
#ifndef N64_DYNAREC_DEFAULT
#define N64_DYNAREC_DEFAULT 0
#endif
int dynarec_enable = N64_DYNAREC_DEFAULT;
boot_mode selected_boot_mode = boot_game;
int sprite_limit = 1;
u32 netplay_num_clients = 0, netplay_client_id = 0;

u32 idle_loop_target_pc = 0xFFFFFFFF;
u32 translation_gate_target_pc[MAX_TRANSLATION_GATES];
u32 translation_gate_targets = 0;

static bool emulator_running = false;

/* Simple frameskip state.
 *
 * FRAMESKIP_INTERVAL is how many frames are skipped after each rendered
 * one, so 0 = render everything, 1 = render every other frame.  Note
 * that the FPS this file reports counts *emulated* frames, which is
 * (interval + 1) times the rate the player actually sees -- skipping is
 * only a win if it buys back more emulated throughput than the frames it
 * throws away, and the PPU + blit are together ~50% of frame time, so it
 * does not.  Overridable so it can be A/B'd on ares. */
static u32 frameskip_counter = 0;
#ifndef FRAMESKIP_INTERVAL
#define FRAMESKIP_INTERVAL 1
#endif

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

  /* Diagnostic: report the gamepak code and the idle-loop override
   * that load_gamepak picked up (if any).  An idle_loop_target_pc of
   * 0xFFFFFFFF means no override matched -> the per-frame busy-wait
   * detection is OFF. */
  {
    extern u8 *gamepak_buffers[];
    char gc[5] = {0};
    memcpy(gc, &gamepak_buffers[0][0xAC], 4);
    debugf("[gpSP]: gamepak_code='%s' idle_loop_target_pc=0x%08lx\n",
           gc, (unsigned long)idle_loop_target_pc);
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

  /* Initialize assembly Thumb dispatch handler table */
  extern void init_thumb_handler_table(void);
  init_thumb_handler_table();

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
          extern u32 prof_idle_hits, prof_idle_detect_fires;
          u32 total_insns = prof_arm_insns + prof_thumb_insns;
          u32 total_ms = ms_per_frame * 60;
          u32 kips = total_ms ? (total_insns / total_ms) : 0;
          u32 cyc_per_insn = total_insns ? (u32)((u64)prof_total * 2 / total_insns) : 0;
          u32 arm_pct = total_insns ? (prof_arm_insns   * 100) / total_insns : 0;
          u32 thm_pct = total_insns ? (prof_thumb_insns * 100) / total_insns : 0;
          debugf("PROF: CPU%lu%% PPU%lu%% Blt%lu%% %lums/f | %luK insns %luKIPS ~%lu cyc/i"
                 " | ARM%lu%%/Thm%lu%% idle %lu rt %lu\n",
                 (unsigned long)cpu_pct,
                 (unsigned long)ppu_pct,
                 (unsigned long)blit_pct,
                 (unsigned long)ms_per_frame,
                 (unsigned long)(total_insns / 1000),
                 (unsigned long)kips,
                 (unsigned long)cyc_per_insn,
                 (unsigned long)arm_pct, (unsigned long)thm_pct,
                 (unsigned long)prof_idle_hits,
                 (unsigned long)prof_idle_detect_fires);

#ifdef PROFILE_CYCLES
          /* Where the VR4300 cycles actually go inside one emulated
           * frame.  prof_emu is all of run_frame(); update_gba is the
           * hardware event engine (scanline/timer/DMA/IRQ) and already
           * contains the PPU render; AOT is translated code.  Whatever
           * is left is the interpreter proper. */
          {
            extern u32 prof_update_ticks, prof_update_calls, prof_aot_ticks;
            extern u32 prof_aot_hits, prof_aot_gba_cycles;
            u32 upd_ex_ppu = prof_update_ticks > prof_ppu_ticks
                           ? prof_update_ticks - prof_ppu_ticks : 0;
            u32 accounted  = prof_update_ticks + prof_aot_ticks;
            u32 interp     = prof_emu > accounted ? prof_emu - accounted : 0;
            u32 e = prof_emu ? prof_emu : 1;
            debugf("PROF:  cyc: interp %lu%% aot %lu%% event %lu%% ppu %lu%%"
                   " | yields/frame %lu | aot-calls %lu gba-cyc %luK\n",
                   (unsigned long)((u64)interp       * 100 / e),
                   (unsigned long)((u64)prof_aot_ticks * 100 / e),
                   (unsigned long)((u64)upd_ex_ppu   * 100 / e),
                   (unsigned long)((u64)prof_ppu_ticks * 100 / e),
                   (unsigned long)(prof_update_calls / 60),
                   (unsigned long)prof_aot_hits,
                   (unsigned long)(prof_aot_gba_cycles / 1000));
            prof_update_ticks = prof_update_calls = prof_aot_ticks = 0;
          }
#endif

#ifdef PROFILE_OPCODES
          /* Heavy diagnostics — opcode histograms, top-N, PC sample.
           * Only built when PROFILE_OPCODES is defined; off by default.
           * Re-enable with -DPROFILE_OPCODES in CFLAGS+ASFLAGS to find
           * new busy-wait loops or guide native handler choices. */
          extern u32 prof_thumb_hist[256], prof_arm_hist[256];
          extern u32 prof_last_d9_pc, prof_last_28_pc, prof_last_88_pc;
          {
            u32 cat_alu = 0, cat_ldst = 0, cat_block = 0, cat_br = 0;
            for (u32 i = 0x00; i <= 0x47; i++) cat_alu   += prof_thumb_hist[i];
            for (u32 i = 0x48; i <= 0x9F; i++) cat_ldst  += prof_thumb_hist[i];
            for (u32 i = 0xA0; i <= 0xCF; i++) cat_block += prof_thumb_hist[i];
            for (u32 i = 0xD0; i <= 0xFF; i++) cat_br    += prof_thumb_hist[i];
            u32 th = prof_thumb_insns ? prof_thumb_insns : 1;
            debugf("PROF:  cat: ALU %lu%% LDST %lu%% BLOCK %lu%% BR %lu%%\n",
                   (unsigned long)((cat_alu   * 100) / th),
                   (unsigned long)((cat_ldst  * 100) / th),
                   (unsigned long)((cat_block * 100) / th),
                   (unsigned long)((cat_br    * 100) / th));

            u32 top_idx[10] = {0};
            u32 top_cnt[10] = {0};
            for (u32 i = 0; i < 256; i++) {
              u32 c = prof_thumb_hist[i];
              if (c == 0) continue;
              for (u32 j = 0; j < 10; j++) {
                if (c > top_cnt[j]) {
                  for (u32 k = 9; k > j; k--) {
                    top_cnt[k] = top_cnt[k-1];
                    top_idx[k] = top_idx[k-1];
                  }
                  top_cnt[j] = c;
                  top_idx[j] = i;
                  break;
                }
              }
            }
            debugf("PROF:  top-thumb:"
                   " %02lx:%lu%% %02lx:%lu%% %02lx:%lu%% %02lx:%lu%% %02lx:%lu%%"
                   " %02lx:%lu%% %02lx:%lu%% %02lx:%lu%% %02lx:%lu%% %02lx:%lu%%\n",
                   (unsigned long)top_idx[0], (unsigned long)((top_cnt[0]*100)/th),
                   (unsigned long)top_idx[1], (unsigned long)((top_cnt[1]*100)/th),
                   (unsigned long)top_idx[2], (unsigned long)((top_cnt[2]*100)/th),
                   (unsigned long)top_idx[3], (unsigned long)((top_cnt[3]*100)/th),
                   (unsigned long)top_idx[4], (unsigned long)((top_cnt[4]*100)/th),
                   (unsigned long)top_idx[5], (unsigned long)((top_cnt[5]*100)/th),
                   (unsigned long)top_idx[6], (unsigned long)((top_cnt[6]*100)/th),
                   (unsigned long)top_idx[7], (unsigned long)((top_cnt[7]*100)/th),
                   (unsigned long)top_idx[8], (unsigned long)((top_cnt[8]*100)/th),
                   (unsigned long)top_idx[9], (unsigned long)((top_cnt[9]*100)/th));

            debugf("PROF:  last 88@0x%08lx 28@0x%08lx d9@0x%08lx\n",
                   (unsigned long)prof_last_88_pc,
                   (unsigned long)prof_last_28_pc,
                   (unsigned long)prof_last_d9_pc);
          }
          memset(prof_thumb_hist, 0, sizeof(prof_thumb_hist));
          memset(prof_arm_hist,   0, sizeof(prof_arm_hist));
#endif

#ifdef PROFILE_PPU
          /* PPU sub-category breakdown within update_scanline, expressed
           * as % of total PPU ticks (prof_ppu_ticks).  "sort" = OAM sort
           * when OAM is dirty, "lo" = layer ordering per scanline,
           * "render" = render_scanline_window (dispatches to BG/OBJ/
           * compose), "blank" = forced-blank memset, "aff" = affine-
           * reference tail update. */
          extern u32 prof_ppu_obj_sort_ticks, prof_ppu_layer_order_ticks;
          extern u32 prof_ppu_render_ticks, prof_ppu_blank_ticks;
          extern u32 prof_ppu_affine_ticks;
          extern u32 prof_ppu_sort_calls, prof_ppu_blank_calls;
          {
            u32 ppu = prof_ppu_ticks ? prof_ppu_ticks : 1;
            debugf("PROF:  ppu: sort%lu%% lo%lu%% render%lu%% blank%lu%% aff%lu%% | sorts=%lu blanks=%lu\n",
                   (unsigned long)((prof_ppu_obj_sort_ticks    * 100) / ppu),
                   (unsigned long)((prof_ppu_layer_order_ticks * 100) / ppu),
                   (unsigned long)((prof_ppu_render_ticks      * 100) / ppu),
                   (unsigned long)((prof_ppu_blank_ticks       * 100) / ppu),
                   (unsigned long)((prof_ppu_affine_ticks      * 100) / ppu),
                   (unsigned long)prof_ppu_sort_calls,
                   (unsigned long)prof_ppu_blank_calls);
          }
          prof_ppu_obj_sort_ticks = 0;
          prof_ppu_layer_order_ticks = 0;
          prof_ppu_render_ticks = 0;
          prof_ppu_blank_ticks = 0;
          prof_ppu_affine_ticks = 0;
          prof_ppu_sort_calls = 0;
          prof_ppu_blank_calls = 0;
#endif

#ifdef PROFILE_AOT
          /* AOT HLE candidate profiling.
           * Top-5 hot 4KB code pages (by execution count) and
           * top-5 BL call targets (by call count).
           * These identify the ARM/Thumb functions worth recompiling. */
          {
            extern u32 prof_page_hist[];
            typedef struct { u32 pc; u32 count; } bl_target_t;
            extern bl_target_t prof_bl_targets[];

            /* Top-5 hot pages */
            u32 pg_idx[5] = {0};
            u32 pg_cnt[5] = {0};
            for (u32 i = 0; i < 6144; i++) {
              u32 c = prof_page_hist[i];
              if (c == 0) continue;
              for (u32 j = 0; j < 5; j++) {
                if (c > pg_cnt[j]) {
                  for (u32 k = 4; k > j; k--) {
                    pg_cnt[k] = pg_cnt[k-1];
                    pg_idx[k] = pg_idx[k-1];
                  }
                  pg_cnt[j] = c;
                  pg_idx[j] = i;
                  break;
                }
              }
            }
            u32 ti = prof_arm_insns + prof_thumb_insns;
            ti = ti ? ti : 1;
            {
            extern u32 prof_rom_page_misses;
            debugf("PROF:  rom-page-misses: %lu (buffer %d MB)\n",
                   (unsigned long)prof_rom_page_misses, ROM_BUFFER_SIZE);
            prof_rom_page_misses = 0;
          }
          debugf("PROF:  aot-pages:"
                   " 0x%05lx:%lu%% 0x%05lx:%lu%% 0x%05lx:%lu%%"
                   " 0x%05lx:%lu%% 0x%05lx:%lu%%\n",
                   (unsigned long)((pg_idx[0]+0x8000)*0x1000), (unsigned long)((pg_cnt[0]*100)/ti),
                   (unsigned long)((pg_idx[1]+0x8000)*0x1000), (unsigned long)((pg_cnt[1]*100)/ti),
                   (unsigned long)((pg_idx[2]+0x8000)*0x1000), (unsigned long)((pg_cnt[2]*100)/ti),
                   (unsigned long)((pg_idx[3]+0x8000)*0x1000), (unsigned long)((pg_cnt[3]*100)/ti),
                   (unsigned long)((pg_idx[4]+0x8000)*0x1000), (unsigned long)((pg_cnt[4]*100)/ti));

            /* Top-5 BL targets */
            u32 bl_idx[5] = {0};
            u32 bl_cnt[5] = {0};
            for (u32 i = 0; i < 512; i++) {
              u32 c = prof_bl_targets[i].count;
              if (c == 0) continue;
              for (u32 j = 0; j < 5; j++) {
                if (c > bl_cnt[j]) {
                  for (u32 k = 4; k > j; k--) {
                    bl_cnt[k] = bl_cnt[k-1];
                    bl_idx[k] = bl_idx[k-1];
                  }
                  bl_cnt[j] = c;
                  bl_idx[j] = i;
                  break;
                }
              }
            }
            debugf("PROF:  aot-bl:"
                   " 0x%08lx:%lu 0x%08lx:%lu 0x%08lx:%lu"
                   " 0x%08lx:%lu 0x%08lx:%lu\n",
                   (unsigned long)prof_bl_targets[bl_idx[0]].pc, (unsigned long)bl_cnt[0],
                   (unsigned long)prof_bl_targets[bl_idx[1]].pc, (unsigned long)bl_cnt[1],
                   (unsigned long)prof_bl_targets[bl_idx[2]].pc, (unsigned long)bl_cnt[2],
                   (unsigned long)prof_bl_targets[bl_idx[3]].pc, (unsigned long)bl_cnt[3],
                   (unsigned long)prof_bl_targets[bl_idx[4]].pc, (unsigned long)bl_cnt[4]);

            memset(prof_page_hist, 0, sizeof(u32) * 6144);
            memset(prof_bl_targets, 0, sizeof(prof_bl_targets[0]) * 512);
          }

#ifdef PROFILE_REGIONS
          /* Where interpreted code lives, by GBA memory region.  ROM and
           * BIOS are at fixed addresses so they can be translated ahead
           * of time; IWRAM/EWRAM code is copied there at runtime, which
           * a static translator cannot follow. */
          {
            extern u32 prof_region_arm[16], prof_region_thumb[16];
            static const char *rn[16] = {
              "bios","?1","ewram","iwram","io","pal","vram","oam",
              "rom","rom","rom","rom","rom","sram","?e","?f"
            };
            u32 tot = 0;
            for (u32 i = 0; i < 16; i++)
              tot += prof_region_arm[i] + prof_region_thumb[i];
            if (!tot) tot = 1;
            debugf("PROF:  regions(arm/thm %%):");
            for (u32 i = 0; i < 16; i++) {
              u32 a = prof_region_arm[i], t = prof_region_thumb[i];
              if (!a && !t) continue;
              debugf(" %s:%lu/%lu", rn[i],
                     (unsigned long)((a * 100) / tot),
                     (unsigned long)((t * 100) / tot));
            }
            debugf("\n");
            memset(prof_region_arm, 0, sizeof(prof_region_arm));
            memset(prof_region_thumb, 0, sizeof(prof_region_thumb));
          }

          /* Top BIOS SWIs by call count -- the candidates for native
           * (HLE) implementations, which would remove the interpreted
           * ARM BIOS handlers entirely. */
          {
            extern u32 prof_swi_hist[256];
            u32 si[6] = {0}, sc[6] = {0};
            for (u32 i = 0; i < 256; i++) {
              u32 c = prof_swi_hist[i];
              if (!c) continue;
              for (u32 j = 0; j < 6; j++) {
                if (c > sc[j]) {
                  for (u32 k = 5; k > j; k--) { sc[k]=sc[k-1]; si[k]=si[k-1]; }
                  sc[j] = c; si[j] = i;
                  break;
                }
              }
            }
            debugf("PROF:  swi:");
            for (u32 j = 0; j < 6; j++)
              if (sc[j]) debugf(" %02lx:%lu", (unsigned long)si[j],
                                (unsigned long)sc[j]);
            debugf("\n");
            memset(prof_swi_hist, 0, sizeof(prof_swi_hist));
          }
#endif /* PROFILE_REGIONS */
#endif

          prof_emu = prof_blit = prof_total = prof_frames = 0;
          prof_ppu_ticks = 0;
          prof_arm_insns = prof_thumb_insns = 0;
          prof_idle_hits = 0;
          prof_idle_detect_fires = 0;
        }
      }
    }
  }

  return 0;
}
