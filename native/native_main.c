/* Native x86-64 harness for the gpSP N64 core: correctness testing and
 * deterministic performance measurement.
 *
 * Links the portable GBA CPU/PPU/AOT code (the same cpu.cc, video.cc and
 * aot_generated.c the real N64 build uses) with no N64/libdragon
 * dependency, so the emulator core can be exercised in ~1s instead of a
 * flashcart round-trip.
 *
 * On benchmarking: x86-64 wall-clock time here is meaningless for N64
 * performance.  What IS meaningful, and what --bench measures, is the
 * amount of *emulated work* per GBA frame:
 *
 *   - interpreted GBA instructions/frame.  This is the primary metric.
 *     Every instruction the AOT does not cover runs through the
 *     interpreter at roughly 150-250 VR4300 cycles on real hardware, so
 *     driving this number down is what raises framerate.  It is exactly
 *     reproducible for a given ROM + input script, so before/after deltas
 *     are trustworthy in a way wall-clock never is.
 *   - AOT dispatches/frame and the GBA cycles they account for, i.e. how
 *     much work the AOT is absorbing.
 *   - ARM vs Thumb split, per hot page.  tools/thumb2c.py only translates
 *     Thumb, so an ARM-dominated hot page cannot be helped by adding AOT
 *     targets -- this split says which pages are actually actionable.
 *
 * Absolute framerate still has to come from real hardware (or ares); this
 * harness tells you which direction you are moving and why.
 */

#include "../common.h"
#include <stdio.h>
#include <string.h>

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

extern u32 prof_arm_insns, prof_thumb_insns;
extern u32 prof_idle_hits, prof_idle_detect_fires;
#ifdef PROFILE_AOT
#define AOT_PAGE_BUCKETS 6144
extern u32 prof_page_hist[];
extern u32 prof_page_hist_thumb[];
extern u32 prof_aot_hits, prof_aot_hw_hits, prof_aot_gba_cycles;
extern u32 prof_pc_hist[];
extern u32 prof_pc_hist_page;
typedef struct { u32 pc; u32 count; } bl_target_t;
extern bl_target_t prof_bl_targets[];
#define AOT_BL_TABLE_SIZE 512
#endif

/* A GBA frame is 228 scanlines * 1232 cycles. Used to express AOT
 * coverage as a fraction of the total emulated cycle budget. */
#define GBA_CYCLES_PER_FRAME 280896

/* Drained once per frame to mirror n64_audio_render_frame(); leaving the
 * sound ring buffer permanently full makes the GBA sound engine behave
 * differently than it does on hardware. */
static s16 audio_scratch[1024 * 2];

static void run_frame(void)
{
  skip_next_frame = 1;
  clear_gamepak_stickybits();
  execute_arm(execute_cycles);
  sound_read_samples(audio_scratch, 1024);
}

/* Deterministic input scripts.  Determinism is the point: it is what
 * makes instruction counts comparable across builds.
 *
 *   mash  - press Start/A every 2s, walking through splash/title/menu.
 *           Ends parked in the game's idle loop, so it measures a
 *           lightly-loaded state and is the best hang/correctness check.
 *   none  - never press anything, so the intro cutscene and the animated
 *           title screen keep running.  Much heavier CPU+PPU load, and
 *           therefore the more useful workload for perf work.
 */
enum { INPUT_MASH, INPUT_NONE };
static int input_mode = INPUT_MASH;

static void poll_fake_input(long frame)
{
  u16 key_input = 0x3FF;      /* active-low; all released */
  if (input_mode == INPUT_MASH && (frame % 120) < 6) {
    key_input &= ~(1 << 3);   /* Start */
    key_input &= ~(1 << 0);   /* A */
  }
  write_ioreg(REG_P1, key_input);
}

static void reset_counters(void)
{
  prof_arm_insns = prof_thumb_insns = 0;
  prof_idle_hits = prof_idle_detect_fires = 0;
#ifdef PROFILE_AOT
  prof_aot_hits = prof_aot_hw_hits = prof_aot_gba_cycles = 0;
  memset(prof_page_hist, 0, sizeof(u32) * AOT_PAGE_BUCKETS);
  memset(prof_page_hist_thumb, 0, sizeof(u32) * AOT_PAGE_BUCKETS);
  memset(prof_bl_targets, 0, sizeof(bl_target_t) * AOT_BL_TABLE_SIZE);
  memset(prof_pc_hist, 0, sizeof(u32) * 2048);
#endif
}

#ifdef PROFILE_AOT
/* Dump every executed page to a file as "addr total thumb", descending
 * by total.  tools/suggest_targets.py turns this into aot_targets.txt
 * ranges. */
static void dump_pages(const char *path, long frames)
{
  FILE *fp = fopen(path, "w");
  if (!fp) return;
  fprintf(fp, "# page_addr  total_insns  thumb_insns   (over %ld frames)\n",
          frames);
  for (;;) {
    u32 best = 0, best_i = 0;
    for (u32 i = 0; i < AOT_PAGE_BUCKETS; i++)
      if (prof_page_hist[i] > best) { best = prof_page_hist[i]; best_i = i; }
    if (!best) break;
    fprintf(fp, "0x%08lx  %10lu  %10lu\n",
            (unsigned long)((best_i + 0x8000) * 0x1000),
            (unsigned long)best,
            (unsigned long)prof_page_hist_thumb[best_i]);
    prof_page_hist[best_i] = 0;   /* consume so the next pass finds the next */
  }
  fclose(fp);
}

static void report_hot_pages(long frames, int topn)
{
  fprintf(stderr, "\n  hot pages (interpreted only; Thumb%% = AOT-able):\n");
  /* Copy so we can consume without destroying the caller's data. */
  static u32 tmp[AOT_PAGE_BUCKETS];
  memcpy(tmp, prof_page_hist, sizeof(tmp));
  u32 grand = 0;
  for (u32 i = 0; i < AOT_PAGE_BUCKETS; i++) grand += tmp[i];
  if (!grand) grand = 1;

  for (int n = 0; n < topn; n++) {
    u32 best = 0, best_i = 0;
    for (u32 i = 0; i < AOT_PAGE_BUCKETS; i++)
      if (tmp[i] > best) { best = tmp[i]; best_i = i; }
    if (!best) break;
    u32 th = prof_page_hist_thumb[best_i];
    fprintf(stderr,
            "    0x%08lx  %8lu/frame  %5.1f%% of interp  Thumb %3lu%%\n",
            (unsigned long)((best_i + 0x8000) * 0x1000),
            (unsigned long)(best / (frames ? frames : 1)),
            100.0 * (double)best / (double)grand,
            (unsigned long)(best ? (th * 100) / best : 0));
    tmp[best_i] = 0;
  }
}

static void report_hot_pcs(long frames, int topn)
{
  if (prof_pc_hist_page == 0xFFFFFFFFu) return;
  fprintf(stderr, "\n  hottest instructions in page 0x%08lx:\n",
          (unsigned long)(prof_pc_hist_page << 12));
  static u32 tmp[2048];
  memcpy(tmp, prof_pc_hist, sizeof(tmp));
  for (int n = 0; n < topn; n++) {
    u32 best = 0, best_i = 0;
    for (u32 i = 0; i < 2048; i++)
      if (tmp[i] > best) { best = tmp[i]; best_i = i; }
    if (!best) break;
    fprintf(stderr, "    0x%08lx  %8lu /frame\n",
            (unsigned long)((prof_pc_hist_page << 12) | (best_i << 1)),
            (unsigned long)(best / (frames ? frames : 1)));
    tmp[best_i] = 0;
  }
}

static void report_hot_calls(long frames, int topn)
{
  fprintf(stderr, "\n  hottest call targets (BL destinations):\n");
  static bl_target_t tmp[AOT_BL_TABLE_SIZE];
  memcpy(tmp, prof_bl_targets, sizeof(tmp));
  for (int n = 0; n < topn; n++) {
    u32 best = 0; int best_i = -1;
    for (int i = 0; i < AOT_BL_TABLE_SIZE; i++)
      if (tmp[i].count > best) { best = tmp[i].count; best_i = i; }
    if (best_i < 0 || !best) break;
    fprintf(stderr, "    0x%08lx  %8lu calls/frame\n",
            (unsigned long)tmp[best_i].pc,
            (unsigned long)(best / (frames ? frames : 1)));
    tmp[best_i].count = 0;
  }
}
#endif

int main(int argc, char **argv)
{
  const char *rom_path = NULL;
  const char *bios_path = "bios/open_gba_bios.bin";
  long num_frames = 600;
  long warmup = 0;
  int bench = 0;
  const char *pages_out = NULL;
  int positional = 0;

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--bench")) { bench = 1; }
    else if (!strcmp(argv[i], "--warmup") && i + 1 < argc) warmup = atol(argv[++i]);
    else if (!strcmp(argv[i], "--input") && i + 1 < argc) {
      const char *m = argv[++i];
      if (!strcmp(m, "none")) input_mode = INPUT_NONE;
      else if (!strcmp(m, "mash")) input_mode = INPUT_MASH;
      else { fprintf(stderr, "unknown --input mode: %s\n", m); return 1; }
    }
    else if (!strcmp(argv[i], "--pages") && i + 1 < argc) pages_out = argv[++i];
#ifdef PROFILE_AOT
    else if (!strcmp(argv[i], "--pchist") && i + 1 < argc)
      prof_pc_hist_page = (u32)strtoul(argv[++i], NULL, 0) >> 12;
#endif
    else if (argv[i][0] == '-') {
      fprintf(stderr, "unknown option: %s\n", argv[i]);
      return 1;
    }
    else if (positional == 0) { rom_path = argv[i]; positional++; }
    else if (positional == 1) { num_frames = atol(argv[i]); positional++; }
    else if (positional == 2) { bios_path = argv[i]; positional++; }
  }

  if (!rom_path) {
    fprintf(stderr,
      "usage: %s <rom.gba> [frames] [bios.bin] [--bench] [--warmup N]\n"
      "       [--input mash|none] [--pages FILE] [--pchist ADDR]\n",
      argv[0]);
    return 1;
  }

  static u16 screen_buf[240 * 160];
  gba_screen_pixels = screen_buf;
  memset(gba_screen_pixels, 0, sizeof(screen_buf));

  /* Match n64_main.c's startup order: the sound engine must be
   * initialised (noise tables + reset_sound) before the ROM runs, or the
   * GBA sound/DMA state starts out garbage. */
  init_sound();

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

  if (!bench) {
    /* Interactive/regression mode: periodic progress, catches hangs. */
    for (long f = 0; f < num_frames; f++) {
      poll_fake_input(f);
      run_frame();
      if ((f % 60) == 59) {
        u32 total = prof_arm_insns + prof_thumb_insns;
        fprintf(stderr, "FRAME %6ld | %luK insns | ARM%lu%%/Thm%lu%% | PC=0x%08x\n",
                f + 1, (unsigned long)(total / 1000),
                (unsigned long)(total ? (prof_arm_insns * 100) / total : 0),
                (unsigned long)(total ? (prof_thumb_insns * 100) / total : 0),
                reg[REG_PC]);
        reset_counters();
      }
    }
    fprintf(stderr, "Done: %ld frames. Final PC=0x%08x\n", num_frames, reg[REG_PC]);
    return 0;
  }

  /* --- benchmark mode --- */

  /* Warm up past BIOS decompression / intro so we measure steady-state
   * gameplay rather than one-off boot work. */
  for (long f = 0; f < warmup; f++) { poll_fake_input(f); run_frame(); }
  reset_counters();

  for (long f = warmup; f < warmup + num_frames; f++) {
    poll_fake_input(f);
    run_frame();
  }

  {
    double frames = (double)(num_frames ? num_frames : 1);
    u64 interp = (u64)prof_arm_insns + (u64)prof_thumb_insns;

    fprintf(stderr, "\n=== BENCH (%ld frames after %ld warmup) ===\n",
            num_frames, warmup);
    fprintf(stderr, "  final PC              0x%08x\n", reg[REG_PC]);
    fprintf(stderr, "  interpreted insns     %10.0f /frame   <-- primary metric\n",
            (double)interp / frames);
    fprintf(stderr, "    ARM                 %10.0f /frame  (%.0f%%)\n",
            (double)prof_arm_insns / frames,
            interp ? 100.0 * (double)prof_arm_insns / (double)interp : 0.0);
    fprintf(stderr, "    Thumb               %10.0f /frame  (%.0f%%)\n",
            (double)prof_thumb_insns / frames,
            interp ? 100.0 * (double)prof_thumb_insns / (double)interp : 0.0);
    fprintf(stderr, "  idle-loop skips       %10.0f /frame\n",
            (double)prof_idle_hits / frames);
#ifdef PROFILE_AOT
    fprintf(stderr, "  AOT dispatches        %10.0f /frame  (auto %lu, handwritten %lu total)\n",
            (double)(prof_aot_hits + prof_aot_hw_hits) / frames,
            (unsigned long)prof_aot_hits, (unsigned long)prof_aot_hw_hits);
    fprintf(stderr, "  AOT-covered cycles    %10.0f /frame  (%.1f%% of %d cyc frame budget)\n",
            (double)prof_aot_gba_cycles / frames,
            100.0 * ((double)prof_aot_gba_cycles / frames) / (double)GBA_CYCLES_PER_FRAME,
            GBA_CYCLES_PER_FRAME);
    report_hot_pages(num_frames, 12);
    report_hot_calls(num_frames, 10);
    report_hot_pcs(num_frames, 16);
    if (pages_out) dump_pages(pages_out, num_frames);
#endif
    /* Single-line machine-readable summary for scripted diffing. */
    fprintf(stderr, "\nBENCHSUM interp_per_frame=%.0f arm=%.0f thumb=%.0f",
            (double)interp / frames,
            (double)prof_arm_insns / frames,
            (double)prof_thumb_insns / frames);
#ifdef PROFILE_AOT
    fprintf(stderr, " aot_per_frame=%.0f aot_cyc_per_frame=%.0f",
            (double)(prof_aot_hits + prof_aot_hw_hits) / frames,
            (double)prof_aot_gba_cycles / frames);
#endif
    fprintf(stderr, "\n");
  }
  return 0;
}
