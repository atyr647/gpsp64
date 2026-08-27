/* Native x86-64 harness for the gpSP N64 core -- CORRECTNESS testing.
 *
 * Links the portable GBA CPU/PPU/AOT code (the same cpu.cc, video.cc and
 * aot_generated.c the real N64 build uses) with no N64/libdragon
 * dependency, so the emulator core can be exercised in ~30s instead of a
 * flashcart round-trip.  This is what found the MOV-PC interworking and
 * LDM/STM base-in-list bugs, and it is the right tool for bisecting a
 * miscompiled AOT function (see native/bisect_correctness.sh).
 *
 * NOT FOR PERFORMANCE.  All performance numbers for this project come
 * from native/ares_bench.sh, which runs the real N64 ROM under ares and
 * reads the ROM's own VR4300 COUNT-based PROF output.  The target is an
 * N64; host instruction counts cannot see the things that actually
 * decide N64 framerate -- I-cache pressure from generated code, memory
 * latency, DMA and RDP contention.  Counting interpreted instructions
 * here can point the wrong way (more AOT coverage lowers this count
 * while potentially thrashing the VR4300's 16 KB I-cache), so treat the
 * counts below strictly as a correctness/regression signal.
 *
 * What --bench reports (correctness-oriented):
 *   - interpreted GBA instructions/frame, exactly reproducible for a
 *     given ROM + input script, so an unexpected change flags a
 *     behavioural regression.
 *   - final PC: a healthy run parks in the game's idle loop; a derailed
 *     one ends up executing data.
 *   - ARM vs Thumb split per hot page, since thumb2c.py is Thumb-only.
 */

#include "../common.h"
#include "../savestate.h"
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
extern u32 prof_iwram_hist[];
extern u32 *prof_pctrace;
extern u32 prof_pctrace_n, prof_pctrace_max, prof_pctrace_skip;
typedef struct { u32 pc; u32 count; } iw_entry_t;
extern iw_entry_t prof_iwram_entry[];
extern u32 prof_region_arm[];
extern u32 prof_region_thumb[];
extern u32 prof_swi_hist[];
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

/* Rendering is off by default: the harness exists to compare emulated CPU
 * work across builds, and the PPU is host-identical.  RENDER=1 turns it
 * on, which is how the PPU2 counters (effect mix, RSP applicability) can
 * be surveyed across game states in seconds rather than in ares. */
static int render_enable = -1;

static void run_frame(void)
{
  if (render_enable < 0) render_enable = getenv("RENDER") ? 1 : 0;
  skip_next_frame = !render_enable;
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
enum { INPUT_MASH, INPUT_WALK, INPUT_NONE };
static int input_mode = INPUT_MASH;

static void poll_fake_input(long frame)
{
  u16 key_input = 0x3FF;      /* active-low; all released */
  if (input_mode == INPUT_WALK) {
    /* Get through the intro and then keep moving.  Start/A mashing alone
     * parks the game on a dialogue box forever -- every state past ~7200
     * frames was byte-identical -- because it advances too slowly and
     * cannot answer a menu that needs the D-pad.  Press A often, Start
     * occasionally, and cycle directions so menu choices and overworld
     * movement both happen. */
    if ((frame % 16) < 4)  key_input &= ~(1 << 0);   /* A     */
    if ((frame % 256) < 4) key_input &= ~(1 << 3);   /* Start */
    /* Hold each direction long enough to actually cross a map.  At 64
     * frames a run covers ~4 tiles, which is enough to shuffle around a
     * room and never find the door -- 54,000 frames of it stayed inside
     * the player's house, which is one of the lightest scenes in the
     * game and a poor thing to benchmark. */
    switch ((frame / 240) % 4) {
    case 0: key_input &= ~(1 << 7); break;           /* Down  */
    case 1: key_input &= ~(1 << 5); break;           /* Right */
    case 2: key_input &= ~(1 << 6); break;           /* Up    */
    case 3: key_input &= ~(1 << 4); break;           /* Left  */
    }
  }
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
#ifdef PROFILE_PPU2
  { extern u32 prof_ppu2_effect[4], prof_ppu2_calls, prof_ppu2_objblend;
    extern u32 prof_ppu2_fastok, prof_ppu2_fastno, prof_ppu2_layers[8];
    memset(prof_ppu2_effect, 0, sizeof(u32) * 4);
    memset(prof_ppu2_layers, 0, sizeof(u32) * 8);
    prof_ppu2_calls = prof_ppu2_objblend = 0;
    prof_ppu2_fastok = prof_ppu2_fastno = 0; }
#endif
  memset(prof_iwram_hist, 0, sizeof(u32) * 512);
  memset(prof_iwram_entry, 0, sizeof(iw_entry_t) * 64);
  memset(prof_region_arm, 0, sizeof(u32) * 16);
  memset(prof_region_thumb, 0, sizeof(u32) * 16);
  memset(prof_swi_hist, 0, sizeof(u32) * 256);
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

/* Where interpreted instructions live, by GBA memory region.  This is
 * the first question the "hybrid port" idea has to answer: code in ROM
 * (0x08+) sits at fixed addresses and can be identified against a decomp
 * symbol map, whereas IWRAM (0x03) holds routines the game copies there
 * at runtime -- most notably the m4a sound mixer, which is hand-written
 * ARM and never appears in ROM at its executing address. */
static void report_regions(long frames)
{
  static const char *names[16] = {
    "BIOS", "?", "EWRAM", "IWRAM", "IO", "PAL", "VRAM", "OAM",
    "ROM0", "ROM0", "ROM1", "ROM1", "ROM2", "ROM2", "SRAM", "?"
  };
  u64 grand = 0;
  for (int i = 0; i < 16; i++)
    grand += (u64)prof_region_arm[i] + (u64)prof_region_thumb[i];
  if (!grand) grand = 1;
  fprintf(stderr, "\n  interpreted instructions by region:\n");
  for (int i = 0; i < 16; i++) {
    u64 a = prof_region_arm[i], t = prof_region_thumb[i];
    if (!(a + t)) continue;
    fprintf(stderr,
            "    %02x %-5s %9.0f/frame  %5.1f%%   ARM %9.0f  Thumb %9.0f\n",
            i, names[i], (double)(a + t) / (double)frames,
            100.0 * (double)(a + t) / (double)grand,
            (double)a / (double)frames, (double)t / (double)frames);
  }
}

/* Which BIOS SWIs the game issues.  A SWI whose cost is dominated by a
 * loop inside the BIOS (LZ77UnComp, CpuFastSet) is a natural candidate
 * for a native implementation: the semantics are documented and the
 * whole call is a pure memory-to-memory transform. */
static void report_iwram(long frames, int topn)
{
  u64 grand = 0;
  for (int i = 0; i < 512; i++) grand += prof_iwram_hist[i];
  if (!grand) return;
  { fprintf(stderr, "\n  IWRAM entry points (PC arriving from outside IWRAM):\n");
    for (int i = 0; i < 64 && prof_iwram_entry[i].count; i++)
      fprintf(stderr, "    0x%08lx  %8.1f entries/frame\n",
              (unsigned long)prof_iwram_entry[i].pc,
              (double)prof_iwram_entry[i].count / (double)frames);
  }
  fprintf(stderr, "\n  hottest IWRAM 64-byte blocks (%.0f insns/frame total):\n",
          (double)grand / (double)frames);
  static u32 tmp[512];
  memcpy(tmp, prof_iwram_hist, sizeof(tmp));
  for (int n = 0; n < topn; n++) {
    u32 best = 0; int bi = -1;
    for (int i = 0; i < 512; i++) if (tmp[i] > best) { best = tmp[i]; bi = i; }
    if (bi < 0) break;
    fprintf(stderr, "    0x%08lx  %8.0f/frame  %5.1f%%\n",
            (unsigned long)(0x03000000u + ((u32)bi << 6)),
            (double)best / (double)frames,
            100.0 * (double)best / (double)grand);
    tmp[bi] = 0;
  }
}

static void report_swis(long frames)
{
  static const char *names[32] = {
    "SoftReset", "RegisterRamReset", "Halt", "Stop", "IntrWait",
    "VBlankIntrWait", "Div", "DivArm", "Sqrt", "ArcTan", "ArcTan2",
    "CpuSet", "CpuFastSet", "GetBiosChecksum", "BgAffineSet",
    "ObjAffineSet", "BitUnPack", "LZ77UnCompWram", "LZ77UnCompVram",
    "HuffUnComp", "RLUnCompWram", "RLUnCompVram", "Diff8bitUnFilterWram",
    "Diff8bitUnFilterVram", "Diff16bitUnFilter", "SoundBiasChange",
    "SoundDriverInit", "SoundDriverMode", "SoundDriverMain",
    "SoundDriverVSync", "SoundChannelClear", "MidiKey2Freq"
  };
  int any = 0;
  for (int i = 0; i < 256; i++) if (prof_swi_hist[i]) { any = 1; break; }
  if (!any) return;
  fprintf(stderr, "\n  BIOS SWI calls:\n");
  for (int i = 0; i < 256; i++) {
    if (!prof_swi_hist[i]) continue;
    fprintf(stderr, "    swi 0x%02x %-22s %8.1f /frame\n", i,
            i < 32 ? names[i] : "?",
            (double)prof_swi_hist[i] / (double)frames);
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
  const char *state_out = NULL;
  const char *state_in = NULL;
  int positional = 0;

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--bench")) { bench = 1; }
    else if (!strcmp(argv[i], "--warmup") && i + 1 < argc) warmup = atol(argv[++i]);
    else if (!strcmp(argv[i], "--input") && i + 1 < argc) {
      const char *m = argv[++i];
      if (!strcmp(m, "none")) input_mode = INPUT_NONE;
      else if (!strcmp(m, "walk")) input_mode = INPUT_WALK;
      else if (!strcmp(m, "mash")) input_mode = INPUT_MASH;
      else { fprintf(stderr, "unknown --input mode: %s\n", m); return 1; }
    }
    else if (!strcmp(argv[i], "--pages") && i + 1 < argc) pages_out = argv[++i];
    else if (!strcmp(argv[i], "--savestate") && i + 1 < argc) state_out = argv[++i];
    else if (!strcmp(argv[i], "--loadstate") && i + 1 < argc) state_in = argv[++i];
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

  /* --romcheck: read the whole cart through the emulator's own read path
   * and checksum it, sequentially and then in a stride that forces the
   * 32 KB page cache to thrash.  A paged build (ROM_BUFFER_SIZE smaller
   * than the cart) must produce the same numbers as a fully resident one;
   * if it does not, demand paging is handing the game wrong data. */
  if (getenv("ROMCHECK")) {
    extern u32 gamepak_size;
    extern u32 prof_rom_page_misses;
    u32 sum1 = 0, sum2 = 0, a;
    for (a = 0; a < gamepak_size; a += 4)
      sum1 = sum1 * 31 + read_memory32(0x08000000 + a);
    u32 miss1 = prof_rom_page_misses;
    for (u32 off = 0; off < 0x8000; off += 4)
      for (a = off; a < gamepak_size; a += 0x8000)
        sum2 = sum2 * 31 + read_memory32(0x08000000 + a);
    u32 sum8 = 0, sum16 = 0, sumx = 0;
    for (a = 0; a < gamepak_size; a += 1)
      sum8 = sum8 * 31 + read_memory8(0x08000000 + a);
    for (a = 0; a < gamepak_size; a += 2)
      sum16 = sum16 * 31 + read_memory16(0x08000000 + a);
    /* Mirrors and past-the-end open bus, which take different paths. */
    for (a = 0; a < 0x2000000; a += 0x400) {
      sumx = sumx * 31 + read_memory32(0x0A000000 + a);
      sumx = sumx * 31 + read_memory16(0x0C000000 + a);
      sumx = sumx * 31 + read_memory8(0x08000000 + a);
    }
    fprintf(stderr, "ROMCHECK size=%lu seq=%08lx stride=%08lx "
                    "misses seq=%lu stride=%lu\n",
            (unsigned long)gamepak_size, (unsigned long)sum1,
            (unsigned long)sum2, (unsigned long)miss1,
            (unsigned long)(prof_rom_page_misses - miss1));
    fprintf(stderr, "ROMCHECK8 %08lx  ROMCHECK16 %08lx  ROMCHECKX %08lx\n",
            (unsigned long)sum8, (unsigned long)sum16, (unsigned long)sumx);
    return 0;
  }

  /* Start from a captured state instead of the boot sequence.  Boot and
   * gameplay have wildly different instruction mixes -- gameplay runs
   * about a thirtieth of boot's instruction count and two thirds of it
   * is the sound mixer -- so a benchmark that only ever reaches boot is
   * measuring the wrong thing. */
  if (state_in) {
    FILE *f = fopen(state_in, "rb");
    void *buf = malloc(GBA_STATE_MEM_SIZE);
    if (f && buf) {
      size_t got = fread(buf, 1, GBA_STATE_MEM_SIZE, f);
      fprintf(stderr, "[gpSP]: savestate %s: read %u bytes, load %s\n",
              state_in, (unsigned)got,
              (got == GBA_STATE_MEM_SIZE && gba_load_state(buf)) ? "OK" : "FAILED");
      /* The GPIO registers (Emerald has an RTC) are not read from a
       * register file -- they are mirrored into the cart's first page,
       * which a fresh load_gamepak() has just overwritten with pristine
       * ROM.  Nothing in gba_load_state puts them back. */
      { extern void update_gpio_romregs(void); update_gpio_romregs(); }
      { char bp[600]; snprintf(bp, sizeof(bp), "%s.bak", state_in);
        FILE *bf = fopen(bp, "rb");
        if (bf) { extern u8 gamepak_backup[];
                  size_t bg = fread(gamepak_backup, 1, 1024 * 128, bf); fclose(bf);
                  fprintf(stderr, "[gpSP]: backup %s: %u bytes\n", bp, (unsigned)bg); } }
    }
    if (f) fclose(f);
    free(buf);
  }

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

  /* STATECHECK: save the current state and immediately load it back, then
   * report every register the round trip did not preserve.  A savestate
   * that restores cleanly in one game state and crashes the game in
   * another is losing something that only matters sometimes, and this
   * finds it without having to guess. */
  if (getenv("STATECHECK")) {
    static u32 before[64];
    void *buf = malloc(GBA_STATE_MEM_SIZE);
    memcpy(before, reg, sizeof(before));
    if (buf) {
      memset(buf, 0, GBA_STATE_MEM_SIZE);
      gba_save_state(buf);
      int ok = gba_load_state(buf);
      fprintf(stderr, "STATECHECK load=%s\n", ok ? "OK" : "FAILED");
      for (u32 i = 0; i < 64; i++)
        if (before[i] != reg[i])
          fprintf(stderr, "STATECHECK reg[%2u] %08lx -> %08lx\n",
                  (unsigned long)i, (unsigned long)before[i],
                  (unsigned long)reg[i]);
      free(buf);
    }
  }

  /* Hand the post-warmup state to the N64 build so ares can start the
   * benchmark in gameplay instead of in the boot sequence.  See
   * native/ares_bench.sh and n64/n64_main.c. */
  if (state_out) {
    void *buf = malloc(GBA_STATE_MEM_SIZE);
    if (buf) {
      memset(buf, 0, GBA_STATE_MEM_SIZE);
      gba_save_state(buf);
      FILE *f = fopen(state_out, "wb");
      if (f) { fwrite(buf, 1, GBA_STATE_MEM_SIZE, f); fclose(f);
               fprintf(stderr, "[gpSP]: wrote savestate %s (%d bytes) at frame %ld\n",
                       state_out, GBA_STATE_MEM_SIZE, warmup); }
      /* The cart's backup memory is deliberately not part of a gpSP
       * savestate -- it belongs to the .sav file.  For a benchmark state
       * that is a trap: a game that has written its save and later reads
       * it back comes up to blank flash and crashes.  Ship it alongside. */
      { char bp[600]; snprintf(bp, sizeof(bp), "%s.bak", state_out);
        FILE *bf = fopen(bp, "wb");
        if (bf) { extern u8 gamepak_backup[];
                  fwrite(gamepak_backup, 1, 1024 * 128, bf); fclose(bf); } }
      free(buf);
    }
  }

  reset_counters();

  /* Arm the trace only once warmup is over, so two runs that reach the
   * same point by different routes -- one by running, one by loading a
   * savestate -- both record from that point. */
  { const char *tp = getenv("PCTRACE");
    if (tp) { prof_pctrace_max = (u32)atol(getenv("PCTRACE_N") ? getenv("PCTRACE_N") : "4000000");
              prof_pctrace = (u32*)malloc((size_t)prof_pctrace_max * 4);
              prof_pctrace_skip = (u32)atol(getenv("PCTRACE_SKIP") ? getenv("PCTRACE_SKIP") : "0"); } }

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
#ifdef PROFILE_PPU2
    { extern u32 prof_ppu2_effect[4], prof_ppu2_calls, prof_ppu2_objblend;
      extern u32 prof_ppu2_fastok, prof_ppu2_fastno;
      u32 c = prof_ppu2_calls ? prof_ppu2_calls : 1;
      u32 ft = prof_ppu2_fastok + prof_ppu2_fastno; if (!ft) ft = 1;
      fprintf(stderr, "\n  ppu2: scanlines=%lu  effect none %lu%% bright %lu%%"
                      " dark %lu%% blend %lu%%  objblend %lu%%"
                      "  RSP-fastpath %lu%%\n",
              (unsigned long)prof_ppu2_calls,
              (unsigned long)(prof_ppu2_effect[0]*100/c),
              (unsigned long)(prof_ppu2_effect[1]*100/c),
              (unsigned long)(prof_ppu2_effect[2]*100/c),
              (unsigned long)(prof_ppu2_effect[3]*100/c),
              (unsigned long)(prof_ppu2_objblend*100/c),
              (unsigned long)(prof_ppu2_fastok*100/ft)); }
#endif
#ifdef N64_BGBAND_VERIFY
    { extern u32 bgband_lines, bgband_mismatch, bgband_skipped;
      fprintf(stderr, "\n  bgband: %lu lines checked, %lu mismatched, %lu skipped\n",
              (unsigned long)bgband_lines, (unsigned long)bgband_mismatch,
              (unsigned long)bgband_skipped); }
#endif
#ifdef PROFILE_MIDFRAME
    { extern u32 prof_mid_vram, prof_mid_pal, prof_mid_bgreg;
      double _f = (double)(num_frames ? num_frames : 1);
      extern u32 prof_vram_writes, prof_vram_dma_bytes;
      fprintf(stderr, "  VRAM per frame: %.0f CPU stores, %.1f KB via DMA\n",
              prof_vram_writes / _f, prof_vram_dma_bytes / _f / 1024.0);
      fprintf(stderr, "\n  mid-frame writes per frame (HDraw only):"
                      "  VRAM %.1f   palette %.1f   BG regs %.1f\n",
              prof_mid_vram / _f, prof_mid_pal / _f, prof_mid_bgreg / _f); }
#endif
    report_regions(num_frames);
    report_swis(num_frames);
    report_iwram(num_frames, 14);
    report_hot_pages(num_frames, 12);
    report_hot_calls(num_frames, 10);
    report_hot_pcs(num_frames, 16);
    if (pages_out) dump_pages(pages_out, num_frames);
    { const char *tp = getenv("PCTRACE");
      if (tp && prof_pctrace) { FILE *f = fopen(tp, "wb");
        if (f) { fwrite(prof_pctrace, 4, prof_pctrace_n, f); fclose(f);
                 fprintf(stderr, "PCTRACE %s: %lu entries\n", tp,
                         (unsigned long)prof_pctrace_n); } } }
    /* Write the last rendered frame as a PNG-able PPM.  Knowing which
     * screen a benchmark state is parked on is not a nicety: the two
     * states surveyed here differ by 2x in rendering cost purely because
     * one has a brightness effect on every scanline and the other does
     * not. */
    { const char *sp = getenv("DUMP_SCREEN");
      if (sp && render_enable) {
        FILE *f = fopen(sp, "wb");
        if (f) {
          fprintf(f, "P6\n%d %d\n255\n", GBA_SCREEN_WIDTH, GBA_SCREEN_HEIGHT);
          for (int y = 0; y < GBA_SCREEN_HEIGHT; y++)
            for (int x = 0; x < GBA_SCREEN_WIDTH; x++) {
              u16 px = gba_screen_pixels[y * GBA_SCREEN_PITCH + x];
              int r, g, b;
#ifdef USE_XBGR1555_FORMAT
              /* GBA-native XBGR1555, which is what the N64 build uses. */
              r = px & 0x1F; g = (px >> 5) & 0x1F; b = (px >> 10) & 0x1F;
              r = r << 3 | r >> 2; g = g << 3 | g >> 2; b = b << 3 | b >> 2;
#else
              /* gpSP's default is RGB565 -- convert_palette in common.h.
               * Decoding it as XBGR1555 swaps red and blue and shifts
               * green, which is what made the first screenshots look
               * wrong.  The emulator was fine; the dump was not. */
              r = (px >> 11) & 0x1F; g = (px >> 5) & 0x3F; b = px & 0x1F;
              r = r << 3 | r >> 2; g = g << 2 | g >> 4; b = b << 3 | b >> 2;
#endif
              fputc(r, f); fputc(g, f); fputc(b, f);
            }
          fclose(f);
        }
      } }

    /* Dump every GBA-visible memory region.  A savestate round trip cannot
     * detect missing coverage -- it rewrites the values that were already
     * correct in memory and leaves anything uncovered untouched, so it
     * always looks clean.  Comparing a state reached by running against
     * the same state reached by loading is what actually finds the gap. */
    { const char *ap = getenv("DUMP_ALL");
      if (ap) { FILE *f = fopen(ap, "wb");
        if (f) {
          extern u8 iwram_raw[], ewram_raw[], vram_raw[], gamepak_backup[];
          fwrite(iwram_raw + 0x8000, 1, 0x8000, f);
          fwrite(ewram_raw, 1, 0x40000, f);
          fwrite(vram_raw, 1, 0x18000, f);
          fwrite(oam_ram_raw, 1, 0x400, f);
          fwrite(palette_ram_raw, 1, 0x400, f);
          fwrite(io_registers_raw, 1, 0x400, f);   /* declared in gba_memory.h */
          fwrite(gamepak_backup, 1, 0x20000, f);
          fwrite(reg, 4, 64, f);
          fclose(f);
        } } }

    { const char *iw = getenv("DUMP_IWRAM");
      if (iw) { extern u8 iwram_raw[]; FILE *f = fopen(iw, "wb");
                if (f) { fwrite(iwram_raw, 1, 64 * 1024, f); fclose(f); } } }
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
