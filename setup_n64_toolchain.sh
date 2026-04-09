#!/bin/bash
# Setup script for gpSP N64 build environment
# Downloads and installs the libdragon toolchain + SDK
#
# Usage: ./setup_n64_toolchain.sh
# After running: make -f Makefile.n64

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TOOLCHAIN_DIR="$SCRIPT_DIR/toolchain/opt/libdragon"
DEB_URL="https://github.com/DragonMinded/libdragon/releases/download/toolchain-continuous-prerelease"
LIBDRAGON_SRC="https://github.com/DragonMinded/libdragon/archive/refs/heads/trunk.tar.gz"

# Detect architecture
ARCH=$(uname -m)
case "$ARCH" in
  x86_64|amd64)  DEB_ARCH="x86_64" ;;
  aarch64|arm64) DEB_ARCH="aarch64" ;;
  *)             echo "Unsupported architecture: $ARCH"; exit 1 ;;
esac

echo "=== gpSP N64 Toolchain Setup ==="
echo "Architecture: $ARCH ($DEB_ARCH)"
echo "Install to: $TOOLCHAIN_DIR"
echo ""

# Step 1: Download GCC cross-compiler
if [ -f "$TOOLCHAIN_DIR/bin/mips64-elf-gcc" ]; then
  echo "[1/3] GCC toolchain already installed, skipping."
else
  echo "[1/3] Downloading MIPS64 GCC toolchain..."
  mkdir -p /tmp/n64-toolchain-setup
  DEB_FILE="/tmp/n64-toolchain-setup/gcc-toolchain-mips64.deb"
  curl -L -o "$DEB_FILE" "$DEB_URL/gcc-toolchain-mips64-${DEB_ARCH}.deb"

  echo "  Extracting..."
  cd /tmp/n64-toolchain-setup
  ar x "$DEB_FILE"
  mkdir -p "$SCRIPT_DIR/toolchain"
  tar xzf data.tar.gz -C "$SCRIPT_DIR/toolchain"

  echo "  Verifying..."
  "$TOOLCHAIN_DIR/bin/mips64-elf-gcc" --version | head -1
fi

# Step 2: Download and build libdragon SDK
if [ -f "$TOOLCHAIN_DIR/mips64-elf/lib/libdragon.a" ]; then
  echo "[2/3] libdragon SDK already installed, skipping."
else
  echo "[2/3] Downloading and building libdragon SDK..."
  LIBDRAGON_TMP="/tmp/n64-toolchain-setup/libdragon"
  mkdir -p "$LIBDRAGON_TMP"
  curl -sL "$LIBDRAGON_SRC" | tar xz -C "$LIBDRAGON_TMP" --strip-components=1

  export N64_INST="$TOOLCHAIN_DIR"
  export PATH="$N64_INST/bin:$PATH"

  echo "  Building libdragon library..."
  make -C "$LIBDRAGON_TMP" -j$(nproc) 2>&1 | tail -3
  echo "  Installing..."
  make -C "$LIBDRAGON_TMP" install 2>&1 | tail -3

  echo "  Building host tools..."
  make -C "$LIBDRAGON_TMP/tools" -j$(nproc) 2>&1 | tail -3
  make -C "$LIBDRAGON_TMP/tools" install 2>&1 | tail -3
fi

# Step 3: Verify
echo "[3/3] Verifying installation..."
echo "  GCC: $($TOOLCHAIN_DIR/bin/mips64-elf-gcc --version | head -1)"
echo "  libdragon.a: $(ls -la $TOOLCHAIN_DIR/mips64-elf/lib/libdragon.a | awk '{print $5}') bytes"
echo "  n64tool: $(ls $TOOLCHAIN_DIR/bin/n64tool)"

echo ""
echo "=== Setup complete! ==="
echo ""
echo "To build the N64 ROM:"
echo "  make -f Makefile.n64"
echo ""
echo "Output: gpsp.z64"

# Cleanup
rm -rf /tmp/n64-toolchain-setup
