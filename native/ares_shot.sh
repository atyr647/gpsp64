#!/usr/bin/env bash
# Capture what ares is actually showing.
#
# ares_bench.sh answers "how fast" and says nothing about "correct".  With
# the BG renderer moving to the RDP, a build can get faster by drawing the
# wrong thing -- or nothing -- so every speed number from here on needs a
# picture next to it.
#
#   native/ares_shot.sh <label> <seconds> [EXTRA_CFLAGS...]
#
# Boots the same savestate ares_bench.sh uses, waits, and writes
# bench-results/<label>.png from the Xvfb root window.
set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
LABEL="${1:?usage: ares_shot.sh <label> <seconds> [EXTRA_CFLAGS...]}"; shift
WAIT="${1:?need seconds}"; shift || true
EXTRA="$*"

SCRATCH="/tmp/ares_shot_build/gpsp64_shot"
ARES="${ARES_BIN:-/tmp/ares_src/build/rundir/bin/ares}"
OUT="$REPO/bench-results/$LABEL.png"

rm -rf "$SCRATCH"; mkdir -p "$SCRATCH"
( cd "$REPO" && tar -cf - --exclude=.git --exclude=toolchain --exclude=bench-results \
    --exclude=.bench-obj --exclude='*.o' --exclude='*.z64' --exclude='*.elf' \
    --exclude='*.dfs' --exclude='*.gba' . ) | ( cd "$SCRATCH" && tar -xf - )
ln -sfn "$REPO/toolchain" "$SCRATCH/toolchain"
mkdir -p "$SCRATCH/filesystem/gba"
# Same single-ROM auto-launch path ares_bench.sh uses, so no synthetic
# controller input is needed to reach gameplay.
cp "$REPO/Pokemon - Emerald Version (USA, Europe).gba" "$SCRATCH/filesystem/gba/emerald.gba"
: "${BENCH_STATE:=$REPO/bench-results/states/overworld.sav}"
[ -f "$BENCH_STATE" ] || BENCH_STATE=""
if [ -n "${BENCH_STATE:-}" ]; then
  cp "$BENCH_STATE" "$SCRATCH/filesystem/boot.sav"; EXTRA="$EXTRA -DN64_BOOT_STATE"
fi

echo "[shot:$LABEL] building ($EXTRA)..."
( cd "$SCRATCH" && export N64_INST="$REPO/toolchain/opt/libdragon" \
  && make -f Makefile.n64 ${EXTRA:+EXTRA_CFLAGS="$EXTRA"} -j"$(nproc)" >"$SCRATCH/build.log" 2>&1 ) \
  || { echo "BUILD FAILED"; tail -25 "$SCRATCH/build.log"; exit 1; }

DISP=":$((150 + RANDOM % 40))"
Xvfb "$DISP" -screen 0 1280x960x24 >/dev/null 2>&1 & XPID=$!
sleep 2
( cd "$SCRATCH" && DISPLAY="$DISP" timeout $((WAIT + 30)) \
    "$ARES" --fullscreen --kiosk --setting Nintendo64/ExpansionPak=true \
      --system "Nintendo 64" gpsp.z64 ) \
    >"$SCRATCH/ares.log" 2>&1 & APID=$!
sleep "$WAIT"
DISPLAY="$DISP" import -window root "$OUT" 2>/dev/null \
  || { DISPLAY="$DISP" xwd -root > /tmp/shot.xwd && convert /tmp/shot.xwd "$OUT"; }
kill -9 "$APID" 2>/dev/null; kill -9 "$XPID" 2>/dev/null; wait 2>/dev/null
cp "$SCRATCH/ares.log" "$REPO/bench-results/$LABEL.shot.txt"
echo "[shot:$LABEL] -> $OUT"
