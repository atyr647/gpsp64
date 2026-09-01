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
#include "../savestate.h"
#include "../gba_memory.h"
#include "../gba_cc_lut.h"

#include "n64_video.h"
#include "emux_prof.h"
#include "n64_audio.h"
#include "n64_input.h"
#include "n64_storage.h"

/* The dynarec's hardware-event call, made interruptible.
 *
 * execute_arm_translate() runs the entire translated-code window with N64
 * interrupts disabled: translated code keeps ARM r13 in $gp, and
 * libdragon's inthandler is gp-relative (it does `sw sp,-31872(gp)`), so an
 * interrupt taken inside translated code reads its own nesting state out of
 * GBA IWRAM, never acknowledges the interrupt, re-enters immediately, and
 * walks $sp out of RDRAM until the RCP freezes the CPU.
 *
 * Deferring interrupts across that window is only safe if the window is
 * short.  It is not.  The assumption was "the JIT returns once per emulated
 * frame"; restored from a savestate the game sits in its VBlank poll loop
 * (Emerald: 0x080008C6..0x080008CE, waiting on a flag the VBlank handler
 * sets) and no frame ever completes, so the window never ends.  Every wait
 * inside update_gba -- vsync, RSP/RDP sync, audio -- then blocks on an
 * interrupt that can never arrive.  Measured: from a savestate the JIT
 * translated four blocks, pinned the ARM PC at 0x080008C6, and completed
 * zero frames, where the interpreter from the same savestate completed
 * 3967.  Cold boot survived only because the JIT does return every frame
 * during boot.
 *
 * mips_update_gba is the one place the window can be reopened safely: it
 * has already spilled every ARM register to reg[] via save_registers, $sp
 * is the real N64 stack throughout (no ARM register is mapped to it), and
 * cfncall restores the real $gp before the call.  libdragon's
 * enable/disable_interrupts are nesting-counted, so this pairs with the
 * disable_interrupts() around the translated-code window rather than
 * fighting it. */
u32 function_cc n64_jit_update_gba(int remaining_cycles)
{
  u32 rv;
  enable_interrupts();
  rv = update_gba(remaining_cycles);
  disable_interrupts();
  return rv;
}

/* The dynarec's half of the BIOS-call hook.
 *
 * SWIs 0xF0-0xFF are unused by the GBA BIOS, so this port patches the game
 * with them: n64/m4a_hle.c replaces the sound driver's four hot mixer loops
 * with `swi 0xFn0000` markers and runs them natively.  cpu.cc intercepts
 * those in both the ARM and Thumb decoders and simply steps over the
 * instruction; the dynarec never had the matching hook, so it treated the
 * marker as a real BIOS SWI, vectored to 0x00000008, and let the open
 * BIOS's dispatcher index its call table with 0xF0.  The result was a
 * branch to a garbage address (0x114230B6 from the overworld savestate) a
 * few dozen events after the first IRQ.
 *
 * A savestate carries the patched IWRAM with it, so this fires even when
 * the m4a HLE is compiled out -- which is why building with N64_M4A= did
 * not change the failure.
 *
 * Returns the ARM PC to resume at.  bios_hle_swi may move reg[REG_PC]
 * itself (m4a_unpatch_and_rerun sets it to target - 4 to re-run the
 * original code), so the resume address is read back from reg[REG_PC]
 * rather than computed here, exactly as arm_pc_offset(4) does in cpu.cc.
 * An unhandled SWI in this block is a no-op the BIOS would also ignore. */
u32 n64_jit_swi_cycles;

u32 function_cc n64_jit_hle_swi(u32 swi_num, u32 swi_pc, u32 step)
{
  extern int bios_hle_swi(u32 swi_num, u32 *cycles);
  u32 cyc = 0;
  reg[REG_PC] = swi_pc;
  bios_hle_swi(swi_num, &cyc);
  n64_jit_swi_cycles = cyc;
  reg[REG_PC] += step;
  return reg[REG_PC];
}

#ifdef N64_JIT_IBPROF
/* Storage for the inline-cache hit counter incremented by mips_stub.S.
 * It lives here rather than beside the tables because those sit in the
 * .jit section, which C cannot reach with the gp-relative addressing GCC
 * picks for a small global. */
u32 ibcache_hits;
#endif

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
/* 0 = draw every frame.  Displayed rate is what the player actually sees,
   and at interval 0 it equals the emulated rate, so the FPS this file
   reports is the real number with no halving to remember.
   Measured at 14 windows: interval 1 gives 25.00 emulated but only 12.50
   displayed; interval 0 gives 17.54 of both.  Skipping buys emulated speed
   and costs smoothness, and smoothness is the thing being optimised for. */
#ifndef FRAMESKIP_INTERVAL
#define FRAMESKIP_INTERVAL 0
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

#ifdef N64_BOOT_STATE
  /* Benchmark harness: start from a captured gameplay state rather than
   * from the boot sequence.  Without this, ares only ever reaches the
   * first few hundred frames -- BIOS decompression and the Game Freak
   * logo -- where the game runs roughly thirty times the instructions
   * per frame of actual gameplay and the sound driver has not started.
   * Every number measured there is a boot number.
   *
   * The state is produced by native/bench.sh (BENCH_SAVESTATE=...) and
   * staged into the DFS image by native/ares_bench.sh.  It costs 416 KB
   * of ROM, so it is off unless explicitly built in. */
  {
    /* Through stdio, not dfs_open: the raw DFS API takes "/boot.sav",
     * while the "rom:/" prefix belongs to the newlib filesystem hook that
     * the rest of this file already uses. */
    FILE *f = fopen("rom:/boot.sav", "rb");
    if (f) {
      void *buf = malloc(GBA_STATE_MEM_SIZE);
      if (buf) {
        size_t got = fread(buf, 1, GBA_STATE_MEM_SIZE, f);
        debugf("[gpSP]: boot.sav %u bytes, load %s\n", (unsigned)got,
               (got == GBA_STATE_MEM_SIZE && gba_load_state(buf)) ? "OK" : "FAILED");
        /* IWRAM 0x7FFC holds the game's IRQ handler pointer -- the BIOS
         * IRQ dispatcher at 0x20 reaches it via `ldr pc, [r0, #-4]` with
         * r0 = 0x04000000, i.e. the IWRAM mirror at 0x03FFFFFC.  Under the
         * JIT that load comes back as 0x00000020 and the block jumps to
         * itself forever; the interpreter reads 0x03001E20.  Print the raw
         * bytes to settle whether the memory or the load is wrong. */
#ifdef N64_JIT_TRACE
        { const u8 *ph = &iwram_raw[0x8000 + 0x7FFC];
          debugf("[gpSP]: iwram+7FFC raw = %02x %02x %02x %02x\n",
                 ph[0], ph[1], ph[2], ph[3]); }
#endif
        free(buf);
      }
      fclose(f);
    } else {
      debugf("[gpSP]: boot.sav not present in DFS\n");
    }
  }
#endif

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

/* A deliberate shift of every function that links after this one.
 *
 * The VR4300's 16 KB instruction cache is direct-mapped, so which lines
 * of the interpreter collide with which lines of the renderer is decided
 * purely by where the linker happened to put them.  This port has hit
 * that before -- dropping 417 KB of dead text measured 1.4% SLOWER -- but
 * it becomes a measurement problem the moment a change adds code: two
 * builds of the same source, differing only in profiling counters, have
 * measured 66 ms and 53 ms.  A change worth 5 ms cannot be read off a
 * single run against a single baseline when layout alone is worth 13.
 *
 * -DN64_TEXT_PAD=<bytes> resamples the layout without changing behaviour,
 * so a configuration can be measured two or three times and quoted with
 * an honest range instead of a number that happens to be a lucky
 * alignment.
 */
#define N64_STR2(x) #x
#define N64_STR(x) N64_STR2(x)
#ifdef N64_TEXT_PAD
/* Called once at init so --gc-sections keeps it.  The filler assembles to
 * zero words, which on MIPS is SLL $0,$0,0 -- nop -- so running through
 * it is harmless. */
__attribute__((noinline)) void n64_text_pad(void)
{
  __asm__ volatile(".space " N64_STR(N64_TEXT_PAD));
}
#endif

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
#ifdef N64_UNCACHED_SCREEN
  /* Experiment: address the GBA screen buffer uncached.
     It is 75 KB written by every scanline and read back by the blit, and a
     D-cache miss histogram puts it at ~50% of all misses while rendering
     (buckets 0x1b/0x1c -- anonymous because this is a function-local static
     that LTO privatises).  The N64 framebuffer costs nothing precisely
     because libdragon puts it in KSEG1, so the same treatment may pay here.
     RISK: the renderer does read-modify-write on the destination for
     stacked/blended pixels (render_tile_Nbpp STCKCOLOR reads *dest_ptr),
     and obj-blend is active on up to 80% of scanlines in this game, so
     uncached *reads* could easily cost more than the writes save.  Measure,
     do not assume. */
  data_cache_hit_writeback_invalidate(screen_buf, sizeof(screen_buf));
  gba_screen_pixels = (u16 *)UncachedAddr(screen_buf);
#endif

#ifdef N64_RDP_BENCH
  /* Must run BEFORE n64_rsp_init(): libdragon drives rdpq from the RSP
   * (rspq), and loading our own rsp_gbascan ucode overwrites rspq's, so
   * any rdpq call afterwards hangs in rspq_next_buffer waiting for a
   * command processor that is no longer there.  That conflict is a real
   * constraint on an RDP renderer, not just on this benchmark -- see
   * n64/n64_rdp_bench.c. */
  { extern void n64_rdp_selftest(void); n64_rdp_selftest(); }
  { extern void n64_rdp_bench(void); n64_rdp_bench(); }
#endif

  /* RSP: prove the offload path works before anything depends on it.
     Rendering is 85% compute, so the RSP is wanted as a second execution
     unit; this only checks that ucode load + DMA in/out + sync are sound. */
#ifdef N64_RDP_BG
  /* The RSP blit and the RDP renderer cannot coexist: libdragon drives
   * rdpq from the RSP -- rspq is a command processor running as RSP
   * ucode -- and rsp_gbascan overwrites it.  With the BG on the RDP the
   * blit goes back to the CPU, which now only has to convert the rows
   * the RDP declined. */
#else
  { extern void n64_rsp_init(void); extern bool n64_rsp_selftest(void);
    n64_rsp_init();
    n64_rsp_selftest();
    { extern void n64_rsp_bench(void); n64_rsp_bench(); } }
#endif



#ifdef N64_TEXT_PAD
  { extern void n64_text_pad(void); n64_text_pad(); }
#endif

  /* Say plainly whether the dynarec is live.  Inferring it from block
   * counters has misled this investigation twice: a build with the JIT
   * compiled in but disabled looks identical to one that is translating,
   * unless you happen to notice the translation counter is flat. */
  debugf("[gpSP]: dynarec_enable=%d\n", dynarec_enable);

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
#ifdef N64_EMUX_PROF
      u32 emux_emu_i0 = 0, emux_emu_d0 = 0, emux_unc0 = 0;
      u32 emux_imiss_emu = 0, emux_dmiss_emu = 0;
      u32 emux_imiss_blit = 0, emux_dmiss_blit = 0;
      u32 emux_unc_emu = 0, emux_unc_blit = 0;
#endif
      extern u32 prof_ppu_ticks;
      #define PROF_TICK() ({ u32 _t; __asm__ volatile("mfc0 %0, $9" : "=r"(_t)); _t; })

      while (emulator_running) {
        u32 t_frame_start = PROF_TICK();

        /* Poll input */
        n64_input_poll();
        n64_input_update();

#ifdef N64_EMUX_PROF
        /* ares's own cache model, read from inside the ROM.  Split the
         * same way the frame timers are, so a cache figure can be put
         * next to the milliseconds it is supposed to explain. */
        { emux_emu_i0 = (u32)EMUX_GLOBAL(EMUX_ICACHE_MISSES);
          emux_emu_d0 = (u32)EMUX_GLOBAL(EMUX_DCACHE_MISSES);
          emux_unc0   = (u32)EMUX_GLOBAL(EMUX_RDRAM_UNCACHED); }
#endif
        /* Run one GBA frame (CPU + PPU) */
        u32 t0 = PROF_TICK();
        run_frame();
        u32 t1 = PROF_TICK();
        prof_emu += t1 - t0;
#ifdef N64_EMUX_PROF
        { u32 im = (u32)EMUX_GLOBAL(EMUX_ICACHE_MISSES);
          u32 dm = (u32)EMUX_GLOBAL(EMUX_DCACHE_MISSES);
          u32 uc = (u32)EMUX_GLOBAL(EMUX_RDRAM_UNCACHED);
          emux_imiss_emu += im - emux_emu_i0;
          emux_dmiss_emu += dm - emux_emu_d0;
          emux_unc_emu   += uc - emux_unc0;
          emux_emu_i0 = im; emux_emu_d0 = dm; emux_unc0 = uc; }
#endif

        /* Output video (blit GBA framebuffer to N64 display) */
        u32 tb0 = PROF_TICK();
        if (!skip_next_frame) {
          n64_video_render_frame();
        }
        u32 tb1 = PROF_TICK();
        prof_blit += tb1 - tb0;
#ifdef N64_EMUX_PROF
        { u32 im = (u32)EMUX_GLOBAL(EMUX_ICACHE_MISSES);
          u32 dm = (u32)EMUX_GLOBAL(EMUX_DCACHE_MISSES);
          emux_imiss_blit += im - emux_emu_i0;
          emux_dmiss_blit += dm - emux_emu_d0;
          emux_unc_blit += (u32)EMUX_GLOBAL(EMUX_RDRAM_UNCACHED) - emux_unc0; }
#endif

#ifdef N64_AUDIO_OUT
        /* Push this frame's PCM.  The game's own m4a mixer produced it;
         * see n64/m4a_hle.c, which is what made running that mixer cheap
         * enough to be worth playing the result of. */
        n64_audio_render_frame();
#endif

        u32 t_frame_end = PROF_TICK();
        prof_total += t_frame_end - t_frame_start;
        prof_frames++;

        /* Print profiling every 60 frames */
        /* PROF window length.  60 emulated frames by default; override with
           -DPROF_FRAMES=N.  This matters for measuring the dynarec: the FPS
           reported here comes from the VR4300 COUNT register, i.e. *emulated*
           N64 time, so it is unaffected by how slowly the host emulator runs.
           But a guest JIT makes ares crawl in wall-clock (it keeps discarding
           its own recompilation of code we write and execute), so 60 emulated
           frames can take hours of real time.  A shorter window still yields
           a valid, directly comparable number. */
        #ifndef PROF_FRAMES
        #define PROF_FRAMES 60
        #endif
        if (prof_frames == PROF_FRAMES) {
          u32 emu_pct = (u32)(((u64)prof_emu * 100) / prof_total);
          u32 blit_pct = (u32)(((u64)prof_blit * 100) / prof_total);
          /* u64: prof_ppu_ticks reaches ~7.7e7 per window and *100 overflows u32,
             which reported rendering as 21% of emulation when the raw
             counters say 54%.  That single wrapped multiply is what made
             the frameskip ablation and the counters appear to disagree by
             ~19 ms/frame. */
          u32 ppu_pct = prof_emu ? (u32)(((u64)prof_ppu_ticks * 100) / prof_emu) : 0;
          u32 cpu_pct = 100 - ppu_pct;  /* CPU = emulation minus PPU */
          u32 ms_per_frame = prof_total / (46875 * PROF_FRAMES);
          extern u32 prof_arm_insns, prof_thumb_insns;
          extern u32 prof_idle_hits, prof_idle_detect_fires;
          u32 total_insns = prof_arm_insns + prof_thumb_insns;
          u32 total_ms = ms_per_frame * 60;
          u32 kips = total_ms ? (total_insns / total_ms) : 0;
          u32 cyc_per_insn = total_insns ? (u32)((u64)prof_total * 2 / total_insns) : 0;
          u32 arm_pct = total_insns ? (prof_arm_insns   * 100) / total_insns : 0;
          u32 thm_pct = total_insns ? (prof_thumb_insns * 100) / total_insns : 0;
          /* Raw counters so the accounting can be checked end to end.
             Two methods disagreed on what rendering costs: a frameskip
             ablation said render+blit ~34 ms/frame while these counters
             implied ~15 ms.  prof_total is the whole frame, prof_emu is
             run_frame(), prof_blit is n64_video_render_frame(), and
             prof_ppu_ticks (inside update_gba) should be the scanline
             rendering within prof_emu.  If emu+blit falls short of total,
             the missing time is in the frame loop itself -- input polling,
             or display_get()/display_show() blocking on vsync. */
#ifdef N64_JIT_IBPROF
          /* Indirect-branch inline cache: hits never reach C, misses do.
           * 489 C-level block lookups a frame is the suspect for the
           * emulation time the yield accounting cannot explain. */
          { extern u32 ibcache_hits; extern u32 prof_jit_hit;
            static u32 ph, pm;
            u32 h = ibcache_hits - ph, m = prof_jit_hit - pm;
            ph = ibcache_hits; pm = prof_jit_hit;
            debugf("PROF:  ibcache: %lu hit + %lu miss per frame (%lu%% hit)\n",
                   (unsigned long)(h / PROF_FRAMES),
                   (unsigned long)(m / PROF_FRAMES),
                   (unsigned long)((h + m) ? h * 100 / (h + m) : 0)); }
#endif
#ifdef N64_EVENT_PROF
          /* Which scheduler shortens the CPU window, and how often.  All
           * of the emulator's CPU time is per-event overhead in this
           * scene, so the mix here is what decides whether that overhead
           * can be reduced at all. */
          { extern u32 prof_ev_video, prof_ev_serial, prof_ev_dma,
                   prof_ev_timer[4], prof_ev_calls;
            debugf("PROF:  event: %lu calls/frame | video %lu%% serial %lu%%"
                   " dma %lu%% timer %lu/%lu/%lu/%lu%%\n",
                   (unsigned long)(prof_ev_calls / PROF_FRAMES),
                   (unsigned long)(prof_ev_video * 100 / (prof_ev_calls | 1)),
                   (unsigned long)(prof_ev_serial * 100 / (prof_ev_calls | 1)),
                   (unsigned long)(prof_ev_dma * 100 / (prof_ev_calls | 1)),
                   (unsigned long)(prof_ev_timer[0] * 100 / (prof_ev_calls | 1)),
                   (unsigned long)(prof_ev_timer[1] * 100 / (prof_ev_calls | 1)),
                   (unsigned long)(prof_ev_timer[2] * 100 / (prof_ev_calls | 1)),
                   (unsigned long)(prof_ev_timer[3] * 100 / (prof_ev_calls | 1)));
            prof_ev_calls = prof_ev_video = prof_ev_serial = prof_ev_dma = 0;
            prof_ev_timer[0] = prof_ev_timer[1] = 0;
            prof_ev_timer[2] = prof_ev_timer[3] = 0; }
#endif
                    debugf("PROF:  raw: total=%lu emu=%lu blit=%lu ppu=%lu"
                 " | emu+blit=%lu (%lu%% of total)\n",
                 (unsigned long)prof_total, (unsigned long)prof_emu,
                 (unsigned long)prof_blit, (unsigned long)prof_ppu_ticks,
                 (unsigned long)(prof_emu + prof_blit),
                 (unsigned long)(prof_total ? ((u64)(prof_emu + prof_blit) * 100) / prof_total : 0));
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
            extern u32 prof_snd_ticks;
            u32 upd_ex_ppu = prof_update_ticks > prof_ppu_ticks
                           ? prof_update_ticks - prof_ppu_ticks : 0;
            u32 accounted  = prof_update_ticks + prof_aot_ticks;
            u32 interp     = prof_emu > accounted ? prof_emu - accounted : 0;
            u32 e = prof_emu ? prof_emu : 1;
            debugf("PROF:  cyc: interp %lu%% aot %lu%% event %lu%% ppu %lu%%"
                   " snd %lu%% | yields/frame %lu | aot-calls/f %lu gba-cyc/f %luK\n",
                   (unsigned long)((u64)interp       * 100 / e),
                   (unsigned long)((u64)prof_aot_ticks * 100 / e),
                   (unsigned long)((u64)upd_ex_ppu   * 100 / e),
                   (unsigned long)((u64)prof_ppu_ticks * 100 / e),
                   (unsigned long)((u64)prof_snd_ticks * 100 / e),
                   (unsigned long)(prof_update_calls / PROF_FRAMES),
                   (unsigned long)(prof_aot_hits / PROF_FRAMES),
                   (unsigned long)(prof_aot_gba_cycles / PROF_FRAMES / 1000));
            /* These two were never reset, so "aot-calls" was a running
             * total since boot and every per-call figure derived from it
             * was meaningless. */
            prof_aot_hits = prof_aot_gba_cycles = 0;
            prof_update_ticks = prof_update_calls = prof_aot_ticks = 0;
            prof_snd_ticks = 0;
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

#ifdef PROFILE_RASTER
          { extern u32 prof_raster_dirty_lines, prof_raster_frames, prof_raster_worst;
            u32 fr = prof_raster_frames ? prof_raster_frames : 1;
            extern u32 prof_raster_mem_dirty;
            debugf("PROF:  raster: %lu frames, %lu dirty lines, worst %lu/160 | vram+pal+oam changed mid-frame in %lu frames\n",
                   (unsigned long)prof_raster_frames,
                   (unsigned long)prof_raster_dirty_lines,
                   (unsigned long)prof_raster_worst,
                   (unsigned long)prof_raster_mem_dirty);
            prof_raster_mem_dirty = 0;
            prof_raster_dirty_lines = prof_raster_frames = prof_raster_worst = 0; }
#endif

#ifdef N64_EMUX_PROF
          {
            /* 48 cycles per line fill in ares's model, so the misses
             * convert straight into milliseconds and can be compared with
             * the frame time they are meant to explain. */
            u32 f = PROF_FRAMES;
            u32 icyc = (emux_imiss_emu + emux_imiss_blit) / f * 48;
            u32 dcyc = (emux_dmiss_emu + emux_dmiss_blit) / f * 48;
            debugf("PROF:  cache: I-miss/frame emu %lu blit %lu = %lu.%02lu ms;"
                   " D-miss/frame emu %lu blit %lu = %lu.%02lu ms;"
                   " uncached RDRAM emu %lu blit %lu\n",
                   (unsigned long)(emux_imiss_emu / f),
                   (unsigned long)(emux_imiss_blit / f),
                   (unsigned long)(icyc / 93750), (unsigned long)((icyc % 93750) * 100 / 93750),
                   (unsigned long)(emux_dmiss_emu / f),
                   (unsigned long)(emux_dmiss_blit / f),
                   (unsigned long)(dcyc / 93750), (unsigned long)((dcyc % 93750) * 100 / 93750),
                   (unsigned long)(emux_unc_emu / f),
                   (unsigned long)(emux_unc_blit / f));
            emux_imiss_emu = emux_dmiss_emu = 0;
            emux_imiss_blit = emux_dmiss_blit = 0;
            emux_unc_emu = emux_unc_blit = 0;
          }
#endif
#ifdef N64_VIDEO_PROBE
          { extern u32 prof_hb_irq, prof_hb_dma, prof_hb_calls;
            u32 c = prof_hb_calls ? prof_hb_calls : 1;
            debugf("PROF:  video: %lu update_gba calls, hblank IRQ armed %lu%%,"
                   " hblank DMA armed %lu%%\n",
                   (unsigned long)(prof_hb_calls / PROF_FRAMES),
                   (unsigned long)(prof_hb_irq * 100 / c),
                   (unsigned long)(prof_hb_dma * 100 / c));
            prof_hb_irq = prof_hb_dma = prof_hb_calls = 0; }
#endif
#ifdef N64_RDP_BG
          {
            extern u32 prof_rdpbg_rows, prof_rdpbg_frames, prof_rdpbg_break;
            extern u32 prof_rdpbg_blank;
            extern u32 n64_rdpbg_slices, n64_rdpbg_groups, n64_rdpbg_tiles;
            extern u32 n64_rdpbg_tluts;
            extern u32 prof_rdpbg_spr, prof_rdpbg_sprno;
            extern u32 n64_rdpbg_frames, n64_rdpbg_overflow;
            u32 f = prof_rdpbg_frames ? prof_rdpbg_frames : 1;
            u32 g = n64_rdpbg_frames ? n64_rdpbg_frames : 1;
            debugf("PROF:  rdpbg: %lu frames drew %lu rows each (%lu%% of screen);"
                   " %lu tiles, %lu TMEM slices, %lu palette groups per frame;"
                   " %lu register breaks, %lu overflows\n",
                   (unsigned long)prof_rdpbg_frames,
                   (unsigned long)(prof_rdpbg_rows / f),
                   (unsigned long)(prof_rdpbg_rows / f * 100 / 160),
                   (unsigned long)(n64_rdpbg_tiles / g),
                   (unsigned long)(n64_rdpbg_slices / g),
                   (unsigned long)(n64_rdpbg_groups / g),
                   (unsigned long)prof_rdpbg_break,
                   (unsigned long)n64_rdpbg_overflow);
            { u32 st = prof_rdpbg_spr + prof_rdpbg_sprno;
              debugf("PROF:  rdpbg: sprites %lu drawn, %lu refused (%lu%%);"
                     " %lu TLUT swaps per frame\n",
                     (unsigned long)(prof_rdpbg_spr / f),
                     (unsigned long)(prof_rdpbg_sprno / f),
                     (unsigned long)(st ? prof_rdpbg_sprno * 100 / st : 0),
                     (unsigned long)(n64_rdpbg_tluts / g));
              prof_rdpbg_spr = prof_rdpbg_sprno = n64_rdpbg_tluts = 0; }
            { debugf("PROF:  rdpbg: %lu blank tiles skipped per frame\n",
                     (unsigned long)(prof_rdpbg_blank / g));
              prof_rdpbg_blank = 0; }
            { extern u32 prof_rdpbg_why[8];
              debugf("PROF:  rdpbg: refused frames: mode%lu window%lu fx%lu"
                     " nolayer%lu 8bpp%lu mosaic%lu objonly%lu | accepted %lu\n",
                     (unsigned long)prof_rdpbg_why[0], (unsigned long)prof_rdpbg_why[1],
                     (unsigned long)prof_rdpbg_why[2], (unsigned long)prof_rdpbg_why[3],
                     (unsigned long)prof_rdpbg_why[4], (unsigned long)prof_rdpbg_why[5],
                     (unsigned long)prof_rdpbg_why[6], (unsigned long)prof_rdpbg_why[7]);
              memset(prof_rdpbg_why, 0, sizeof(prof_rdpbg_why)); }
            { extern u32 prof_rdpbg_win[6];
              debugf("PROF:  rdpbg: window state: dispcnt%04lx win0h=%04lx"
                     " win0v=%04lx win1h=%04lx winin=%04lx winout=%04lx\n",
                     (unsigned long)prof_rdpbg_win[0], (unsigned long)prof_rdpbg_win[1],
                     (unsigned long)prof_rdpbg_win[2], (unsigned long)prof_rdpbg_win[3],
                     (unsigned long)prof_rdpbg_win[4], (unsigned long)prof_rdpbg_win[5]); }
            { extern u32 n64_rdpbg_t_sort, n64_rdpbg_t_emit, n64_rdpbg_t_wait;
              /* COUNT ticks at CPU/2, so x2 gives cycles; /93750 gives ms. */
              debugf("PROF:  rdpbg: per frame sort %lu.%02lu ms, emit %lu.%02lu ms,"
                     " wait-for-RDP %lu.%02lu ms\n",
                     (unsigned long)(n64_rdpbg_t_sort * 2 / g / 93750),
                     (unsigned long)((n64_rdpbg_t_sort * 2 / g % 93750) * 100 / 93750),
                     (unsigned long)(n64_rdpbg_t_emit * 2 / g / 93750),
                     (unsigned long)((n64_rdpbg_t_emit * 2 / g % 93750) * 100 / 93750),
                     (unsigned long)(n64_rdpbg_t_wait * 2 / g / 93750),
                     (unsigned long)((n64_rdpbg_t_wait * 2 / g % 93750) * 100 / 93750));
              n64_rdpbg_t_sort = n64_rdpbg_t_emit = n64_rdpbg_t_wait = 0; }
            { extern u32 n64_rdpbg_t_acquire, n64_rdpbg_t_show;
              debugf("PROF:  rdpbg: per frame display_get %lu.%02lu ms,"
                     " display_show %lu.%02lu ms\n",
                     (unsigned long)(n64_rdpbg_t_acquire * 2 / g / 93750),
                     (unsigned long)((n64_rdpbg_t_acquire * 2 / g % 93750) * 100 / 93750),
                     (unsigned long)(n64_rdpbg_t_show * 2 / g / 93750),
                     (unsigned long)((n64_rdpbg_t_show * 2 / g % 93750) * 100 / 93750));
              n64_rdpbg_t_acquire = n64_rdpbg_t_show = 0; }
            prof_rdpbg_rows = prof_rdpbg_frames = prof_rdpbg_break = 0;
            n64_rdpbg_slices = n64_rdpbg_groups = n64_rdpbg_tiles = 0;
            n64_rdpbg_frames = n64_rdpbg_overflow = 0;
          }
#endif
#ifdef N64_RDPGATE
          {
            extern u32 prof_gate_lines, prof_gate_pass, prof_gate_band;
            extern u32 prof_gate_frames, prof_gate_wholeframe;
            u32 gl = prof_gate_lines ? prof_gate_lines : 1;
            u32 gf = prof_gate_frames ? prof_gate_frames : 1;
            debugf("PROF:  rdpgate: %lu%% of lines are BG-only tiled "
                   "(%lu of %lu); %lu%% sit in a stable 8-line tile band; "
                   "%lu%% of frames are band-drawable end to end\n",
                   (unsigned long)(prof_gate_pass * 100 / gl),
                   (unsigned long)prof_gate_pass, (unsigned long)prof_gate_lines,
                   (unsigned long)(prof_gate_band * 100 / gl),
                   (unsigned long)(prof_gate_wholeframe * 100 / gf));
            prof_gate_lines = prof_gate_pass = prof_gate_band = 0;
            prof_gate_frames = prof_gate_wholeframe = 0;
          }
          {
            extern u32 prof_slice_frames, prof_slice_draws;
            extern u32 prof_slice_loads, prof_slice_pairs;
            u32 sf = prof_slice_frames ? prof_slice_frames : 1;
            u32 draws = prof_slice_draws / sf;
            u32 loads = prof_slice_loads / sf;
            u32 pairs = prof_slice_pairs / sf;
            /* 1725 cyc per 32-tile TMEM slice, 195 cyc per textured rect,
             * both measured; set_tile is assumed ~120 until measured. */
            u32 cyc = loads * 1725 + pairs * 120 + draws * 195;
            debugf("PROF:  rdpgate: per frame %lu tile draws, %lu TMEM slices,"
                   " %lu (slice,palette) pairs -> ~%lu cyc = %lu.%02lu ms CPU\n",
                   (unsigned long)draws, (unsigned long)loads,
                   (unsigned long)pairs, (unsigned long)cyc,
                   (unsigned long)(cyc / 93750),
                   (unsigned long)((cyc % 93750) * 100 / 93750));
            prof_slice_frames = prof_slice_draws = 0;
            prof_slice_loads = prof_slice_pairs = 0;
          }
#endif
#ifdef PROFILE_PPU2
          {
            extern u32 prof_ppu2_effect[4], prof_ppu2_mode[8];
            extern u32 prof_ppu2_layers[8], prof_ppu2_objblend, prof_ppu2_calls;
            u32 c = prof_ppu2_calls ? prof_ppu2_calls : 1;
            debugf("PROF:  ppu2: calls=%lu effect none%lu%% bright%lu%% dark%lu%% blend%lu%%"
                   " | mode0=%lu%% mode1=%lu%% mode2=%lu%% mode4=%lu%%"
                   " | layers 1=%lu%% 2=%lu%% 3=%lu%% 4=%lu%% | objblend=%lu%%\n",
                   (unsigned long)prof_ppu2_calls,
                   (unsigned long)(prof_ppu2_effect[0]*100/c),
                   (unsigned long)(prof_ppu2_effect[1]*100/c),
                   (unsigned long)(prof_ppu2_effect[2]*100/c),
                   (unsigned long)(prof_ppu2_effect[3]*100/c),
                   (unsigned long)(prof_ppu2_mode[0]*100/c),
                   (unsigned long)(prof_ppu2_mode[1]*100/c),
                   (unsigned long)(prof_ppu2_mode[2]*100/c),
                   (unsigned long)(prof_ppu2_mode[4]*100/c),
                   (unsigned long)(prof_ppu2_layers[1]*100/c),
                   (unsigned long)(prof_ppu2_layers[2]*100/c),
                   (unsigned long)(prof_ppu2_layers[3]*100/c),
                   (unsigned long)(prof_ppu2_layers[4]*100/c),
                   (unsigned long)(prof_ppu2_objblend*100/c));
            { extern u32 prof_ppu2_fastok, prof_ppu2_fastno;
              u32 ft = prof_ppu2_fastok + prof_ppu2_fastno; if (!ft) ft = 1;
              debugf("PROF:  ppu2: RSP BG fast path applicable on %lu%% of scanlines"
                     " (%lu of %lu)\n",
                     (unsigned long)(prof_ppu2_fastok*100/ft),
                     (unsigned long)prof_ppu2_fastok, (unsigned long)ft);
              prof_ppu2_fastok = prof_ppu2_fastno = 0; }
            { extern u32 prof_ppu2_objlast, prof_ppu2_objmid, prof_ppu2_noobj;
              u32 tt = prof_ppu2_objlast + prof_ppu2_objmid + prof_ppu2_noobj;
              if (!tt) tt = 1;
              debugf("PROF:  ppu2: layer order -- no obj %lu%%, obj last %lu%%,"
                     " obj interleaved %lu%% (batchable = first two)\n",
                     (unsigned long)(prof_ppu2_noobj*100/tt),
                     (unsigned long)(prof_ppu2_objlast*100/tt),
                     (unsigned long)(prof_ppu2_objmid*100/tt));
              prof_ppu2_objlast = prof_ppu2_objmid = prof_ppu2_noobj = 0; }
            { extern u32 prof_ppu2_4bpp, prof_ppu2_8bpp;
              u32 bt = prof_ppu2_4bpp + prof_ppu2_8bpp;
              debugf("PROF:  ppu2: tiled layer draws 4bpp=%lu 8bpp=%lu (%lu%% 4bpp)\n",
                     (unsigned long)prof_ppu2_4bpp, (unsigned long)prof_ppu2_8bpp,
                     (unsigned long)(bt ? prof_ppu2_4bpp*100/bt : 0));
              prof_ppu2_4bpp = prof_ppu2_8bpp = 0; }
            for (int _i = 0; _i < 4; _i++) prof_ppu2_effect[_i] = 0;
            for (int _i = 0; _i < 8; _i++) { prof_ppu2_mode[_i] = 0; prof_ppu2_layers[_i] = 0; }
            prof_ppu2_objblend = prof_ppu2_calls = 0;
          }
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
            /* u64 for the ratio: prof_ppu_render_ticks reaches ~6e7 per
               window, and x100 overflows u32 (max 4.29e9).  That overflow is
               what produced the nonsense 4%/14%/29% render figures; the raw
               counts below show render is consistently ~93%. */
            u64 ppu = prof_ppu_ticks ? prof_ppu_ticks : 1;
            /* Raw ticks too: the percentages did not add up (render read 91%
               in one window and 4% in others with everything else at 0-3%),
               so print the numerator and denominator and find out why. */
            debugf("PROF:  ppuraw: total=%lu sort=%lu lo=%lu render=%lu blank=%lu aff=%lu\n",
                   (unsigned long)prof_ppu_ticks,
                   (unsigned long)prof_ppu_obj_sort_ticks,
                   (unsigned long)prof_ppu_layer_order_ticks,
                   (unsigned long)prof_ppu_render_ticks,
                   (unsigned long)prof_ppu_blank_ticks,
                   (unsigned long)prof_ppu_affine_ticks);
            debugf("PROF:  ppu: sort%lu%% lo%lu%% render%lu%% blank%lu%% aff%lu%% | sorts=%lu blanks=%lu\n",
                   (unsigned long)(((u64)prof_ppu_obj_sort_ticks * 100) / ppu),
                   (unsigned long)(((u64)prof_ppu_layer_order_ticks * 100) / ppu),
                   (unsigned long)(((u64)prof_ppu_render_ticks * 100) / ppu),
                   (unsigned long)(((u64)prof_ppu_blank_ticks * 100) / ppu),
                   (unsigned long)(((u64)prof_ppu_affine_ticks * 100) / ppu),
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

            /* Top-5 hot pages, plus a wider list below.  Five was enough
             * to pick the first AOT targets; choosing the next tranche
             * needs to see where the tail actually is. */
            #define PROF_PG_N 20
            u32 pg_idx[PROF_PG_N] = {0};
            u32 pg_cnt[PROF_PG_N] = {0};
            for (u32 i = 0; i < 6144; i++) {
              u32 c = prof_page_hist[i];
              if (c == 0) continue;
              for (u32 j = 0; j < PROF_PG_N; j++) {
                if (c > pg_cnt[j]) {
                  for (u32 k = PROF_PG_N - 1; k > j; k--) {
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

            /* The whole ranked list, in a form tools/suggest_targets.py
             * can read: page address, then share of interpreted
             * instructions in tenths of a percent. */
            for (u32 j = 0; j < PROF_PG_N; j += 5)
              debugf("PROF:  aot-pgN %lu: 0x%05lx:%lu 0x%05lx:%lu 0x%05lx:%lu"
                     " 0x%05lx:%lu 0x%05lx:%lu\n", (unsigned long)j,
                     (unsigned long)((pg_idx[j+0]+0x8000)*0x1000), (unsigned long)((pg_cnt[j+0]*1000)/ti),
                     (unsigned long)((pg_idx[j+1]+0x8000)*0x1000), (unsigned long)((pg_cnt[j+1]*1000)/ti),
                     (unsigned long)((pg_idx[j+2]+0x8000)*0x1000), (unsigned long)((pg_cnt[j+2]*1000)/ti),
                     (unsigned long)((pg_idx[j+3]+0x8000)*0x1000), (unsigned long)((pg_cnt[j+3]*1000)/ti),
                     (unsigned long)((pg_idx[j+4]+0x8000)*0x1000), (unsigned long)((pg_cnt[j+4]*1000)/ti));

            /* Instruction-level view of one page.
             *
             * Page 0x08000000 is 33.7% of all interpreted instructions --
             * more than the next six pages together -- and it is the one
             * page that must never be AOT-translated, because an AOT
             * function whose internal loop covers idle_loop_target_pc
             * (0x080008CE) never returns to the interpreter and the
             * idle-skip fast path can never fire.  So the question is
             * what *else* is on it, and whether that can be reached
             * another way.  -DN64_PCHIST_PAGE=0x8000 answers it. */
#ifdef N64_PCHIST_PAGE
            { extern u32 prof_pc_hist[], prof_pc_hist_page;
              u32 hi[8] = {0}, hc[8] = {0}, i2, j2, k2;
              prof_pc_hist_page = N64_PCHIST_PAGE;
              for (i2 = 0; i2 < 2048; i2++) {
                u32 c = prof_pc_hist[i2];
                if (!c) continue;
                for (j2 = 0; j2 < 8; j2++)
                  if (c > hc[j2]) {
                    for (k2 = 7; k2 > j2; k2--) { hc[k2] = hc[k2-1]; hi[k2] = hi[k2-1]; }
                    hc[j2] = c; hi[j2] = i2; break;
                  }
              }
              debugf("PROF:  pc-hist 0x%05lx000: %04lx:%lu %04lx:%lu %04lx:%lu %04lx:%lu"
                     " %04lx:%lu %04lx:%lu %04lx:%lu %04lx:%lu\n",
                     (unsigned long)prof_pc_hist_page,
                     (unsigned long)(hi[0]*2), (unsigned long)hc[0],
                     (unsigned long)(hi[1]*2), (unsigned long)hc[1],
                     (unsigned long)(hi[2]*2), (unsigned long)hc[2],
                     (unsigned long)(hi[3]*2), (unsigned long)hc[3],
                     (unsigned long)(hi[4]*2), (unsigned long)hc[4],
                     (unsigned long)(hi[5]*2), (unsigned long)hc[5],
                     (unsigned long)(hi[6]*2), (unsigned long)hc[6],
                     (unsigned long)(hi[7]*2), (unsigned long)hc[7]);
              memset(prof_pc_hist, 0, sizeof(u32) * 2048); }
#endif

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
