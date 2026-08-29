#!/usr/bin/env bash
# Export the ares instrumentation from /tmp into the repo.
#
# Run this after ANY edit to the ares tree. The counters this project
# measures with -- D-cache histograms, RDPWORK, the uncached-store cost
# knob, GPSP_PCPROF, ARESEMIT -- exist nowhere else, and /tmp does not
# survive a container restart.
set -eu
cd "$(dirname "$0")/.."
ARES_SRC="${ARES_SRC:-/tmp/ares_src}"
OUT="tools/ares-patches/gpsp64-instrumentation.patch"
[ -d "$ARES_SRC/.git" ] || { echo "no ares tree at $ARES_SRC"; exit 1; }
( cd "$ARES_SRC" && git diff ) > "$OUT"
echo "exported $(wc -l < "$OUT") lines from $ARES_SRC @ $(cd "$ARES_SRC" && git rev-parse --short HEAD)"
git --no-pager diff --stat -- "$OUT" || true
echo "now: git add $OUT && git commit"
