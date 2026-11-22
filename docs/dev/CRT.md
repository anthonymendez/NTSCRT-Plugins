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
(`test_crt_order` checks all 7, with the plug-in's default look and alone). Since 2026-11-20 sim doesn't load
crtsim's own `.slangp` but our `crt/crtsim-single.slangp`, which reads nothing from earlier frames either
(see "One chain frame" below).

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
with the frame's uniforms. (Both only with `NTSCRT_SIM_SINGLE=0` since the single chain frame below: one frame
has no second encode to wait for.)

A temporal preset with no entry in `feedback_resets()` (none bundled: only a `.slangp` added to the resources)
keeps the old prime-on-seek path, which is not order-independent; `CrtStage::ran_temporal_preset()` then tells
Screen Loop to keep no checkpoints. With crtsim, Screen Loop now keeps its checkpoints (its TV pictures are a
function of the pass).

## One chain frame (2026-11-20)

The rule above rendered every sim frame twice on the chain, and the two pictures were the same frame. Now
one chain frame gives the same bytes. Clearing the feedback ourselves (or patching librashader so
`clear_history` clears `feedback_framebuffers` too) would not: a zeroed `PassFeedback0` gives
`max(Cur, 0) = Cur`, the *reset* frame's picture, while the rule's frame reads the reset frame's composite
output as its previous frame (its own afterglow and bleed). What the second frame needs is that picture, made
in the same chain frame. So sim (`kPresets` still says `crt/crtsim.slangp`; `chain_file()` in `crt.cpp` swaps
the file) loads **`crt/crtsim-single.slangp`** (`core/stages/crt/slang/`, copied into
`<resources>/shaders/` next to the vendored files by `copy_resources.cmake`), crtsim with one pass more:

| pass | shader | what it does |
| --- | --- | --- |
| 0 | `crtsim-single/prime.slang` | composite as the reset frame ran it: persistence 0 (a constant), frame count n - 1 (0 stays 0) for the NTSC artifact phase; its "previous frame" is `Source`, multiplied by 0 |
| 1 | `crtsim-single/composite.slang` | composite as the frame ran it: current frame `Original`, previous frame `PassOutput0` (pass 0) |
| 2-5 | crtsim's `screen`, `post-downsample`, `post-upsample`, `present` | unchanged (`CRTPASS` alias, `Original`) |

Both new files are crtsim's `composite.slang` (CC0) changed only on the lines marked `NTSCRT`. Why it is the
same bytes: pass 0 renders into an RGBA8 framebuffer of the input's size with nearest filtering, exactly
what the reset frame wrote into composite's feedback framebuffer (same size, format, filter and wrap: librashader
binds `PassOutput0` and `PassFeedback0` with the producing pass's filter / wrap, and `Original` with pass 0's,
which is nearest as composite's `Source` was); pass 1 runs composite's code on the same input, parameters and
frame count. The alpha channel differs (pass 0 reads `Source.a` where the reset frame read the feedback's
alpha), but as above no output byte reads composite's alpha. Nothing reads an earlier frame, so
`uses_history()` is false: sim's chain is now shared between instances on D3D11 like the other presets, needs no
`Device::finish()` on Metal, and `ran_temporal_preset()` stays false as before.

`NTSCRT_SIM_SINGLE=0` (read when a chain is made) loads crtsim's own `.slangp` with the two-frame rule again,
for comparison; `test_crt_order` "sim: one chain frame renders the two-frame reset's bytes" compares the two
(4 sim looks x 5 frames, 0, 1, 2, 75, 1850: both artifact phases, x 200x150 and 1280x720, the second supersampled;
the single-frame instance renders them backwards). Changing pass 0's frame count to n makes it fail (7,000-8,000
of 120,000 values per frame), so it does see the difference. In Resolve (headless, `--parity --parity-groups crt
--parity-configs cpu,gpu`, run once as is and once with `NTSCRT_SIM_SINGLE=0`) all 134 frames of the two runs,
the 18 sim ones included, decode to the same pixels; `--only order` (now with an `order:sim` case: 16 frames
in order, then 6 of them out of order in a fresh tool) gives the same pixels for every sim frame.

What it saves (RTX 5090, Release, the GPU shared with other work at ~90% load, so medians swung 2-4x;
the minimum of 8 interleaved runs' medians is the steady figure). The stage alone (`test_crt "CRT frame timings"`,
`NTSCRT_CRT_TIMINGS=1`, one frame finished on the GPU), `NTSCRT_SIM_SINGLE=0` -> default:

| chain input -> output | played | sought (every other frame) |
| --- | --- | --- |
| 640x480 -> 3840x2160 | 0.326 -> 0.248 ms | 0.317 -> 0.240 ms |
| 640x480 -> 1920x1080 | 0.253 -> 0.171 ms | 0.238 -> 0.173 ms |
| 1920x1080 -> 3840x2160 | 0.334 -> 0.265 ms | 0.338 -> 0.227 ms |
| 1920x1080 -> 1920x1080 | 0.273 -> 0.200 ms | 0.266 -> 0.191 ms |

Through the whole pipeline (`bench_transfer --looks shader:sim --formats rgba32f`, `NTSCRT_PROFILE=1`, its
`crt_median_ms`: the timings note's `crt=` with the GPU waited for; best of 3 interleaved runs, played): UHD
0.32 -> 0.24 ms, 1080p 0.21 -> 0.17 ms. Every case's last frame (UHD and 1080p, played and sought, sim and the
default look) is byte-identical between the two (`--dump`).

## What the rule cost (2026-11-12)

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

## Shader cache warm-up (2026-11-22)

The shader cache (`shader_cache.h`) is keyed by the plug-in version, librashader, `d3dcompiler_47.dll` and the GPU
and driver (macOS: the OS release), so it can't ship pre-built. Two things fill it ahead of a render
(`shader_warm.h`): both build every bundled preset's chain (`crt_bundled_chain_files()`: the chain files a render
loads, `crtsim-single` for Sim) with `create_chain(..., compile_only)` on the render device, through the same
`configure_shader_cache`, and throw it away; a complete run writes `warm.txt` into the key folder.

- **Installers:** `ntscrt-warm` (`ofx/warm/`, in the bundle next to the plug-in) loads the plug-in binary and calls
  its `ntscrt_ofx_warm_shader_cache` (`ntscrt_warm_shader_cache`: the core's own shared device, like an instance
  without a native device). `install.ps1`, the MSI (an impersonated deferred action) and `install.sh` / the `.pkg`
  (`launchctl asuser` + `sudo -u` the console user) run it as the user, never elevated / root; the tool refuses
  SYSTEM and root itself, and on Windows takes LOCALAPPDATA from the user's known folder. 5-minute cap (the tool
  stops itself); any failure is one line. Measured: 0.8-1.4 s for all seven, cold, on an RTX 5090.
- **Background:** `ntscrt_warm_shader_cache_background`, called by the OpenFX plug-in when it creates an instance
  (Resolve's plug-in scan only loads and describes). Once per process; skipped when `warm.txt` lists the current
  chain files and the folder still holds that many objects. A thread waits `NTSCRT_SHADER_WARMUP_DELAY_MS` (3 s),
  then before each preset until no render call is running and none ended in the last `NTSCRT_SHADER_WARMUP_IDLE_MS`
  (1 s): `warmup::RenderScope` in `ntscrt_render_cpu` / `_metal` / `_host_gpu`. Each chain is built through
  `ntscrt_libra_run_isolated` (`rust/librashader-patch/pool.rs`): on a private one-thread rayon pool at background
  priority, so its `into_par_iter` work never queues in the global pool render chains compile on; compile-only
  chains take no device context lock (D3D11: the deferred context's command list is dropped; nobody frames them).
  The last `ntscrt_destroy` stops it and waits for the preset being built (Royale cold on that one thread: ~1.1 s).
  Not done: pre-building the in-memory chains (~70 MB CPU + 20 MB GPU each, and building them for the shared
  registry would hold its lock and the context lock while renders want them); FxPlug doesn't call it yet; the
  installer warms the default adapter only (a laptop whose Resolve runs on the other GPU is covered by the
  background warm-up).

Headless Resolve, `--only firstframe`, 1080p, RTX 5090 (render ms / chain build ms):

| | aperture | easymode | glow_gauss | glow_lanczos | hyllian | royale | sim |
|---|---|---|---|---|---|---|---|
| empty cache | 250 / 103 | 33 / 27 | 61 / 46 | 24 / 15 | 98 / 79 | 594 / 567 | 59 / 40 |
| after `ntscrt-warm` (found all, compiled 0) | 166 / 22 | 11 / 4 | 21 / 9 | 18 / 9 | 33 / 13 | 186 / 158 | 27 / 10 |

A new effect's first frame (CRT off): 26 ms empty cache, 25 ms warmed, 21 ms while the background warm-up
(no delay, no idle wait) was compiling beside it; 28 ms with its defaults (it waited, then was stopped when the
session quit, 0 presets built).

## Tests

- `test_crt_order`: for each bundled preset (default look and CRT alone) and two sim looks with the feedback
  pushed (persistence 1 and 0.09), frames 0, 75 and 1850 rendered first in a fresh instance are compared byte
  for byte with the same frames after another frame, repeated (2nd, 3rd), backwards, forwards and interleaved
  between two instances. It writes every frame's hash; `test_crt_order_unshared` (second process,
  `NTSCRT_SHARED_CHAINS=0`) checks its own against them. Before the fix: 83 of 1168 checks failed, all sim.
- `test_crt_order` "sim: one chain frame renders the two-frame reset's bytes": `crt/crtsim-single.slangp`
  against crtsim's own preset with the two-frame rule (`NTSCRT_SIM_SINGLE=0`), byte for byte.
- `tests/howlaround/test_howl_seek.cpp`: Screen Loop with sim renders the same with and without checkpoints.
- Resolve: `run_resolve_tests.py --only order` has an `order:sim` case (sim frames out of order in a fresh tool).
- Resolve: the parity suite's sim cases are in the `cpu2` session too (`tags: noise`), so `cpu2-vs-cpu` checks
  them across sessions; `--parity-groups crt` runs the CRT cases only.
