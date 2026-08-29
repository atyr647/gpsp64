# Reproducing the build and measurement environment

Most of what this port depends on is committed. Three things are not, and
all three live on disposable storage:

| | where | size | if lost |
| --- | --- | --- | --- |
| patched ares | `/tmp/ares_src` | 2.9 GB | `tools/bootstrap-env.sh` |
| ares binary | `/tmp/ares_src/build/rundir/bin/ares` | 310 MB | same |
| N64 toolchain | `toolchain/` (gitignored) | 295 MB | `./setup_n64_toolchain.sh` |

Run **`tools/check-env.sh`** to confirm nothing is at risk, and
**`tools/bootstrap-env.sh`** to rebuild after a restart.

## Pinned versions

- **ares commit b80f67d** — `ares-emulator/ares`, master as of 2026-07-30.
  Configured `-G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
  -DARES_BUILD_LOCAL=ON -DARES_ENABLE_SDL=ON`, target `desktop-ui/ares`.
- **mips64-elf-gcc 16.2.0**, from libdragon's toolchain release.
- Host tools the harness scripts need: `Xvfb`, ImageMagick (`import`,
  `convert`), `gdb`, `ninja`, `cmake`.
- A Vulkan ICD. Software rendering via lavapipe
  (`/usr/share/vulkan/icd.d/lvp_icd.json`, package `mesa-vulkan-drivers`)
  is enough. **Do not set `VK_ICD_FILENAMES`** — pointing it at a wrong
  filename silently disables paraLLEl-RDP, which is why the RDP looked
  unvalidatable for a long stretch of this project.

## The one that is not pinned

`setup_n64_toolchain.sh` downloads from libdragon's
`toolchain-continuous-prerelease` tag. That URL moves. A rebuild can
therefore land on a different compiler and produce timings that are not
comparable with anything recorded in `Makefile.n64` or the docs — and the
difference would be silent, because the build would succeed.

`tools/check-env.sh` compares the installed compiler against the version
above and warns on a mismatch. If it ever fires, re-measure the baseline
before trusting any comparison against older numbers.

## The ares instrumentation is the fragile part

`tools/ares-patches/gpsp64-instrumentation.patch` is the only copy of the
counters this project measures with: the D-cache miss histograms at 64 KB
and 1 KB plus per-PC attribution, `RDPWORK`, `GPSP_UNCACHED_WCOST`,
`GPSP_RDP_CHARGE`, `GPSP_PCPROF` and `ARESEMIT`. Several conclusions rest
on them and on nothing else — that a third of the frame is cache stalls,
that the RDP renderer costs the RDP ~2 ms, that `rdpq_exec` is flat in
uncached-store cost while the rspq path is linear in it.

**After any edit to the ares tree, run `tools/save-ares-patch.sh` and
commit.** This already went wrong once: the saved patch had gone stale by
one counter, and a container restart would have taken it. `check-env.sh`
now hashes the live diff against the committed patch and fails if they
differ, so the same mistake reports itself instead of waiting to be
discovered.

## What is deliberately not preserved

Scratch build trees under `/tmp/aresbench`, `/tmp/jit`, `/tmp/ares_pcprof`
and `/tmp/ares_shot_build`. They are regenerated per run and reached 22 GB
in one session, which filled the disk mid-build. Delete them freely.

Benchmark logs in `bench-results/` are gitignored except for the
savestate and a few verification screenshots that were force-added. The
durable form of a result is the number quoted in a commit message or in
`docs/`, next to the conditions it was measured under — not the log.
