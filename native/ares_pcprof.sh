#!/usr/bin/env bash
# Where is the VR4300 actually executing?
#
# Every profile in this project so far has been derived: a counter here,
# a timer there, a residual computed by subtraction.  That works until
# two of the derived numbers disagree, and by then it is hard to tell
# which one is wrong.  This is the direct instrument -- ares samples the
# emulated PC every N instructions and the addresses are matched against
# the ELF's symbol table, so it reports where the CPU *is* with no model
# of where it ought to be.
#
#   native/ares_pcprof.sh <label> <sample-interval> [EXTRA_CFLAGS...]
set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
LABEL="${1:?usage: ares_pcprof.sh <label> <interval> [flags]}"; shift
EVERY="${1:-20000}"; shift || true
EXTRA="$*"

SCRATCH="/tmp/ares_pcprof/$LABEL"
ARES="${ARES_BIN:-/tmp/ares_src/build/rundir/bin/ares}"
: "${BENCH_STATE:=$REPO/bench-results/states/overworld.sav}"
[ -f "$BENCH_STATE" ] && EXTRA="$EXTRA -DN64_BOOT_STATE"

rm -rf "$SCRATCH"; mkdir -p "$SCRATCH"
( cd "$REPO" && tar -cf - --exclude=.git --exclude=toolchain --exclude=bench-results \
    --exclude=.bench-obj --exclude='*.o' --exclude='*.z64' --exclude='*.elf' \
    --exclude='*.dfs' --exclude='*.gba' . ) | ( cd "$SCRATCH" && tar -xf - )
ln -sfn "$REPO/toolchain" "$SCRATCH/toolchain"
mkdir -p "$SCRATCH/filesystem/gba"
cp "$REPO/Pokemon - Emerald Version (USA, Europe).gba" "$SCRATCH/filesystem/gba/emerald.gba"
[ -f "$BENCH_STATE" ] && cp "$BENCH_STATE" "$SCRATCH/filesystem/boot.sav"

echo "[pcprof:$LABEL] building ($EXTRA)..."
( cd "$SCRATCH" && export N64_INST="$REPO/toolchain/opt/libdragon" \
  && make -f Makefile.n64 ${EXTRA:+EXTRA_CFLAGS="$EXTRA"} -j"$(nproc)" >"$SCRATCH/build.log" 2>&1 ) \
  || { echo "BUILD FAILED"; tail -25 "$SCRATCH/build.log"; exit 1; }

DISP=":$((200 + RANDOM % 40))"
Xvfb "$DISP" -screen 0 640x480x24 >/dev/null 2>&1 & XPID=$!
sleep 2
echo "[pcprof:$LABEL] sampling every $EVERY instructions..."
( cd "$SCRATCH" && DISPLAY="$DISP" GPSP_PCPROF="$EVERY" timeout 240 \
    "$ARES" --setting Nintendo64/ExpansionPak=true --system "Nintendo 64" gpsp.z64 ) \
    >"$SCRATCH/ares.log" 2>&1
kill -9 "$XPID" 2>/dev/null; wait 2>/dev/null

grep '^PCPROF ' "$SCRATCH/ares.log" | awk '{print $2}' > "$SCRATCH/pcs.txt"
echo "[pcprof:$LABEL] $(wc -l < "$SCRATCH/pcs.txt") samples"
python3 "$REPO/tools/pcprof.py" "$SCRATCH/gpsp.elf" "$SCRATCH/pcs.txt" \
  | tee "$REPO/bench-results/$LABEL.pcprof.txt"
