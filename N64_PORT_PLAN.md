# Plan: Porting gpSP (GBA Emulator) to Nintendo 64

## Target Hardware Summary

| Spec | N64 |
|------|-----|
| CPU | NEC VR4300 (MIPS III, 64-bit) @ 93.75 MHz |
| RAM | 4 MB (8 MB with Expansion Pak) |
| GPU | RDP (rasterizer) + RSP (vector coprocessor) |
| Display | 320x240 typical |
| Storage | Cartridge ROM / SD card via flashcart |
| SDK | libdragon (open-source) |

## Why This Is Feasible

- **gpsp already has a MIPS dynarec** (`mips_emit.h`, `mips_codegen.h`, `mips_stub.S`) — the N64's VR4300 is MIPS III
- **No floating-point in the core** — all fixed-point math, which is critical given the VR4300's slow FPU
- **Already ported to similarly constrained MIPS platforms** — PSP (MIPS R4000, 333 MHz, 32 MB RAM) and PS2
- **GBA's 240x160 display fits cleanly** inside N64's 320x240 framebuffer

## The #1 Challenge: Memory (4-8 MB total)

Current memory footprint is ~35-40 MB. N64 has 4-8 MB for **everything** (code, framebuffer, audio, emulator state, ROM data). This dominates every design decision.

| Component | Current Size | N64 Budget | Strategy |
|-----------|-------------|------------|----------|
| GBA ROM | up to 32 MB | 0 MB in RAM | Stream from SD card |
| ROM dynarec cache | 10 MB (2 MB small) | ~512 KB | Aggressive eviction |
| RAM dynarec cache | 512 KB (384 KB small) | ~128 KB | Reduce further |
| EWRAM | 512 KB (with SMC mirror) | 256 KB | Drop SMC mirror, use dirty-page tracking |
| IWRAM | 64 KB (with SMC mirror) | 32 KB | Drop SMC mirror |
| VRAM + Palette + OAM | ~98 KB | ~98 KB | Keep as-is |
| Framebuffer (N64) | — | ~150 KB | 320x240x16-bit |
| Sound buffer | 128 KB | ~16 KB | Shrink buffer |
| Color LUT | ~263 KB | 0 | Compute on-the-fly or reduce |
| Emulator code + stack | — | ~256 KB | Estimate |
| **Total** | **~35-40 MB** | **~1.5 MB** + streaming | |

**Expansion Pak (8 MB) is required.** Even then, every byte matters.

---

## Phased Implementation Plan

### Phase 1: Build System & Platform Skeleton

**Goal:** Get gpsp compiling for N64 via libdragon, booting to a blank screen.

1. **Add N64 platform target to Makefile**
   - New `platform = n64` block alongside existing PSP/3DS/Vita/Switch targets
   - Use libdragon's `n64.mk` build system (MIPS III cross-compiler)
   - Define flags: `-DN64`, `-DSMALL_TRANSLATION_CACHE`, `-DMIPS_ARCH=3`
   - Set `MAX_ROM_SIZE` to something like 2 MB (page-cached window)

2. **Create `n64/` platform directory** with:
   - `n64_main.c` — entry point, hardware init, main loop
   - `n64_video.c` — framebuffer setup via libdragon display API
   - `n64_audio.c` — audio output via libdragon audio API
   - `n64_input.c` — controller input mapping
   - `n64_storage.c` — SD card ROM loading via libdragon's `dragonfs` or raw SD access

3. **Strip libretro dependency**
   - gpsp currently uses libretro as its frontend abstraction
   - Replace `libretro.c` callbacks with direct N64 hardware calls
   - Keep the core emulator (`cpu.cc`, `video.cc`, `sound.c`, `gba_memory.c`) untouched

### Phase 2: Memory Architecture Redesign

**Goal:** Fit the entire emulator in ~6 MB usable RAM (8 MB Expansion Pak minus framebuffers/stack/OS).

4. **ROM streaming from SD card**
   - Implement a demand-paging system: keep only N pages (e.g., 8-16 x 32 KB = 256-512 KB) of ROM in RAM
   - gpsp already has a `gamepak_file_large` demand-paging path — adapt this for libdragon's SD/filesystem API
   - Use LRU eviction for ROM pages
   - This is the single most important optimization

5. **Shrink dynarec translation caches**
   - ROM translation cache: 512 KB (down from 2 MB "small" mode)
   - RAM translation cache: 128 KB (down from 384 KB)
   - Add `TINY_TRANSLATION_CACHE` configuration tier
   - More aggressive cache flushing — evict least-recently-used blocks

6. **Eliminate SMC (self-modifying code) mirrors**
   - Currently EWRAM/IWRAM are doubled for SMC detection
   - Use page-level dirty tracking instead (single bit per 1 KB page)
   - Saves 256 KB (EWRAM) + 32 KB (IWRAM)

7. **Reduce/eliminate color conversion LUT**
   - `gba_cc_lut.c` is 263 KB of lookup tables
   - Either compute conversions at runtime (GBA's 15-bit RGB to N64's 16-bit RGBA) or use a smaller 32 KB approximation table
   - The N64 RDP could potentially do color conversion in hardware

8. **Shrink audio buffer**
   - Current: 128 KB (`s16 sound_buffer[65536]`)
   - N64: 16 KB buffer (4096 samples) — sufficient at libdragon's typical 44.1 KHz output
   - Ring buffer with smaller window

### Phase 3: MIPS III Dynarec Adaptation

**Goal:** Get the existing MIPS dynarec working on the VR4300.

9. **Audit `mips_emit.h` for MIPS III compatibility**
   - The existing MIPS dynarec targets MIPS32 R2 (PSP's Allegrex). The VR4300 is MIPS III (64-bit base, but we run in 32-bit mode)
   - Key differences:
     - No MIPS32 R2 instructions (`ext`, `ins`, `seh`, `seb`, `rotr`) — need fallback sequences
     - No `movn`/`movz` conditional moves (MIPS IV, not III) — use branch sequences
     - TLB differences (but we won't use TLB)
   - Guard R2-specific instructions behind `#ifndef N64` / `#ifdef MIPS_HAS_R2_INSTS`

10. **Cache coherency for JIT code**
    - N64's VR4300 has separate I-cache (16 KB) and D-cache (8 KB)
    - After writing JIT code to the translation cache, must invalidate I-cache
    - libdragon provides `data_cache_hit_writeback` and `inst_cache_hit_invalidate`
    - Add `n64_flush_cache(void *addr, size_t len)` to replace the 3DS/PSP equivalents

11. **Register allocation review**
    - Current MIPS dynarec uses s0-s7 for cached GBA state — this is fine on VR4300
    - Verify ABI compatibility with libdragon's MIPS O32/N32 calling convention
    - The `mips_stub.S` context switch code needs adaptation for N64's specific stack layout

12. **Translation cache placement**
    - JIT code must be in cacheable, executable memory
    - On N64, all RAM is executable — no DEP/NX concerns
    - Place translation caches at fixed addresses for predictability

### Phase 4: Video Output

**Goal:** Render GBA's 240x160 output to N64's 320x240 display.

13. **Framebuffer integration**
    - GBA renders to a 240x160 x 16-bit buffer internally
    - Blit this centered into N64's 320x240 framebuffer (40px horizontal padding, 40px vertical padding)
    - Use libdragon's `display_get()` / `display_show()` double-buffering API
    - Color format: GBA is RGB555, N64 is RGBA5551 — trivial bit shift (set alpha bit to 1)

14. **Optional: RDP-accelerated scaling**
    - Use the N64's RDP texture rectangle commands to upscale the GBA framebuffer
    - Could do 1.33x scale (240 to 320 width) with bilinear filtering for free
    - This is a nice-to-have, not required for initial port

### Phase 5: Audio Output

**Goal:** Get GBA audio playing through the N64.

15. **Audio driver using libdragon**
    - libdragon's audio subsystem provides a callback-based buffer fill model
    - gpsp's `sound.c` renders audio at 64 KHz internally — resample down to 44.1 KHz or 22.05 KHz
    - Use a small ring buffer (4096 samples) to bridge emulator output to audio callback
    - May need to drop to 22.05 KHz if CPU budget is too tight

16. **Audio-video sync**
    - GBA runs at 59.73 FPS — close enough to N64's 60 Hz NTSC
    - Sync audio generation to frame boundaries
    - If running behind, skip audio frames rather than accumulate latency

### Phase 6: Input

**Goal:** Map N64 controller to GBA inputs.

17. **Controller mapping**
    - Direct mapping via libdragon's `joypad` subsystem:

    | GBA | N64 |
    |-----|-----|
    | D-Pad | D-Pad |
    | A | A |
    | B | B |
    | L | L |
    | R | R |
    | Start | Start |
    | Select | Z |

    - C-buttons could serve as alternate face buttons or for emulator menu

### Phase 7: Storage & ROM Loading

**Goal:** Load GBA ROMs and save games from flashcart SD card.

18. **ROM loading from SD card**
    - Use libdragon's filesystem API (`dfs_open`, `fopen` on `sd://`)
    - ROM file browser UI using N64's display (simple text-based menu)
    - Load ROM header first, then demand-page the rest

19. **Save game support**
    - GBA saves (SRAM/Flash/EEPROM) are small (up to 128 KB)
    - Write save files to SD card
    - Periodic auto-save to avoid data loss on power-off
    - BIOS file also loaded from SD card (16 KB)

### Phase 8: Optimization & Profiling

**Goal:** Achieve playable frame rates (target: 30-60 FPS for simpler games).

20. **CPU budget analysis**
    - N64 CPU: 93.75 MHz. GBA CPU: 16.78 MHz
    - Dynarec overhead ratio is typically 3-8x, meaning we need ~50-135 MHz equivalent
    - **Simpler GBA games** (platformers, RPGs) should be feasible at full speed
    - **Complex games** (Mode 7 effects, heavy DMA) may need frameskip

21. **Optimization targets**
    - Hot path profiling of the dynarec output
    - Optimize GBA video rendering — skip offscreen scanlines
    - Frameskip support (render every 2nd or 3rd frame)
    - Audio: consider mono output to halve audio processing
    - Use N64's RSP for audio mixing (advanced — significant effort)

22. **Memory access optimization**
    - VR4300 cache is small (16 KB I / 8 KB D)
    - Keep hot data (register file, I/O registers, current IWRAM) in cached memory
    - ROM page cache will cause cache thrashing — minimize working set

---

## Risk Assessment

| Risk | Severity | Mitigation |
|------|----------|------------|
| RAM too tight even at 8 MB | **Critical** | Aggressive ROM streaming, tiny caches, drop features |
| CPU too slow for complex games | **High** | Frameskip, audio quality reduction, game compatibility list |
| SD card I/O latency for ROM streaming | **High** | Prefetch heuristics, larger page cache if RAM allows |
| MIPS III missing instructions vs MIPS32 R2 | **Medium** | Fallback instruction sequences (already partially handled) |
| Cache coherency bugs in JIT | **Medium** | Thorough cache flush after every JIT compilation |
| libdragon API limitations | **Low** | libdragon is mature and well-documented |

## Estimated Complexity by Phase

| Phase | Effort | Dependencies |
|-------|--------|-------------|
| 1. Build system | Low | libdragon toolchain |
| 2. Memory redesign | **High** | Core architecture changes |
| 3. MIPS III dynarec | **High** | Deep ISA knowledge |
| 4. Video | Low | libdragon display API |
| 5. Audio | Medium | libdragon audio API |
| 6. Input | Low | libdragon joypad API |
| 7. Storage | Medium | libdragon SD/filesystem |
| 8. Optimization | **High** | Profiling on real hardware |

## Summary

The port is **feasible but challenging**, primarily due to the N64's severe RAM constraints (8 MB for everything). The three key advantages are:

1. **Existing MIPS dynarec** — the hardest part of any emulator port is already done
2. **No FPU dependency** — gpsp uses all fixed-point math
3. **Proven embedded ports** — PSP and PS2 (both MIPS) demonstrate the core is portable to constrained hardware

The critical path is: **Memory redesign -> MIPS III dynarec adaptation -> ROM streaming**. Everything else is straightforward platform integration via libdragon.
