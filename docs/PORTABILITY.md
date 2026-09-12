# What of this port works on games that are not Pokémon Emerald

Every measurement in this repository comes from one savestate of one game.
The mechanisms generalise to different degrees and the magnitudes do not
generalise at all. This is the honest accounting.

## The number that matters most

    RDP renderer path active     21.0 ms/frame   47.6 fps
    CPU renderer (gate refused)  48.0 ms/frame   20.8 fps

**2.3x.** Whether a game's frames pass the RDP renderer's acceptance gate
is the single biggest determinant of whether this port is playable on it.
Nothing else in the port comes close to that ratio.

## Fully general

Applies to any game, because none of it looks at game content.

- The I-cache placement work: the `keep.text.*` hot-chain group
  (`n64/n64_hotchain.h`), the dynarec memory-stub ordering, and
  `tools/hotchain.py` which stops either from silently regressing.
- Both cache-coherency fixes -- writing back the RSP's input records, and
  writing back `vram_swapped` before the RDP loads a slice. These are
  hardware-correctness bugs, not optimisations.
- Audio: mixing at the rate the hardware plays (22,050 Hz rather than
  64 KHz), the producer/consumer rate lock, and recomputing
  `frequency_step` on savestate load instead of restoring a value that
  bakes in the mixing rate of whatever build wrote the state.
- Dynarec: stub ordering, the indirect-branch cache, the SMC tag skew.
- `N64_AOT=` removing 554 KB of unreachable generated code.
- The measurement apparatus -- `-DN64_TEXTPAD`, `tools/pccyc.py`, the
  component timers, and `px1/prims` as a rendering invariant. Arguably the
  most portable thing here, since it is what makes any of the rest
  checkable.

## Engine-specific, not game-specific

- **The m4a HLE** (`N64_M4A_NATIVE`). It finds `SoundMainRAM` by code
  signature -- the `"Smsh"` ident plus a prologue match, scanned over
  IWRAM -- not by a hardcoded address, so it works on any game using
  Nintendo's m4a/Sappy driver. That is a large share of commercial GBA
  titles, but games with custom or third-party drivers get nothing from
  it.

## Conditional on content: the renderer gate

The RDP path, and everything built on top of it in this port -- the
bucketing, the `order_layers` deferral, the scanline split, the RSP tile
offload -- only applies to frames the gate accepts.

Emerald's overworld passes on **every** frame, with every refusal counter
at zero. That is close to a best case and should not be read as typical.

What the gate still refuses:

| condition | prospects |
| --- | --- |
| video mode != 0 | affine and bitmap modes; substantial renderer work |
| OBJ window enabled | per-pixel, cannot be resolved per row |
| 8bpp background tiles | mechanical (CI8 in TMEM) but halves TMEM capacity |
| mosaic | rare enough to ignore |
| per-row: affine, 8bpp, semi-transparent or OBJ-window sprites, or a row over gpSP's sprite budget | costs rows, not frames |

Two restrictions have been lifted:

- **Colour effects that are inert.** A configured-but-idle blend -- `BLDY`
  at zero, or empty target masks, or 100%/0% alpha coefficients -- no
  longer refuses the frame. The conditions are not invented here; they are
  exactly the ones `render_scanline_conditional_tile()` uses to decide an
  effect does nothing and fall through to regular rendering, so agreeing
  with it is the same test rather than a parallel argument about GBA
  semantics. Games commonly leave a fade configured between transitions,
  which is the state this catches.
- **Windows, per row rather than per frame.** A window used to refuse all
  160 rows. A row outside every window's vertical range sees exactly one
  layer set (`WINOUT`) and renders identically to an unwindowed row; only
  rows a window actually crosses need splitting horizontally, and those
  are the ones the CPU still takes. For a dialogue box over the bottom
  quarter of the screen this is the difference between 0% coverage and
  75%. The vertical and zero-width tests are taken from
  `render_window_n_pass` rather than re-derived.

Both are inert on Emerald's overworld -- it configures no effect and no
window -- so neither is measured here, only shown not to regress. They are
generalisations on the strength of the argument, not of a number.

## Emerald-specific leftovers

`n64/aot_targets.txt` and the hardcoded addresses in `n64/aot_hle.c`. Both
are effectively dead: the generated corpus is off by default and the
hand-written hooks are reachable only from the interpreter, which executes
zero instructions in a dynarec build.

## What would move the needle next

More gate coverage, and it cannot be prioritised responsibly from here.
Emerald's overworld passes everything, and its boot sequence draws no
background tiles at all, so this repository contains no workload that
exercises the remaining restrictions. Picking between "real windows",
"8bpp" and "mode 1" needs refusal counters from other ROMs; the counters
already exist (`PROF: rdpbg: refused frames:`) and cost nothing to read.

Before relaxing anything that is not provably a no-op, build the pixel
comparison the port does not yet have: render a frame both ways and
compare the results. The canary counts rows and tiles, and `px1/prims`
counts primitives and pixels; neither can see a wrong *colour*. That gap
is exactly how the `vram_swapped` writeback bug survived -- wrong texture
contents draw the same rectangles over the same pixels.

**That harness now exists** (`-DN64_RDPBG_PIXEL_VERIFY`, `n64/n64_video.c`
+ `video.cc`), and it found something on its first run, on Emerald's own
overworld, with no window and no gate relaxation involved.

## Open finding: the RDP path draws the wrong colours, right now, for Emerald

`-DN64_RDPBG_PIXEL_VERIFY` re-renders every RDP-owned row on the CPU into a
scratch buffer, at the exact point in `update_scanline()` the row would
otherwise have been drawn -- so register state (scroll, window, blend) is
whatever it genuinely was for that row, not an end-of-frame guess. After
the RDP finishes the frame (`n64_rdpbg_end()`, which calls
`rdpq_detach_wait()`), it reads the framebuffer back and compares,
converting the CPU's native BGR555 through the same `xbgr_pair_to_rgba_pair`
the blit uses.

Run against Emerald's overworld savestate: **every row disagrees.**
9,600 of 9,600 rows checked over one PROF window, ~2.16M of 2,304,000
pixels wrong. This is not a harness bug -- each of the following was
checked and ruled out before concluding that:

- **The ground truth is real.** Calling `render_scanline_window()` twice
  for the same row produces byte-identical output (diff=0), so it is
  deterministic and has no side effect this harness could be tripping
  over.
- **The buffer/stride arithmetic is exact.** The computed byte offsets
  into both the framebuffer and the scratch buffer matched their expected
  values precisely (`disp->stride`, the GBA offset math, the row stride).
- **Not a semi-transparent-OBJ blend.** `obj_alpha_count[vcount]` was 0 for
  the sampled row.
- **Not the inert-effect gate relaxation shipped earlier.** `BLDCNT` was
  `0x1e40` (alpha-blend mode, but zero 1st-target layers), which is
  genuinely inert by gpSP's own logic, not just this port's copy of it.
- **Not the window relaxation either.** `WIN0`/`WIN1` were both enabled in
  `DISPCNT` for this frame, but `WIN0` covers the entire screen (a
  pre-existing special case, unaffected by this session's per-row
  masking) and `WIN1` is zero-width (degenerate, correctly treated as
  always-out by both the old and new logic).
- **The TLUT's source data and bit layout are consistent** with
  `xbgr_pair_to_rgba_pair`'s documented format, and both read from the
  same `palette_ram_converted` array the CPU renderer itself uses.

What was *not* isolated: whether the RDP-side tile/palette **selection**
(`n64_rdp_bg.c`'s tilemap walk) picks a different tile or sub-palette than
gpSP's real renderer for the same screen position, or whether selection
agrees and the mismatch is in **TMEM/TLUT sampling** once the RDP has that
selection. Distinguishing those needs one further instrumentation pass:
capture the `(vt, pal)` the RDP's walk assigns to a specific screen
position and compare it against what the CPU renderer's own tile lookup
computes for the same position -- a different, more invasive hook than
this harness needed, and the next concrete step for whoever continues
this.

Whether this predates this session or was introduced by earlier RDP-path
work is not established either; the harness that could have caught it
did not exist until now. Given the size of the effect (essentially every
pixel, not a handful at tile edges), a plausible register or scale/shift
error in RDP command generation is more likely than a one-off rounding
bug, but that is a hypothesis, not a finding.

This does not roll back anything already shipped: the two gate relaxations
in this document are unaffected by it (both were shown inert on this
scene, not responsible for it), and the RDP path's pixel *counts* and
*positions* are still independently verified correct by `px1/prims` and
the row/tile canary. What is now in question is specifically pixel
*colour*, on every configuration, including the one this port has always
considered its best case.
