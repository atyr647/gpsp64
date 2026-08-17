/* Native x86-64 test harness for gpSP N64 core logic.
 *
 * Links the portable GBA CPU/PPU/AOT code (the same cpu.cc, video.cc,
 * aot_generated.c used by the real N64 build) without any N64/libdragon
 * hardware dependency, so the AOT dispatch + cycle-accounting bug can be
 * iterated on natively instead of round-tripping through a flashcart.
 *
 * NOT a performance benchmark: x86-64 wall-clock numbers are meaningless
 * for N64 timing.  This only validates correctness/behavior (does the
 * hang reproduce, does a cycle-penalty fix change it) — final performance
 * numbers still require real N64 hardware.
 */

#include "../common.h"
#include <stdio.h>

/* Platform globals normally provided by n64/n64_main.c */
u32 skip_next_frame = 0;
u32 num_skipped_frames = 0;
int dynarec_enable = 0;
boot_mode selected_boot_mode = boot_game;
int sprite_limit = 1;
u32 netplay_num_clients = 0, netplay_client_id = 0;

u32 idle_loop_target_pc = 0xFFFFFFFF;
u32 translation_gate_target_pc[MAX_TRANSLATION_GATES];
u32 translation_gate_targets = 0;

void error_msg(const char *text) { fprintf(stderr, "[gpSP error]: %s\n", text); }
void info_msg(const char *text)  { fprintf(stderr, "[gpSP]: %s\n", text); }

void set_fastforward_override(bool fastforward) { (void)fastforward; }
void netpacket_poll_receive(void) {}
void netpacket_send(uint16_t client_id, const void *buf, size_t len) {
  (void)client_id; (void)buf; (void)len;
}

static void run_frame(void)
{
  skip_next_frame = 1;
  clear_gamepak_stickybits();
  execute_arm(execute_cycles);
}

/* Simulate a player mashing Start/A every couple seconds to get past
 * splash/title/menu screens, since the harness has no real controller.
 * GBA key register is active-low: 0 = pressed, 1 = released.
 * Bit 0 = A, bit 3 = Start. */
static void poll_fake_input(long frame)
{
  long phase = frame % 120;   /* every 2 seconds of GBA time (60fps) */
  u16 key_input = 0x3FF;      /* all released */
  if (phase < 6) {
    key_input &= ~(1 << 3);   /* Start */
    key_input &= ~(1 << 0);   /* A */
  }
  write_ioreg(REG_P1, key_input);
}

int main(int argc, char **argv)
{
  if (argc < 2) {
    fprintf(stderr, "usage: %s <rom.gba> [frames] [bios.bin]\n", argv[0]);
    return 1;
  }
  const char *rom_path = argv[1];
  long num_frames = (argc >= 3) ? atol(argv[2]) : 600;
  const char *bios_path = (argc >= 4) ? argv[3] : "bios/open_gba_bios.bin";

  static u16 screen_buf[240 * 160];
  gba_screen_pixels = screen_buf;
  memset(gba_screen_pixels, 0, sizeof(screen_buf));

  init_gamepak_buffer();

  bool bios_ok = false;
  if (load_bios((char *)bios_path) == 0 && bios_rom_raw[0] == 0x18)
    bios_ok = true;
  if (!bios_ok) {
    info_msg("Using built-in open-source BIOS");
    memcpy(bios_rom_raw, open_gba_bios_rom, sizeof(bios_rom_raw));
  }

  memset(gamepak_backup, 0xff, sizeof(gamepak_backup));

  if (load_gamepak(rom_path, FEAT_AUTODETECT, FEAT_AUTODETECT, 0) != 0) {
    error_msg("Could not load game file");
    return 1;
  }

  {
    extern u8 *gamepak_buffers[];
    char gc[5] = {0};
    memcpy(gc, &gamepak_buffers[0][0xAC], 4);
    fprintf(stderr, "[gpSP]: gamepak_code='%s' idle_loop_target_pc=0x%08lx\n",
            gc, (unsigned long)idle_loop_target_pc);
  }

  reset_gba();

  extern u32 prof_arm_insns, prof_thumb_insns;
  extern u32 prof_idle_hits, prof_idle_detect_fires;
#ifdef PROFILE_AOT
  extern u32 prof_page_hist[];
  extern u32 prof_ppu_ticks;
#endif

  for (long f = 0; f < num_frames; f++) {
    poll_fake_input(f);
    run_frame();

    if ((f % 60) == 59) {
      u32 total_insns = prof_arm_insns + prof_thumb_insns;
      u32 arm_pct = total_insns ? (prof_arm_insns * 100) / total_insns : 0;
      u32 thm_pct = total_insns ? (prof_thumb_insns * 100) / total_insns : 0;
      fprintf(stderr,
              "FRAME %6ld | %luK insns | ARM%lu%%/Thm%lu%% idle %lu rt %lu | PC=0x%08x\n",
              f + 1,
              (unsigned long)(total_insns / 1000),
              (unsigned long)arm_pct, (unsigned long)thm_pct,
              (unsigned long)prof_idle_hits,
              (unsigned long)prof_idle_detect_fires,
              reg[REG_PC]);
#ifdef PROFILE_AOT
      {
        u32 pg_idx[3] = {0}, pg_cnt[3] = {0};
        for (u32 i = 0; i < 6144; i++) {
          u32 c = prof_page_hist[i];
          if (!c) continue;
          for (u32 j = 0; j < 3; j++) {
            if (c > pg_cnt[j]) {
              for (u32 k = 2; k > j; k--) { pg_cnt[k]=pg_cnt[k-1]; pg_idx[k]=pg_idx[k-1]; }
              pg_cnt[j] = c; pg_idx[j] = i;
              break;
            }
          }
        }
        fprintf(stderr, "  aot-pages: 0x%05lx:%lu 0x%05lx:%lu 0x%05lx:%lu\n",
                (unsigned long)((pg_idx[0]+0x8000)*0x1000), (unsigned long)pg_cnt[0],
                (unsigned long)((pg_idx[1]+0x8000)*0x1000), (unsigned long)pg_cnt[1],
                (unsigned long)((pg_idx[2]+0x8000)*0x1000), (unsigned long)pg_cnt[2]);
        memset(prof_page_hist, 0, sizeof(u32) * 6144);
      }
#endif
      prof_arm_insns = prof_thumb_insns = 0;
      prof_idle_hits = 0;
      prof_idle_detect_fires = 0;
    }
  }

  fprintf(stderr, "Done: %ld frames. Final PC=0x%08x\n", num_frames, reg[REG_PC]);
  return 0;
}
