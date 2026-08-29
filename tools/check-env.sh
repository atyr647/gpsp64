#!/usr/bin/env bash
# Verify the build/measure environment is intact and reconstructible.
#
# This project depends on things that live outside git: a patched ares in
# /tmp, a 295 MB cross toolchain, a Vulkan ICD, an X server. Containers
# get restarted and /tmp gets reclaimed. Everything here is either
# committed or rebuildable by tools/bootstrap-env.sh -- this script checks
# that claim is still true, and in particular catches the one failure that
# is silent:
#
#   ares instrumentation edited in /tmp but not re-exported to the repo.
#
# That happened once already. The saved patch had gone stale by one
# counter and nobody would have noticed until the tree was gone.
#
# Exit status is the number of problems found.
set -u
cd "$(dirname "$0")/.."
REPO="$PWD"
ARES_SRC="${ARES_SRC:-/tmp/ares_src}"
PATCH="tools/ares-patches/gpsp64-instrumentation.patch"
PROBLEMS=0
note() { printf '  %-7s %s\n' "$1" "$2"; [ "$1" = FAIL ] && PROBLEMS=$((PROBLEMS+1)); return 0; }

echo "== committed assets (lost only if the repo is lost)"
for f in "Pokemon - Emerald Version (USA, Europe).gba" \
         "bench-results/states/overworld.sav" \
         "$PATCH" "setup_n64_toolchain.sh" "docs/ENVIRONMENT.md"; do
  if git ls-files --error-unmatch "$f" >/dev/null 2>&1; then note ok "$f"
  else note FAIL "$f is NOT tracked by git"; fi
done

echo "== unpushed work (lost if the container goes)"
if git rev-parse --abbrev-ref '@{u}' >/dev/null 2>&1; then
  AHEAD=$(git rev-list --count '@{u}..HEAD' 2>/dev/null || echo 0)
  [ "$AHEAD" = 0 ] && note ok "no unpushed commits" \
                   || note FAIL "$AHEAD commit(s) not pushed -- git push"
else
  note FAIL "branch has no upstream -- git push -u origin \$(git branch --show-current)"
fi
DIRTY=$(git status --porcelain | grep -vE '^\?\? |gpsp\.(map|z64|elf)' | wc -l)
[ "$DIRTY" = 0 ] && note ok "no uncommitted tracked changes" \
                 || note warn "$DIRTY uncommitted change(s) -- commit before a restart"

echo "== ares instrumentation"
if [ -d "$ARES_SRC/.git" ]; then
  LIVE=$(cd "$ARES_SRC" && git diff | git hash-object --stdin)
  SAVED=$(git hash-object "$PATCH")
  if [ "$LIVE" = "$SAVED" ]; then note ok "saved patch matches the live tree"
  else note FAIL "live ares diff differs from $PATCH -- run tools/save-ares-patch.sh"; fi
  HAVE=$(cd "$ARES_SRC" && git rev-parse --short HEAD)
  WANT=$(grep -oP 'ares commit \K[0-9a-f]+' docs/ENVIRONMENT.md 2>/dev/null | head -1)
  [ -n "$WANT" ] && { [ "$HAVE" = "$WANT" ] && note ok "ares at pinned commit $WANT" \
                      || note warn "ares at $HAVE, ENVIRONMENT.md pins $WANT"; }
else
  note warn "$ARES_SRC is gone -- rebuild with tools/bootstrap-env.sh (patch is committed)"
fi
[ -x "$ARES_SRC/build/rundir/bin/ares" ] && note ok "ares binary present" \
                                         || note warn "ares binary missing -- tools/bootstrap-env.sh"

echo "== rebuildable dependencies"
GCC="toolchain/opt/libdragon/bin/mips64-elf-gcc"
if [ -x "$GCC" ]; then
  V=$("$GCC" --version | head -1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+')
  W=$(grep -oP 'mips64-elf-gcc \K[0-9.]+' docs/ENVIRONMENT.md 2>/dev/null | head -1)
  [ "$V" = "$W" ] && note ok "toolchain gcc $V" || note warn "toolchain gcc $V, recorded $W"
else
  note warn "toolchain missing -- ./setup_n64_toolchain.sh"
fi
for p in Xvfb import gdb ninja cmake; do
  command -v "$p" >/dev/null || note warn "$p missing (harness scripts need it)"
done
ls /usr/share/vulkan/icd.d/lvp_icd.json >/dev/null 2>&1 \
  || note warn "no lavapipe Vulkan ICD -- ares runs without paraLLEl-RDP"

echo
[ "$PROBLEMS" = 0 ] && echo "OK: nothing at risk of permanent loss." \
                    || echo "$PROBLEMS problem(s) -- fix before the next restart."
exit "$PROBLEMS"
