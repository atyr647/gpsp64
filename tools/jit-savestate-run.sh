#!/usr/bin/env bash
set -u
REPO=/home/user/gpsp64; LABEL="$1"; shift; EXTRA="${*:-}"
SCRATCH="/tmp/jit/$LABEL"; ARES=/tmp/ares_src/build/rundir/bin/ares
LOG="$SCRATCH/run.log"
rm -rf "$SCRATCH"; mkdir -p "$SCRATCH"
# NOTE: ares.log/run.log are excluded deliberately.  This script used to
# redirect ares output to a relative "ares.log", which lands in the *caller's*
# cwd (the repo), while the copy-back read "$SCRATCH/ares.log" -- a stale log
# the tar had just packed in from the repo.  Every result file was therefore a
# log from an earlier run.  Absolute path + exclusion, so it cannot recur.
( cd "$REPO" && tar -cf - --exclude=.git --exclude=toolchain --exclude=bench-results \
    --exclude='*.o' --exclude='*.z64' --exclude='*.elf' --exclude='*.dfs' --exclude='*.gba' \
    --exclude='ares.log' --exclude='run.log' . ) \
  | ( cd "$SCRATCH" && tar -xf - )
ln -sfn "$REPO/toolchain" "$SCRATCH/toolchain"
mkdir -p "$SCRATCH/filesystem/gba"
cp "$REPO/Pokemon - Emerald Version (USA, Europe).gba" "$SCRATCH/filesystem/gba/emerald.gba"
cp "$REPO/bench-results/states/overworld.sav" "$SCRATCH/filesystem/boot.sav"
( cd "$SCRATCH" && export N64_INST="$REPO/toolchain/opt/libdragon" \
  && make -f Makefile.n64 ${JITSAVE_MAKE:-} EXTRA_CFLAGS="-DN64_BOOT_STATE $EXTRA" -j"$(nproc)" >build.log 2>&1 ) \
  || { echo BUILD FAILED; tail -25 "$SCRATCH/build.log"; exit 1; }
# The ROM must be newer than the log, always.  Guard rather than trust.
rm -f "$LOG"
DISP=":$((140 + RANDOM % 20))"; Xvfb "$DISP" -screen 0 640x480x24 >/dev/null 2>&1 & XPID=$!
sleep 2
( cd "$SCRATCH" && DISPLAY="$DISP" GPSP_PCCYC="${GPSP_PCCYC:-}" GPSP_PCCYC_DUMP="${GPSP_PCCYC_DUMP:-}" timeout "${JITSAVE_TIMEOUT:-240}" "$ARES" \
    --setting Nintendo64/ExpansionPak=true --system "Nintendo 64" gpsp.z64 ) >"$LOG" 2>&1
kill -9 $XPID 2>/dev/null; wait 2>/dev/null
[ "$LOG" -nt "$SCRATCH/gpsp.z64" ] || { echo "STALE LOG -- refusing to publish"; exit 1; }
cp "$LOG" "$REPO/bench-results/$LABEL.jit.txt"
echo "[$LABEL] log -> bench-results/$LABEL.jit.txt ($(wc -l <"$LOG") lines)"
