# Where the remaining performance is, and what it would take

Written after the port reached 22.0 ms/frame silent (45.5 fps) and 24.0 ms
with audio (41.7 fps), when the question became whether tuning the current
design could reach 60 fps or whether a different split of work was needed.

The short answer: the current design is close to its ceiling, a different
split of work is not, and the gap between them is about 6 ms of a 22 ms
frame. This is what the measurements say and what each option would cost.

## The finding that reframes everything

A cycle-weighted PC profile (`GPSP_PCCYC`, `tools/pccyc.py`) of the 22.0 ms
frame:

| share | where |
| --- | --- |
| 22.4% | translated code (ROM blocks) -- the actual GBA emulation |
| 15.5% | `n64_rdpbg_flush` -- sort + RDP command emission |
| 11.6% | `n64_rdpbg_frame_end` -- tilemap walk (emit_bg/emit_objs inlined) |
| 8.1% | `update_gba` -- event scheduling |
| 7.9% | `sound_timer` -- DirectSound FIFO resampling |
| 5.4% | `update_scanline` |
| 3.5% | `render_gbc_sound` -- PSG channels |
| 3.3% | `bios_hle_swi` |
| ~22% | everything else, nothing above 2.3% |

**Only 22% of the frame is emulating the GBA.** The rest is the emulator's
own machinery, and two thirds of that machinery is work the N64 has
dedicated silicon for: display-list generation and audio mixing.

Meanwhile the RSP is nearly idle. It runs the per-tile rectangle encoding
(~1257 tiles at ~50 scalar instructions is ~63K of the ~1.4M RSP cycles a
22 ms frame affords, under 5%), and the CPU blocks on
`rspq_syncpoint_wait` for **0.04 ms/frame**. The second processor is
doing almost nothing while the first is the bottleneck.

## The number that decides it

`-DN64_ABLATE_RDPBG` (video.cc) skips `rdpbg_frame_end` entirely -- no
tilemap walk, no sort, no command emission, nothing drawn. It is safe to
ablate because the function is output-only: everything the game can
observe is computed in `update_scanline` before it runs, and the emulated
state is provably identical (same SWI dispatch count, same JIT flush
count, same instruction counts).

    normal      22.0 ms/f   45.5 fps    blit 16.91M ticks/window
    ablated     17.0 ms/f   58.8 fps    blit  0.13M

Display-list generation costs **5.97 ms/frame of CPU**, 27% of the frame.
With it gone the same emulator runs at 58.8 fps.

That is the ceiling for moving it, not the expected result -- a real RSP
implementation leaves the rdpq state calls, the TMEM uploads and some
driving logic on the CPU. But it establishes that the target is reachable
in principle, which the profile alone could not.

## The options, with bounds

### A. Display-list generation to the RSP -- up to 6.0 ms

The RSP already does the per-tile arithmetic (`rsp_rdpbg.S`). What remains
on the CPU is the tilemap walk, the counting sort, and the batch loop.

The obstacle is DMEM: 4 KB total, of which this overlay uses ~3.1 KB for
128 input records and their 2 KB of output. A layer is 651 tiles and the
sort is over the whole layer, so **the RSP cannot sort** -- it cannot hold
the list. That is a hard architectural constraint, not a microcode
problem.

Two ways around it:

1. **Bucket during the walk.** Append each record to a chained block per
   `(slice,palette)` key as the walk produces it, so the sort and the
   gather both disappear and what the RSP consumes is already contiguous.
   This also fixes a cost the CPU pays today: the pipeline makes three
   passes over a ~10 KB list against an 8 KB D-cache (walk writes it, sort
   permutes it, emit gathers it). See the note in `n64/n64_rdp_bg.c` --
   carrying records through the sort instead was tried and is a wash,
   because it moves the pass rather than removing it.
2. **Move the walk too.** The tilemap is DMA-able a row at a time (64
   bytes, 32 entries) and `vram_tile_nz` is a 384-byte bitmap that fits in
   DMEM comfortably. The RSP could produce records directly, with the CPU
   handing over only the BG control registers and scroll values. Combined
   with (1) the CPU's share of the renderer approaches zero.

Realistic landing: 4.0-4.5 ms of the 6.0 recovered. Largest piece of work
in the port to date.

**Update.** Option 1 is done, and it moved the goalposts. Bucketing during
the walk, plus tightening the walk's inner loop to advance a tilemap
pointer instead of recomputing an address per tile, took the renderer's
CPU cost from 6.23 ms to 4.84 ms -- a 22% cut, consistent across three
`.text` layouts. The breakdown also shifted: the sort is gone (0.82 ->
0.03 ms) and the emit loop no longer touches records (2.45 -> 1.51 ms), so
what is left is almost entirely the walk itself at ~3.3 ms.

That is exactly the piece option 2 moves, and it is now 68% of the
renderer rather than 46% of it. But the prize shrank with the cost: the
ceiling for moving display-list generation is now ~4.8 ms, not 6.0.

### B. Audio mixing to the RSP -- 2.5-3.5 ms

`sound_timer` (7.9%) and `render_gbc_sound` (3.5%) are 11.4% of the silent
frame and more with `-DN64_AUDIO_OUT`, because with audio off `SNDBUF` is
masked to 512 entries and the writes stay in cache.

libdragon ships `rsp_mixer.S` -- a 32-channel RSP mixer with resampling,
volume and pan, designed to coexist with rspq. The fit is good but not
uniform:

- **DirectSound A/B** is the larger half (`sound_timer`) and maps almost
  directly: an 8-bit PCM stream resampled to the output rate is exactly
  what the mixer does. This is the piece to do first.
- **The four PSG channels** are synthesis, not playback. Square and noise
  can be driven as looping waveforms with per-channel frequency, and the
  wave channel is a 32-sample loop; envelope and sweep become periodic
  volume updates from the CPU at event granularity instead of per-sample.
  That is an approximation of gpSP's per-sample model and may be audible.

Smaller and better-isolated than A, and the infrastructure exists.

### C. Dynarec code layout -- 1-2 ms, speculative

57% of instruction-cache misses are dynarec-generated code, and blocks are
laid out in translation order, so hot blocks are scattered across a 2 MB
cache. A trace layout -- re-emitting hot blocks contiguously -- is standard
JIT practice and unexplored here. The placement work in
`docs/CACHE_PROFILING.md` showed how much contiguity is worth for `.text`;
this is the same idea applied to the code the JIT writes.

Unquantified, unlike A and B. Worth a measurement before a design.

## What is already ruled out, with evidence

Do not re-try these; each was measured, not argued.

| idea | result |
| --- | --- |
| AOT + dynarec hybrid | **+12-15% slower** at three `.text` layouts; the dynarec has since gained the ibcache, stub ordering and hot-chain placement, so AOT's native C now loses to it while still paying the hook round trip. `Makefile.n64` |
| Growing the hot-`.text` group past 62% | slower at all three layouts (+5.6%, +0.7%, +1.3%); the group's span is index space claimed against everything outside it. `n64/n64_hotchain.h` |
| Carrying records through the sort | a wash: emit -0.63 ms, sort +0.70 ms. The cost is the pass over 10 KB, not which pass. `n64/n64_rdp_bg.c` |
| ARM dead-flag elimination | correct and bit-identical, measured as a regression: moved cost from generated code into `.text`. `docs/DYNAREC.md` |
| Frame reuse / frameskip | rejected on product grounds -- displayed fps is the figure that matters |
| Rewriting `rdpbg_regs_match()` as six masked 32-bit compares | neutral: 60.87M -> 60.41M, inside noise. The per-scanline register poll is not where `update_scanline` spends its time, which is worth knowing before optimising it again |
| Splitting `update_scanline` into a small hot path and a 3.8KB out-of-line body | ambiguous and not shipped: `ppu` median 4.63M -> 4.23M but two of three whole-frame points got worse. It also shrinks the hot-chain group from 62% to 39% of the I-cache, so it confounds two effects at once |

The last two are the shape of what is left at this level: real changes whose
effect is smaller than a three-layout median can resolve. Anything further
in this direction needs either a bigger sample (eight layouts, not three)
or a component timer aimed directly at the thing being changed.

## Where this lands

Starting from 24.0 ms with audio:

    today                                   24.0 ms   41.7 fps
    + A at the realistic 4.25 ms            19.8 ms   50.6 fps
    + B at 3.0 ms                           16.8 ms   59.6 fps
    + C if it delivers 1.5 ms               15.3 ms   65.4 fps

A and B together put 60 fps with sound right at the boundary -- reachable,
but not with margin, and the estimate for A carries the most uncertainty
because how much CPU residue an RSP renderer leaves is a design outcome
rather than something the ablation can predict.

The honest summary: **tuning the current design is finished; changing
which processor does the work is not.** The N64's answer to this problem
has always been that the CPU runs the game and the RSP builds the display
list. This port still has the CPU doing both.
