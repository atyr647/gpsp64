#!/bin/bash
# Benchmark the real N64 ROM under ares.  This is the ONLY source of
# performance numbers for this project: the target is an N64, so the
# figures that matter are N64 frame times, not host instruction counts.
#
#   native/ares_bench.sh <label> [EXTRA_CFLAGS...]
#
# ARES_NORECOMP=1 disables ares's own CPU recompiler (exported as
# GPSP_NORECOMP; 'Recompiler' is an emulator option, not a settings
# node, and ares EXITS on an unrecognised --setting).  Essential when
# benchmarking the gpSP dynarec: our ROM writes MIPS code and executes it,
# which is the worst case for a recompiling host -- ares keeps discarding
# translations and a single emulated frame can take 15 minutes of wall
# clock.  With ares interpreting, the cost is uniform and iteration is
# usable.  The reported FPS comes from the VR4300 COUNT register, i.e.
# emulated time, so it stays valid either way.
#
# What it measures comes straight from the ROM's own PROF output, which
# is derived from the VR4300 COUNT register (CP0 $9, CPU/2 = 46.875 MHz)
# that ares emulates:
#
#   ms/f        wall time per emulated GBA frame  -> FPS
#   CPU/PPU/Blt where that time goes
#   cyc/i       VR4300 cycles per interpreted GBA instruction, i.e. how
#               expensive the interpreter itself is
#   ARM/Thm     instruction mix
#
# Emulation advances a fixed amount of work per frame regardless of how
# fast the host runs, so PROF window N is the same game state in every
# build.  Comparing like-indexed windows across builds is therefore a
# fair A/B.
#
# The GBA ROM is copied into a scratch build tree so the DFS auto-launch
# path works; it is never placed in the repo tree, so no copyrighted data
# can be committed by accident.
set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO"

LABEL="${1:?usage: ares_bench.sh <label> [EXTRA_CFLAGS...]}"; shift || true
# N64_OPT=-O3 etc. is forwarded as a make variable, not a CFLAG: it has
# to be applied through the same target-specific mechanism libdragon
# uses for -O2, or it is overridden.  See Makefile.n64.
EXTRA="$*"

ROM="${BENCH_ROM:-$REPO/Pokemon - Emerald Version (USA, Europe).gba}"
# NOTE: do NOT set VK_ICD_FILENAMES.  ares needs a Vulkan device for
# paraLLEl-RDP, and the loader finds one on its own if a driver is
# installed (mesa-vulkan-drivers provides lavapipe, a software device).
# Pointing VK_ICD_FILENAMES at a wrong filename overrides that search and
# silently disables the RDP -- which is why every run in this project
# printed "Vulkan init failed: No RDP rendering support", why
# rdpq_tex_blit quietly did nothing, and why the RDP looked
# unvalidatable.  With it working the baseline is unchanged (55.0 ms/f,
# nothing here issues RDP commands yet) but the RDP is now testable.
ARES="${ARES_BIN:-/tmp/ares_src/build/rundir/bin/ares}"
WINDOWS="${ARES_WINDOWS:-40}"      # PROF windows (60 frames each) to collect
TIMEOUT="${ARES_TIMEOUT:-420}"     # hard wall-clock cap, seconds
SCRATCH="/tmp/aresbench/$LABEL"
OUT="$REPO/bench-results"; mkdir -p "$OUT"
LOG="$OUT/$LABEL.ares.txt"

[ -x "$ARES" ] || { echo "ares not found at $ARES"; exit 1; }
[ -f "$ROM" ]  || { echo "GBA ROM not found: $ROM"; exit 1; }

echo "[ares:$LABEL] staging build tree..."
rm -rf "$SCRATCH"; mkdir -p "$SCRATCH"
# Copy only what the build needs; skip the repo's own build outputs.
tar -cf - --exclude=.git --exclude=toolchain --exclude=bench-results \
    --exclude=.bench-obj --exclude='*.o' --exclude='*.z64' --exclude='*.elf' \
    --exclude='*.dfs' --exclude='*.gba' . | (cd "$SCRATCH" && tar -xf -)
ln -sfn "$REPO/toolchain" "$SCRATCH/toolchain"
# Embedded ROM enables the single-ROM auto-launch path, so the benchmark
# needs no synthetic controller input at all.
cp "$ROM" "$SCRATCH/filesystem/gba/emerald.gba"

# BENCH_STATE=<file> starts the ROM from a captured gameplay state rather
# than from the boot sequence.  ares is slow enough that a run only ever
# reaches the first few hundred emulated frames -- BIOS decompression and
# the Game Freak logo -- which is not the workload this port is for.
# Produce the file with:
#   BENCH_SAVESTATE=/path/boot.sav BENCH_WARMUP=1500 native/bench.sh mkstate
# Default to the captured overworld state.  Booting the ROM instead
# spends the whole run on BIOS decompression, the Game Freak logo and the
# title screen -- a different workload with a different BG cost, which is
# why numbers from a boot run and numbers from this state are not
# comparable.  Every published figure for this port is from this state.
: "${BENCH_STATE:=$REPO/bench-results/states/overworld.sav}"
[ -f "$BENCH_STATE" ] || BENCH_STATE=""
if [ -n "${BENCH_STATE:-}" ]; then
  [ -f "$BENCH_STATE" ] || { echo "BENCH_STATE not found: $BENCH_STATE"; exit 1; }
  cp "$BENCH_STATE" "$SCRATCH/filesystem/boot.sav"
  EXTRA="$EXTRA -DN64_BOOT_STATE"
  echo "[ares:$LABEL] booting from savestate $BENCH_STATE"
fi

echo "[ares:$LABEL] building N64 ROM${EXTRA:+ ($EXTRA)}..."
( cd "$SCRATCH" \
  && export N64_INST="$REPO/toolchain/opt/libdragon" PATH="$REPO/toolchain/opt/libdragon/bin:$PATH" \
  && make -f Makefile.n64 clean >/dev/null 2>&1 \
  && make -f Makefile.n64 ${EXTRA:+EXTRA_CFLAGS="$EXTRA"} ${N64_OPT:+N64_OPT="$N64_OPT"} ${AOT_OPT:+AOT_OPT="$AOT_OPT"} ${CPU_OPT:+CPU_OPT="$CPU_OPT"} ${PROFILE_FLAGS+PROFILE_FLAGS="$PROFILE_FLAGS"} -j"$(nproc)" >"$SCRATCH/build.log" 2>&1 ) \
  || { echo "BUILD FAILED"; tail -25 "$SCRATCH/build.log"; exit 1; }

TEXT=$("$REPO/toolchain/opt/libdragon/bin/mips64-elf-size" "$SCRATCH/gpsp.elf" | awk 'NR==2{print $1}')
echo "[ares:$LABEL] .text = $TEXT bytes"

DISP=":$((90 + RANDOM % 50))"
Xvfb "$DISP" -screen 0 640x480x24 >/dev/null 2>&1 &
XPID=$!
sleep 2

echo "[ares:$LABEL] running ares (up to ${TIMEOUT}s, want $WINDOWS PROF windows)..."
( cd "$SCRATCH" && DISPLAY="$DISP" timeout "$TIMEOUT" \
    "$ARES" --setting Nintendo64/ExpansionPak=true \
      --setting Developer/HomebrewMode="${ARES_HOMEBREW:-false}" \
      --system "Nintendo 64" gpsp.z64 ) \
    >"$SCRATCH/ares.log" 2>&1 &
APID=$!

# Stop as soon as enough windows are captured; no need to burn the full timeout.
for _ in $(seq "$TIMEOUT"); do
  sleep 1
  n=$(grep -c '^PROF: CPU' "$SCRATCH/ares.log" 2>/dev/null); n=${n:-0}
  [ "$n" -ge "$WINDOWS" ] && break
  kill -0 "$APID" 2>/dev/null || break
done
kill -9 "$APID" 2>/dev/null; kill -9 "$XPID" 2>/dev/null; wait 2>/dev/null

cp "$SCRATCH/ares.log" "$LOG"
echo "  .text = $TEXT bytes" >> "$LOG"

python3 "$REPO/tools/parse_prof.py" "$LOG" --label "$LABEL" --text "$TEXT"
