# CRT stage: render order

The CRT stage (`core/stages/crt/crt.cpp`) runs the bundled RetroArch slang presets through librashader. A
host renders frames in any order (scrubbing, render-queue chunks, Fusion rendering a frame several times,
several instances of the effect), so a frame's picture must be a function of the frame alone. This page says
which presets keep state between frames, why one of them didn't give that, and the rule that does.
Test: `tests/crt/test_crt_order.cpp` (`test_crt_order`, `test_crt_order_unshared` in ctest).

## Which presets keep state

A preset keeps state when a pass reads the previous frame: `PassFeedbackN`, `OriginalHistoryN` or
`<alias>Feedback` (`uses_history` in `crt.cpp` scans the preset's shaders for them). Of the 7 bundled presets
only **crtsim** (`sim`) does: its first pass, `crtsim/composite.slang`, reads its own previous output
(`PassFeedback0`) for the persistence / bleed effect:

```
Cur = saturate(max(Cur, Persistence * 10 / (1 + 2 * Bleed) * (Prev + Bleed * (PrevLeft + PrevRight))))
```

Royale, aperture, easymode, glow_gauss, glow_lanczos and hyllian read nothing from earlier frames
(`test_crt_order` checks all 7, with the plug-in's default look and alone).

## The bug (fixed 2026-11-12)

A sim frame depended on the frames rendered before it: two Resolve sessions of the same CPU configuration
differed by up to 97/255 (default look + sim), 243/255 with its params pushed; in the core alone, the same
frame rendered after another frame, three times in a row and first in a fresh instance gave three pictures.
Causes:

1. **librashader's `clear_history` doesn't clear the feedback.** `FilterChainD3D11::frame`
   (`vendor/librashader/librashader-runtime-d3d11/src/filter_chain.rs`, `if options.clear_history`) clears
   `history_framebuffers` (OriginalHistory) only; `feedback_framebuffers` keep the last frame's pass outputs
   (Metal and the other runtimes do the same). The stage's "seek" path rendered the frame's own input once at
   frame count - 1 with `clear_history` to prime the feedback, then the frame: but that prime itself read
   `PassFeedback0`, i.e. the last frame rendered, whatever it was. A repeated frame primed again over its own
   output (so the 2nd and 3rd render differed); a fresh chain read new framebuffers, whose contents D3D11 leaves
   undefined (zero in practice).
2. **Playing fed the real previous frame.** When the frame came right after the previous one (frame count + 1)
   the stage skipped the prime, so a played frame showed the previous frame's afterglow and a sought one didn't.
3. On Metal, both frames were encoded into one command buffer; librashader's Metal runtime keeps each pass's
   uniforms in one shared buffer read when the GPU runs it, so the prime ran with the frame's own uniforms.

## The rule

Upstream (`Mp4Exporter`, `Pipeline.encode`) renders a clip strictly in order on a fresh chain: frame 0 sees a
zeroed feedback, every later frame the previous frame's composite output. That history is unbounded (with
`Tuning_Persistence_*` >= 0.1 the feedback weight is >= 1 and never fades), it needs every earlier frame's
chain input (Digital video, NTSC, Glitch of each earlier frame), and the stage only ever sees the current frame.
Exact seek = play is therefore out of reach, so both follow one fixed priming rule:

**Every sim frame, played or sought, is two chain frames on the same chain: first the frame's own chain input
with the frame's own params except `Tuning_Persistence_R/G/B = 0`, at frame count n - 1; then the frame itself
with its params at frame count n** (n = `shader_frame` = frame + 1, upstream's export numbering).

With persistence 0 the composite is `max(Cur, 0 * ...) = Cur` for any previous value (its framebuffer is RGBA8,
so always finite): exactly the picture a zeroed feedback gives, whatever ran before. The second frame then sees
the first one's composite as its "previous frame". So a sim frame = upstream's second frame of a clip made of
this frame held: the first frame of every clip and every seek look as they did (the upstream golden
`testpattern-crtsim` and every self-golden are unchanged, byte for byte), and the persistence / bleed controls
still shape the frame (its own afterglow and horizontal bleed). What it can't show any more is upstream's
trail of earlier frames: moving content (and moving VHS noise) leaves no afterglow. The alpha channel of the
composite keeps a constant weight 1 in that formula, but no pass reads it (screen samples `.rgb`), so no
output byte depends on it. Parameter values for the reset live in `feedback_resets()` (`crt.cpp`), by preset.

On Metal a `Device::finish()` separates the two frames (point 3). With a host's own Metal command buffer
(`ntscrt_render_metal`, not used by the OpenFX plug-in) the stage can't wait, so there the reset frame may run
with the frame's uniforms.

A temporal preset with no entry in `feedback_resets()` (none bundled: only a `.slangp` added to the resources)
keeps the old prime-on-seek path, which is not order-independent; `CrtStage::ran_temporal_preset()` then tells
Screen Loop to keep no checkpoints. With crtsim, Screen Loop now keeps its checkpoints (its TV pictures are a
function of the pass).

## What it costs

RTX 5090, Release. The stage alone (`test_crt "CRT frame timings"`, `NTSCRT_CRT_TIMINGS=1`; one frame finished
on the GPU, median of 100; sim from a 640x480 chain input), before -> after:

| output | played | sought (every other frame) |
| --- | --- | --- |
| 3840x2160 | 0.22 -> 0.28..0.31 ms | 0.29..0.31 -> 0.28..0.33 ms |
| 1920x1080 | 0.15 -> 0.22..0.23 ms | 0.22 -> 0.22 ms |

Played frames now cost what seeks always did (one more chain frame, ~0.07 ms); seeks are unchanged. Through
the whole pipeline (`bench_transfer --looks default,shader:sim --formats rgba32f`, 100 frames, two interleaved
runs per build, `--seek` = every other frame) the difference is within run-to-run noise:

| case | before (ms) | after (ms) |
| --- | --- | --- |
| UHD default look | 11.11 / 11.02 | 10.99 / 11.11 |
| UHD sim, played | 11.23 / 11.16 | 11.07 / 11.03 |
| UHD sim, sought | 11.37 / 10.88 | 11.19 / 11.18 |
| 1080p default look | 3.35 / 3.25 | 3.21 / 3.22 |
| 1080p sim, played | 3.25 / 3.23 | 3.20 / 3.24 |
| 1080p sim, sought | 3.36 / 3.21 | 3.29 / 3.35 |

The default look's last frame is byte-identical before and after (`bench_transfer --dump`); sim's played frame
differs (1080p: 4.8% of 8-bit values, at most 175/255, mostly in the moving head-switching band at the bottom,
where the old one showed the previous frames' noise).

## Tests

- `test_crt_order`: for each bundled preset (default look and CRT alone) and two sim looks with the feedback
  pushed (persistence 1 and 0.09), frames 0, 75 and 1850 rendered first in a fresh instance are compared byte
  for byte with the same frames after another frame, repeated (2nd, 3rd), backwards, forwards and interleaved
  between two instances. It writes every frame's hash; `test_crt_order_unshared` (second process,
  `NTSCRT_SHARED_CHAINS=0`) checks its own against them. Before the fix: 83 of 1168 checks failed, all sim.
- `tests/howlaround/test_howl_seek.cpp`: Screen Loop with sim renders the same with and without checkpoints.
- Resolve: the parity suite's sim cases are in the `cpu2` session too (`tags: noise`), so `cpu2-vs-cpu` checks
  them across sessions; `--parity-groups crt` runs the CRT cases only.
