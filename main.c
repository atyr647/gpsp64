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

#include "common.h"
#include "n64/m4a_hle.h"
#include <ctype.h>

/* CPU vs PPU profiling (N64 COUNT register).  Gated on __mips__ (not just
 * N64) so a native x86-64 build of the portable core — e.g. the AOT
 * debugging harness in native/ — can still define N64 (for AOT dispatch)
 * without pulling in the MIPS-only mfc0 instruction. */
#if defined(N64) && defined(__mips__)
  #define PROF_TICK() ({ u32 _t; __asm__ volatile("mfc0 %0, $9" : "=r"(_t)); _t; })
  u32 prof_ppu_ticks = 0;
  /* Cycle attribution, -DPROFILE_CYCLES only.  prof_emu (whole frame of
   * emulation) minus these tells us how much is left for the actual
   * instruction interpretation, which is the number that decides which
   * optimisations are worth anything. */
  u32 prof_update_ticks = 0;   /* inside update_gba, PPU render included */
  u32 prof_update_calls = 0;   /* timeslice yields -- one per hw event */
  u32 prof_snd_ticks = 0;      /* gpSP's own PSG synthesis + DirectSound */
#else
  #define PROF_TICK() 0
  #ifdef N64
    u32 prof_ppu_ticks = 0;
  #endif
#endif

timer_type timer[4];

u32 frame_counter = 0;
u32 cpu_ticks = 0;
u32 execute_cycles = 0;
#ifdef N64_EVENT_PROF
u32 prof_ev_video, prof_ev_serial, prof_ev_dma, prof_ev_timer[4],
    prof_ev_calls;
#endif
#ifdef N64_VIDEO_PROBE
u32 prof_hb_irq = 0, prof_hb_dma = 0, prof_hb_calls = 0;
#endif
s32 video_count = 0;

u32 last_frame = 0;
u32 flush_ram_count = 0;
u32 gbc_update_count = 0;
u32 oam_update_count = 0;

char main_path[512];

static u32 random_state = 0;

// Generate 16 random bits.
u16 rand_gen() {
  random_state = ((random_state * 1103515245) + 12345) & 0x7fffffff;
  return random_state;
}

// Add some random state to the initial seed.
void rand_seed(u32 data) {
  random_state ^= rand_gen() ^ data;
}


static unsigned update_timers(irq_type *irq_raised, unsigned completed_cycles)
{
   unsigned i, ret = 0;
   for (i = 0; i < 4; i++)
   {
      if(timer[i].status == TIMER_INACTIVE)
         continue;

      if(timer[i].status != TIMER_CASCADE)
      {
         timer[i].count -= completed_cycles;
         /* io_registers accessors range: REG_TM0D, REG_TM1D, REG_TM2D, REG_TM3D */
         write_ioreg(REG_TMXD(i), -(timer[i].count >> timer[i].prescale));
      }

      if(timer[i].count > 0)
         continue;

#ifdef N64_TIMER_BATCH
      /* With the window no longer clamped to this timer's next overflow
         (see the scheduler below), more than one can fall inside a single
         call.  Service each of them; the samples handed to sound_timer are
         the same ones, just produced in a burst rather than one per
         yield. */
      do {
#endif
      /* irq_raised value range: IRQ_TIMER0, IRQ_TIMER1, IRQ_TIMER2, IRQ_TIMER3 */
      if(timer[i].irq)
         *irq_raised |= (IRQ_TIMER0 << i);

      if((i != 3) && (timer[i + 1].status == TIMER_CASCADE))
      {
         timer[i + 1].count--;
         write_ioreg(REG_TMXD(i + 1), -timer[i+1].count);
      }

      if(i < 2)
      {
         if(timer[i].direct_sound_channels & 0x01)
#ifdef PROFILE_CYCLES
            { u32 _ss = PROF_TICK();
              ret += sound_timer(timer[i].frequency_step, 0);
              prof_snd_ticks += PROF_TICK() - _ss; }
#else
            ret += sound_timer(timer[i].frequency_step, 0);
#endif

         if(timer[i].direct_sound_channels & 0x02)
#ifdef PROFILE_CYCLES
            { u32 _ss = PROF_TICK();
              ret += sound_timer(timer[i].frequency_step, 1);
              prof_snd_ticks += PROF_TICK() - _ss; }
#else
            ret += sound_timer(timer[i].frequency_step, 1);
#endif
      }

      timer[i].count += (timer[i].reload << timer[i].prescale);
#ifdef N64_TIMER_BATCH
      /* A zero reload would never lift count back above zero: bail rather
         than spin. */
      } while (timer[i].count <= 0 && (timer[i].reload << timer[i].prescale));
#endif
   }
   return ret;
}

void init_main(void)
{
  u32 i;
  for(i = 0; i < 4; i++)
  {
    timer[i].status = TIMER_INACTIVE;
    timer[i].prescale = 0;
    timer[i].irq = 0;
    timer[i].reload = timer[i].count = 0x10000;
    timer[i].direct_sound_channels = TIMER_DS_CHANNEL_NONE;
    timer[i].frequency_step = 0;
  }

  timer[0].direct_sound_channels = TIMER_DS_CHANNEL_BOTH;
  timer[1].direct_sound_channels = TIMER_DS_CHANNEL_NONE;

  frame_counter = 0;
  cpu_ticks = 0;
  execute_cycles = 960;
  video_count = 960;

#ifdef HAVE_DYNAREC
  init_dynarec_caches();
  init_emitter(gamepak_must_swap());
#endif
}

u32 function_cc update_gba(int remaining_cycles)
{
#ifdef PROFILE_CYCLES
  u32 _upd_t0 = PROF_TICK();
  prof_update_calls++;
#endif
#ifdef PROFILE_EFF
  /* Frame-independent efficiency metric: N64 cycles spent per GBA cycle
     emulated.  Full speed is 5.56 (93.75 MHz host / 16.78 MHz GBA).
     Unlike the FPS report this needs no frame boundary, so it yields a
     valid number after a fraction of a frame -- which is what makes
     dynarec iteration practical, since the dynarec makes the host crawl
     in wall-clock even though emulated time advances normally. */
  { static u32 _n = 0, _t0 = 0, _c0 = 0;
    /* 400 was far too coarse for the dynarec: it manages only a few hundred
       update_gba calls in an entire run, and with the first sample used to
       prime the interval that needs 800 before printing anything.  50 gives
       a reading within seconds on either path. */
    if ((++_n % 50) == 0) {
      u32 t = PROF_TICK(), c = cpu_ticks;
      if (_t0) {
        u32 dt = (t - _t0) * 2;          /* COUNT is CPU/2 */
        u32 dc = c - _c0;
        if (dc)
          fprintf(stderr, "EFF %lu.%02lu N64cyc per GBAcyc (target 5.56)\n",
                  (unsigned long)(dt / dc),
                  (unsigned long)(((u64)(dt % dc) * 100) / dc));
      }
      /* Re-sample AFTER printing: an ISViewer fprintf costs real emulated
         cycles under ares, and folding it into the next interval inflated
         the reading roughly tenfold (106 vs an expected ~9). */
      _t0 = PROF_TICK(); _c0 = cpu_ticks;
    } }
#endif
#ifdef N64_TIME_TRACE
  /* Is emulated time advancing at all?  If the JIT is merely slow, vcount
     still cycles 0..227 and frame_counter climbs.  If it is spinning
     without ever reaching a hardware event, both sit still. */
  { static u32 _n = 0;
    /* Dense at the start, sparse afterwards.  A stall shows up as the
     * trace simply stopping, so the first N calls must be logged one by
     * one or the stop point is only known to within 50 calls. */
#ifndef N64_TIME_TRACE_DENSE
#define N64_TIME_TRACE_DENSE 3000
#endif
    if (++_n <= N64_TIME_TRACE_DENSE || (_n % 50) == 0)
    { extern u32 prof_icache_ticks, prof_icache_calls, prof_icache_work;
      { extern u32 prof_jit_xlat, prof_jit_hit, prof_jit_flush;
        extern u32 prof_jit_chainlen, prof_jit_chainmax;
        extern u32 prof_jit_badregion, prof_jit_badpc;
        fprintf(stderr, "JIT xlat=%lu hit=%lu flush=%lu chainmax=%lu runaway=%lu\n",
                (unsigned long)prof_jit_xlat, (unsigned long)prof_jit_hit,
                (unsigned long)prof_jit_flush,
                (unsigned long)prof_jit_chainlen,
                (unsigned long)prof_jit_chainmax);
        fprintf(stderr, "JIT badregion=%lu lastbadpc=%08lx\n",
                (unsigned long)prof_jit_badregion,
                (unsigned long)prof_jit_badpc); }
      /* COUNT alongside cpu_ticks, so N64 cycles per GBA cycle can be
       * computed from two points of this trace rather than trusted from
       * the EFF metric, which disagrees with the observed rate by four
       * orders of magnitude. */
      { u32 _cnt; __asm__ volatile("mfc0 %0, $9" : "=r"(_cnt));
        fprintf(stderr, "COUNT %lu\n", (unsigned long)_cnt); }
      /* Where is the emulated CPU?  With the JIT hanging from a savestate
       * while emulated time keeps advancing, the ARM PC and halt state
       * say more than any counter: they distinguish "spinning in game
       * code" from "halted waiting for an interrupt that never arrives"
       * from "jumped somewhere impossible". */
      fprintf(stderr, "ARM pc=%08lx cpsr=%08lx halt=%lu mode=%lu\n",
              (unsigned long)reg[REG_PC], (unsigned long)reg[REG_CPSR],
              (unsigned long)reg[CPU_HALT_STATE], (unsigned long)reg[CPU_MODE]);
      fprintf(stderr, "TIME upd=%lu vcount=%lu frame=%lu ticks=%lu"
                      " | icache calls=%lu work=%lu ticks=%luK\n",
              (unsigned long)_n, (unsigned long)read_ioreg(REG_VCOUNT),
              (unsigned long)frame_counter, (unsigned long)cpu_ticks,
              (unsigned long)prof_icache_calls,
              (unsigned long)prof_icache_work,
              (unsigned long)(prof_icache_ticks / 1000)); } }
#endif
  u32 changed_pc = 0;
  u32 frame_complete = 0;
  irq_type irq_raised = IRQ_NONE;
  int dma_cycles;
  trace_update_gba(remaining_cycles);

  remaining_cycles = MAX(remaining_cycles, -64);

  do
  {
    unsigned i;
    // Number of cycles we ask to run - cycles that we did not execute
    // (remaining_cycles can be negative and should be close to zero)
    unsigned completed_cycles = execute_cycles - remaining_cycles;
    cpu_ticks += completed_cycles;

    remaining_cycles = 0;

    // Timers can trigger DMA (usually sound) and consume cycles
    dma_cycles = update_timers(&irq_raised, completed_cycles);
    // Check for serial port IRQs as well.
    if (update_serial(completed_cycles))
      irq_raised |= IRQ_SERIAL;

#ifdef N64_VIDEO_PROBE
    /* Is the HBlank half of each scanline's video event doing anything?
     *
     * The emulator yields out of the interpreter 672 times a frame, and
     * with only ~24,000 GBA instructions executed per frame that works
     * out at ~2,200 host cycles per yield -- the transitions, not the
     * work between them, are what the emulation half costs.  456 of
     * those yields are video events, two per scanline.  If neither an
     * HBlank IRQ nor an HBlank DMA is ever armed, the HBlank boundary
     * does not need to be a scheduling point at all and the two halves
     * of a scanline could be one 1232-cycle event. */
    { extern u32 prof_hb_irq, prof_hb_dma, prof_hb_calls;
      u32 _i; prof_hb_calls++;
      if (read_ioreg(REG_DISPSTAT) & 0x10) prof_hb_irq++;
      for (_i = 0; _i < 4; _i++)
        if (dma[_i].start_type == DMA_START_HBLANK) { prof_hb_dma++; break; } }
#endif
    // Video count tracks the video cycles remaining until the next event
    video_count -= completed_cycles;

    // Ran out of cycles, move to the next video area
    if(video_count <= 0)
    {
      u32 vcount = read_ioreg(REG_VCOUNT);
      u32 dispstat = read_ioreg(REG_DISPSTAT);

#ifdef N64_MERGE_HBLANK
      /* Run both halves of the scanline as one event when the HBlank
       * boundary is not observable.
       *
       * The emulator leaves the interpreter 672 times a frame and only
       * executes ~24,000 GBA instructions in between -- the game spends
       * most of its frame in a five-instruction idle loop -- so the cost
       * of the emulation half is the transitions, not the work.  456 of
       * those 672 are video events, two per scanline, and the second of
       * each pair exists only so that an HBlank IRQ or an HBlank DMA can
       * fire at the right cycle.  Measured on the overworld: neither is
       * ever armed, 0% of update_gba calls.
       *
       * So when both are disarmed, the HDraw->HBlank and HBlank->line
       * transitions are done together and the CPU is given the whole
       * 1232-cycle scanline.  The condition is re-checked every scanline,
       * so a game (or a scene) that does arm one drops straight back to
       * the two-phase schedule.
       *
       * What this trades away: the CPU no longer runs with the HBlank
       * flag set, so code that polls DISPSTAT bit 1 would never see it,
       * and a VRAM write made "during HBlank" now lands before the line
       * it precedes is rendered rather than after.  Both only matter to
       * code synchronised to HBlank, which is the same code that would
       * have armed the IRQ or the DMA.
       */
      u32 merge_hblank = 0;
#endif
      // Check if we are in hrefresh (0) or hblank (1)
      if ((dispstat & 0x02) == 0)
      {
        // Transition from hrefresh to hblank
        dispstat |= 0x02;
        video_count += (272);    // hblank duration, 272 cycles

        // Check if we are drawing (0) or we are in vblank (1)
        if ((dispstat & 0x01) == 0)
        {
          u32 i;

          // Render the scan line
          if(reg[OAM_UPDATED])
            oam_update_count++;

          { u32 _ps = PROF_TICK();
          update_scanline();
          prof_ppu_ticks += PROF_TICK() - _ps; }

          // Trigger the HBlank DMAs if enabled
          for (i = 0; i < 4; i++)
          {
            if(dma[i].start_type == DMA_START_HBLANK)
              dma_transfer(i, &dma_cycles);
          }
        }

        // Trigger the hblank interrupt, if enabled in DISPSTAT
        if (dispstat & 0x10)
          irq_raised |= IRQ_HBLANK;

#ifdef N64_MERGE_HBLANK
        if (!(dispstat & 0x10)) {
          u32 i;
          merge_hblank = 1;
          for (i = 0; i < 4; i++)
            if (dma[i].start_type == DMA_START_HBLANK) { merge_hblank = 0; break; }
        }
#endif
      }
#ifdef N64_MERGE_HBLANK
      if ((dispstat & 0x02) != 0 || merge_hblank)
#else
      else
#endif
      {
        // Transition from hblank to the next scan line (vdraw or vblank)
        video_count += 960;
        dispstat &= ~0x02;
        vcount++;

        if(vcount == 160)
        {
          // Transition from vrefresh to vblank
          u32 i;
          dispstat |= 0x01;

          // Reinit affine transformation counters for the next frame
          video_reload_counters();

          // Trigger VBlank interrupt if enabled
          if (dispstat & 0x8)
            irq_raised |= IRQ_VBLANK;

          // Trigger the VBlank DMAs if enabled
          for (i = 0; i < 4; i++)
          {
            if(dma[i].start_type == DMA_START_VBLANK)
              dma_transfer(i, &dma_cycles);
          }
        }
        else if (vcount == 228)
        {
          // Transition from vblank to next screen
          vcount = 0;
          dispstat &= ~0x01;

          /* If there's no cheat hook, run on vblank! */
          if (cheat_master_hook == ~0U)
             process_cheats();

/*        printf("frame update (%x), %d instructions total, %d RAM flushes\n",
           reg[REG_PC], instruction_count - last_frame, flush_ram_count);
          last_frame = instruction_count;
*/
/*          printf("%d gbc audio updates\n", gbc_update_count);
          printf("%d oam updates\n", oam_update_count); */
          gbc_update_count = 0;
          oam_update_count = 0;
          flush_ram_count = 0;

          // Force audio generation. Need to flush samples for this frame.
#ifdef PROFILE_CYCLES
          { u32 _ss = PROF_TICK();
            render_gbc_sound();
            prof_snd_ticks += PROF_TICK() - _ss; }
#else
          render_gbc_sound();
#endif

          /* Keep the m4a mixer stub installed (see n64/m4a_hle.c).
           * Once found this is a single halfword compare. */
          M4A_HLE_FRAME();

          // We completed a frame, tell the dynarec to exit to the main thread
          frame_complete = 0x80000000;
          frame_counter++;
        }

        // Vcount trigger (flag) and IRQ if enabled
        if(vcount == (dispstat >> 8))
        {
          dispstat |= 0x04;
          if(dispstat & 0x20)
            irq_raised |= IRQ_VCOUNT;
        }
        else
          dispstat &= ~0x04;

        write_ioreg(REG_VCOUNT, vcount);
      }
      write_ioreg(REG_DISPSTAT, dispstat);
    }

    // Flag any V/H blank interrupts, DMA IRQs, Vcount, etc.
    if (irq_raised)
      flag_interrupt(irq_raised);

    // Raise any pending interrupts. This changes the CPU mode.
    if (check_and_raise_interrupts())
      changed_pc = 0x40000000;

    // Figure out when we need to stop CPU execution. The next event is
    // a video event or a timer event, whatever happens first.
    execute_cycles = MAX(video_count, 0);
#ifdef N64_EVENT_PROF
    /* 673 update_gba calls per frame against ~7400 executed GBA
     * instructions: essentially all of the emulator's CPU time is this
     * event machinery, not code execution.  Count what actually shortens
     * the window, so it is clear which scheduler is responsible. */
    { extern u32 prof_ev_video, prof_ev_serial, prof_ev_dma, prof_ev_timer[4],
             prof_ev_calls;
      u32 _base = execute_cycles; unsigned _t; u32 _who = 0;
      prof_ev_calls++;
      if (serial_next_event() < _base) { _who = 1; }
      if (reg[CPU_HALT_STATE] == CPU_DMA) { _who = 2; }
      for (_t = 0; _t < 4; _t++)
        if (timer[_t].status == TIMER_PRESCALE && timer[_t].count < _base &&
            timer[_t].count < execute_cycles)
          { _who = 3 + _t; }
      if (_who == 0)      prof_ev_video++;
      else if (_who == 1) prof_ev_serial++;
      else if (_who == 2) prof_ev_dma++;
      else                prof_ev_timer[_who - 3]++; }
#endif
    {
      u32 cc = serial_next_event();
      execute_cycles = MIN(execute_cycles, cc);
    }

    // If we are paused due to a DMA, cap the number of cyles to that amount.
    if (reg[CPU_HALT_STATE] == CPU_DMA) {
      u32 dma_cyc = reg[REG_SLEEP_CYCLES];
      // The first iteration is marked by bit 31 set, just do nothing now.
      if (dma_cyc & 0x80000000)
        dma_cyc &= 0x7FFFFFFF;  // Start counting DMA cycles from now on.
      else
        dma_cyc -= MIN(dma_cyc, completed_cycles);  // Account DMA cycles.

      reg[REG_SLEEP_CYCLES] = dma_cyc;
      if (!dma_cyc)
        reg[CPU_HALT_STATE] = CPU_ACTIVE;   // DMA finished, resume execution.
      else
        execute_cycles = MIN(execute_cycles, dma_cyc);  // Continue sleeping.
    }

    for (i = 0; i < 4; i++)
    {
       if (timer[i].status == TIMER_PRESCALE &&
           timer[i].count < execute_cycles)
       {
#ifdef N64_TIMER_BATCH
          /* Stopping the CPU at every timer overflow is what makes the
             event machinery the dominant cost of a frame: measured at 675
             update_gba calls per frame in the overworld, 32% of them for
             timer 0 alone, against ~7400 GBA instructions actually
             executed.
             An overflow only has to be timed precisely if something
             observes it at that instant -- an IRQ, or a cascade into a
             timer that may itself raise one.  A timer that only feeds the
             direct-sound FIFO does not: update_timers now services however
             many overflows fall inside the window, producing the same
             samples in the same order. */
          if (timer[i].irq ||
              ((i != 3) && timer[i + 1].status == TIMER_CASCADE))
#endif
          execute_cycles = timer[i].count;
       }
    }
  } while(reg[CPU_HALT_STATE] != CPU_ACTIVE && !frame_complete);

  // We voluntarily limit this. It is not accurate but it would be much harder.
  dma_cycles = MIN(64, dma_cycles);
  dma_cycles = MIN(execute_cycles, dma_cycles);

#ifdef PROFILE_CYCLES
  prof_update_ticks += PROF_TICK() - _upd_t0;
#endif
  {
    u32 _rv = (execute_cycles - dma_cycles) | changed_pc | frame_complete;
#ifdef N64_TIME_TRACE
    /* The return value is what selects the dynarec stub's next path: bit 31
     * exits to main, bit 30 goes to lookup_pc, and the low 15 bits become
     * reg_cycles.  When the JIT stops calling update_gba, the last value it
     * was handed says which way it went. */
    { static u32 _m = 0;
      if (++_m <= N64_TIME_TRACE_DENSE || (_m % 50) == 0)
        fprintf(stderr, "RET %lu rv=%08lx exec=%lu dma=%d halt=%lu pc=%08lx\n",
                (unsigned long)_m, (unsigned long)_rv,
                (unsigned long)execute_cycles, (int)dma_cycles,
                (unsigned long)reg[CPU_HALT_STATE],
                (unsigned long)reg[REG_PC]); }
#endif
    return _rv;
  }
}

void reset_gba(void)
{
  init_memory();
  init_main();
  init_cpu();
  reset_sound();
}

#ifdef TRACE_REGISTERS
void print_regs(void)
{
  printf("R0=%08x R1=%08x R2=%08x R3=%08x "
         "R4=%08x R5=%08x R6=%08x R7=%08x "
         "R8=%08x R9=%08x R10=%08x R11=%08x "
         "R12=%08x R13=%08x R14=%08x\n",
         reg[0], reg[1], reg[2], reg[3],
         reg[4], reg[5], reg[6], reg[7],
         reg[8], reg[9], reg[10], reg[11],
         reg[12], reg[13], reg[14]);
}
#endif

bool main_check_savestate(const u8 *src)
{
  int i;
  const u8 *p1 = bson_find_key(src, "emu");
  const u8 *p2 = bson_find_key(src, "timers");
  if (!p1 || !p2)
    return false;

  if (!bson_contains_key(p1, "cpu-ticks", BSON_TYPE_INT32) ||
      !bson_contains_key(p1, "exec-cycles", BSON_TYPE_INT32) ||
      !bson_contains_key(p1, "video-count", BSON_TYPE_INT32) ||
      !bson_contains_key(p1, "sleep-cycles", BSON_TYPE_INT32))
    return false;

  for (i = 0; i < 4; i++)
  {
    char tname[2] = {'0' + i, 0};
    const u8 *p = bson_find_key(p2, tname);
    if (!p)
      return false;

    if (!bson_contains_key(p, "count", BSON_TYPE_INT32) ||
        !bson_contains_key(p, "reload", BSON_TYPE_INT32) ||
        !bson_contains_key(p, "prescale", BSON_TYPE_INT32) ||
        !bson_contains_key(p, "freq-step", BSON_TYPE_INT32) ||
        !bson_contains_key(p, "dsc", BSON_TYPE_INT32) ||
        !bson_contains_key(p, "irq", BSON_TYPE_INT32) ||
        !bson_contains_key(p, "status", BSON_TYPE_INT32))
      return false;
  }

  return true;
}

bool main_read_savestate(const u8 *src)
{
  int i;
  const u8 *p1 = bson_find_key(src, "emu");
  const u8 *p2 = bson_find_key(src, "timers");
  if (!p1 || !p2)
    return false;

  if (!(bson_read_int32(p1, "cpu-ticks", &cpu_ticks) &&
         bson_read_int32(p1, "exec-cycles", &execute_cycles) &&
         bson_read_int32(p1, "video-count", (u32*)&video_count) &&
         bson_read_int32(p1, "sleep-cycles", &reg[REG_SLEEP_CYCLES])))
    return false;

  if (!bson_read_int32(p1, "frame-count", &frame_counter))
    frame_counter = 60 * 10;  // Use "fake" 10 seconds.

  for (i = 0; i < 4; i++)
  {
    char tname[2] = {'0' + i, 0};
    const u8 *p = bson_find_key(p2, tname);

    if (!(
      bson_read_int32(p, "count", (u32*)&timer[i].count) &&
      bson_read_int32(p, "reload", &timer[i].reload) &&
      bson_read_int32(p, "prescale", &timer[i].prescale) &&
      bson_read_int32(p, "freq-step", &timer[i].frequency_step) &&
      bson_read_int32(p, "dsc", &timer[i].direct_sound_channels) &&
      bson_read_int32(p, "irq", &timer[i].irq) &&
      bson_read_int32(p, "status", &timer[i].status)))
      return false;
  }

  return true;
}

unsigned main_write_savestate(u8* dst)
{
  int i;
  u8 *wbptr, *wbptr2, *startp = dst;
  bson_start_document(dst, "emu", wbptr);
  bson_write_int32(dst, "frame-count", frame_counter);
  bson_write_int32(dst, "cpu-ticks", cpu_ticks);
  bson_write_int32(dst, "exec-cycles", execute_cycles);
  bson_write_int32(dst, "video-count", video_count);
  bson_write_int32(dst, "sleep-cycles", reg[REG_SLEEP_CYCLES]);
  bson_finish_document(dst, wbptr);

  bson_start_document(dst, "timers", wbptr);
  for (i = 0; i < 4; i++)
  {
    char tname[2] = {'0' + i, 0};
    bson_start_document(dst, tname, wbptr2);
    bson_write_int32(dst, "count", timer[i].count);
    bson_write_int32(dst, "reload", timer[i].reload);
    bson_write_int32(dst, "prescale", timer[i].prescale);
    bson_write_int32(dst, "freq-step", timer[i].frequency_step);
    bson_write_int32(dst, "dsc", timer[i].direct_sound_channels);
    bson_write_int32(dst, "irq", timer[i].irq);
    bson_write_int32(dst, "status", timer[i].status);
    bson_finish_document(dst, wbptr2);
  }
  bson_finish_document(dst, wbptr);

  return (unsigned int)(dst - startp);
}


