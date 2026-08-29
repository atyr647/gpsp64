# ares instrumentation used by this port

Every performance figure in this project comes from ares, and several of
them come from counters ares does not ship with. This patch is those
counters. It applies to the ares tree the benchmarks run against
(`/tmp/ares_src` in the development container, master as of 2026-07-30).

```
cd <ares checkout> && git apply .../gpsp64-instrumentation.patch
cd build && ninja desktop-ui/ares
```

Without it the benchmark scripts still work — they only need the frame
time ares already prints — but `DMISS`, `DMISSFINE`, `DMISSPC`,
`RDPWORK`, `GPSP_UNCACHED_WCOST` and `GPSP_PCPROF` all disappear, and
with them the ability to answer why a change did or did not help. See
docs/CACHE_PROFILING.md.

## What it adds

| | |
| --- | --- |
| `DMISS` / `DMISSFINE` / `DMISSPC` | D-cache miss histograms at 64 KB and 1 KB, plus attribution to the instruction that caused each miss |
| `IMISS` | I-cache hit/miss counts (partial — see the caveat in the docs) |
| `RDPWORK` | RDP workload per sync: primitives, covered pixels by cycle type, TMEM words, and a cycle estimate from a stated model |
| `GPSP_RDP_CHARGE=1` | advance the RDP thread's clock by that estimate, so waiting on the RDP costs something |
| `GPSP_UNCACHED_WCOST` / `RCOST` | charge N CPU cycles per uncached CPU access to RDRAM (default 0) |
| `GPSP_PCPROF=<n>` | sample the emulated PC every n instructions, for `native/ares_pcprof.sh` |
| `ARESEMIT` | how often ares recompiles a guest block — the check that ruled out ares thrashing on the gpSP dynarec's self-modifying code |

**`GPSP_PCPROF` is instruction-weighted, not time-weighted.** It samples
every N *executed instructions*, so it overstates code that runs many
cheap predictable instructions and understates code that stalls. On the
dynarec, a 21.6% share of samples turned out to be 2% of cycles. Use it
to find what runs; never to rank what costs.

All of it is off or zero by default, so a stock build of this patch
reproduces the same frame times as unpatched ares. That is deliberate:
the numbers recorded in `Makefile.n64` and the docs were taken over many
months and have to stay comparable.

## Why patch the emulator at all

Because the alternative is guessing. `Makefile.n64` carried an explicit
unsolved note for a long time — the interpreter costing ~200 VR4300
cycles per instruction where scalar work predicts 20-40, "the mechanism
is not established" — and four separate attempts to fix it by removing
work from the loop all measured neutral. The answer (about a third of the
frame is cache-miss stalls) was sitting in a counter ares already
maintained and never printed.
