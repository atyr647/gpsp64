#!/usr/bin/env bash
# Rebuild everything this project needs that is not in git.
#
# Safe to re-run; each step is skipped if already satisfied. After this,
# tools/check-env.sh should report nothing at risk.
set -eu
cd "$(dirname "$0")/.."
REPO="$PWD"
ARES_SRC="${ARES_SRC:-/tmp/ares_src}"
ARES_COMMIT="$(grep -oP 'ares commit \K[0-9a-f]+' docs/ENVIRONMENT.md | head -1)"
PATCH="$REPO/tools/ares-patches/gpsp64-instrumentation.patch"

echo "== [1/3] N64 toolchain"
if [ -x toolchain/opt/libdragon/bin/mips64-elf-gcc ]; then
  echo "   present: $(toolchain/opt/libdragon/bin/mips64-elf-gcc --version | head -1)"
else
  ./setup_n64_toolchain.sh
  echo "   NOTE: setup_n64_toolchain.sh pulls libdragon's continuous-prerelease"
  echo "   toolchain, which is not pinned.  If the compiler version differs from"
  echo "   the one in docs/ENVIRONMENT.md, published timings are not comparable."
fi

echo "== [2/3] ares, patched"
if [ ! -d "$ARES_SRC/.git" ]; then
  echo "   cloning ares @ $ARES_COMMIT"
  git clone https://github.com/ares-emulator/ares.git "$ARES_SRC"
  ( cd "$ARES_SRC" && git checkout -q "$ARES_COMMIT" )
fi
if ( cd "$ARES_SRC" && git diff --quiet ); then
  echo "   applying $PATCH"
  ( cd "$ARES_SRC" && git apply "$PATCH" )
else
  echo "   tree already modified; leaving it alone"
  ( cd "$ARES_SRC" && git apply --check "$PATCH" 2>/dev/null ) \
    && echo "   (saved patch would still apply cleanly)" || true
fi

echo "== [3/3] building ares (this takes a while)"
if [ ! -x "$ARES_SRC/build/rundir/bin/ares" ]; then
  cmake -S "$ARES_SRC" -B "$ARES_SRC/build" -G Ninja \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DARES_BUILD_LOCAL=ON -DARES_ENABLE_SDL=ON
fi
ninja -C "$ARES_SRC/build" desktop-ui/ares
echo
echo "done.  verify with: tools/check-env.sh"
