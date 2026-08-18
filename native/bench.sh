#!/bin/bash
# Build and run the native benchmark harness.
#
#   native/bench.sh [label] [extra-cflags...]
#
# Measures emulated work per GBA frame (interpreted instructions, AOT
# coverage, hot pages).  See native/native_main.c for why interpreted
# instructions/frame is the metric that tracks real N64 framerate.
#
# Results go to bench-results/<label>.txt and the page histogram to
# bench-results/<label>.pages so tools/suggest_targets.py can consume it.
# Re-run with a different label and diff to see the effect of a change.
set -u
cd "$(dirname "$0")/.."

LABEL="${1:-baseline}"
shift || true
EXTRA="$*"

ROM="${BENCH_ROM:-Pokemon - Emerald Version (USA, Europe).gba}"
BIOS="${BENCH_BIOS:-bios/open_gba_bios.bin}"
FRAMES="${BENCH_FRAMES:-1800}"   # 30s of GBA time at steady state
WARMUP="${BENCH_WARMUP:-1200}"   # skip BIOS decompress + intro cutscene

OUT=bench-results
mkdir -p "$OUT"

CFLAGS="-DN64 -DPROFILE_AOT -I. -Inative -O2 -w -ffunction-sections -fdata-sections $EXTRA"
CXXFLAGS="$CFLAGS -fno-rtti -fno-exceptions"
OBJ=".bench-obj/$LABEL"
rm -rf "$OBJ"; mkdir -p "$OBJ/mips" "$OBJ/n64"

echo "[bench:$LABEL] compiling${EXTRA:+ ($EXTRA)}..."
for f in main gba_memory sound cheats serial gbp rfu serial_proto gba_cc_lut memmap; do
  gcc $CFLAGS -c "$f.c" -o "$OBJ/$f.o" || { echo "FAILED: $f.c"; exit 1; }
done
for f in cpu video; do
  g++ $CXXFLAGS -c "$f.cc" -o "$OBJ/$f.o" || { echo "FAILED: $f.cc"; exit 1; }
done
for f in n64/aot_hle n64/aot_generated; do
  gcc $CFLAGS -c "$f.c" -o "$OBJ/$f.o" || { echo "FAILED: $f.c"; exit 1; }
done
gcc $CFLAGS -c native/native_main.c -o "$OBJ/native_main.o" || exit 1
gcc -I. -c bios_data.S -o "$OBJ/bios_data.o" || exit 1

g++ -Wl,--gc-sections -o "$OBJ/bench" "$OBJ"/*.o "$OBJ"/n64/*.o -lm 2>/dev/null \
  || g++ -Wl,--gc-sections -o "$OBJ/bench" $(find "$OBJ" -name '*.o') -lm \
  || { echo "LINK FAILED"; exit 1; }

# Report generated-AOT size: expanding coverage costs N64 ROM/RAM, so
# track it alongside the speed win.
AOTSZ=$(wc -c < n64/aot_generated.c)
AOTFN=$(grep -c '^static u32 aot_gen_' n64/aot_generated.c || echo 0)

echo "[bench:$LABEL] running $FRAMES frames (warmup $WARMUP)..."
"$OBJ/bench" "$ROM" "$FRAMES" "$BIOS" --bench --warmup "$WARMUP" \
    --input "${BENCH_INPUT:-mash}" --pages "$OUT/$LABEL.pages" > "$OUT/$LABEL.txt" 2>&1
RC=$?

echo "  aot_generated.c: $AOTSZ bytes, $AOTFN functions" >> "$OUT/$LABEL.txt"

if [ $RC -ne 0 ]; then
  echo "[bench:$LABEL] RUN FAILED (rc=$RC)"; tail -20 "$OUT/$LABEL.txt"; exit $RC
fi

sed -n '/=== BENCH/,$p' "$OUT/$LABEL.txt"
echo "  aot_generated.c: $AOTSZ bytes, $AOTFN functions"
echo "[bench:$LABEL] full output: $OUT/$LABEL.txt"
