# Contract changes log
Append one entry per change to a Wave-0 contract file: date, agent, file, change, reason.

## 2026-11-10 — agent G — `core/stages/stages.h` (Howlaround), feedback_json keys

**Change.** `stages.h` now includes `howlaround/howl_hooks.h` (declares `HowlPass`, `HowlHooks`) and
`Howlaround` gained one method, `void set_hooks(HowlHooks hooks)`. `after_frame` keeps its signature but its
first argument is now **this frame's chain input** (after Downscale + Glitch, before the CRT), not the CRT
output. Owner of the Howlaround section: agent G (was "agent D").

**Reason.** Upstream renders Screen Loop strictly in order, and the TV picture that feeds the loop is the
chain input drawn by the loop's *own* copy of the CRT chain at a fixed size (`HowlaroundFeedback`), not the
exported frame. A plugin host renders out of order, so `Howlaround::camera()` must be able to recompute past
frames (the run-up, history after a seek), which needs the whole chain — only the façade can run it. Without
hooks the stage still works (the TV then shows the camera picture directly, no NTSC/CRT compounding), so the
Wave-0 façade keeps compiling and behaving.

**Random access** (`core/stages/howlaround/howl_engine.h`): frame N is defined as the loop run from the window
start A(N) = 0 for N < 2W, else (floor(N/W) - 1)·W (W = 240 frames by default), with upstream's run-up at A.
Same settings → same frames whatever the access order; the first 2W frames equal upstream's sequential export.
Sequential playback and re-rendering the last ~4 frames continue the history; other seeks re-run from A(N)
(up to 2W chain passes plus the run-up).

**feedback_json** (upstream `"kind": "ntscrt-video-feedback"`; `values` = upstream knobs, version-1 ids are
migrated). NTSCRT-Plugins extensions, all optional:
- `"length"`: seconds a move spans / a seamless loop repeats over. Hosts should set it to the effect's duration
  (upstream: the clip's duration). Fallback: upstream `"seconds"` (stills, clamped 0.5…60), else 5.
- `"window"`: W above, integer frames; `<= 0` = always from frame 0 (exact upstream, unbounded seek cost).
- `"enabled"`: false = off (missing = on; the bundled presets have no such key).

**Façade change needed (Wave 2, `core/src/ntscrt.cpp`).**
1. Own a second CRT stage for the loop (upstream keeps a separate `LRShaderChain`, since librashader chains carry
   per-chain frame history): `std::unique_ptr<CrtStage> howl_crt;` created next to `crt`.
2. With Screen Loop active, order the frame as upstream does — the camera comes **before** Digital/NTSC:
   `crop -> upload -> howl->camera() -> download -> Digital -> NTSC -> upload -> Downscale -> Glitch -> CRT`,
   then `howl->after_frame(*chain, feedback, ctx)` (the chain input, not `*out`).
3. Before `camera()`, every render (the lambdas capture this call's `ctx`/args):
   ```cpp
   // Stage counters come from ctx.frame through internal.h's inline helpers, so give
   // each stage a context whose frame yields the pass's counter; look/source stay at pass.frame.
   auto at = [&](int64_t counter_frame, const HowlPass &p) {
       FrameContext c = ctx; c.frame = counter_frame; c.look = &ctx.look_at(p.frame); return c; };
   HowlHooks h;
   h.chain_input = [&](const std::shared_ptr<gpu::Texture> &cam, const HowlPass &p) {
       ImageBGRA8 img; img.resize(cam->desc().width, cam->desc().height);
       dev.finish(); dev.download(*cam, img.px.data(), ptrdiff_t(img.width) * 4);
       FrameContext c = at(p.frame, p);                       // digital_frame = frame
       if (!p.run_up && inst->digital.active(*c.look)) inst->digital.process(img, c);  // upstream run-up: no Digital
       FrameContext cn = at(p.ntsc_frame - 1, p);             // ntsc_frame(cn) == p.ntsc_frame
       if (inst->ntsc.active(*c.look)) inst->ntsc.process(img, cn);
       auto t = dev.create_texture({img.width, img.height, gpu::Format::BGRA8, true, false});
       dev.upload(*t, img.px.data(), ptrdiff_t(img.width) * 4);
       auto chain = inst->downscale->run(t, c);
       FrameContext cg = c; cg.frame = int64_t(std::llround(p.glitch_time * ctx.fps));  // glitch_time(cg)
       if (inst->glitch->active(*c.look)) chain = inst->glitch->run(chain, cg);
       return chain;
   };
   h.render_tv = [&](const gpu::Texture &chain, gpu::Texture &tv, const HowlPass &p) {
       inst->howl_crt->render(chain, tv, at(p.shader_frame - 1, p));  // shader_frame(c) == p.shader_frame
   };
   inst->howl->set_hooks(std::move(h));   // cheap; re-set each render since the lambdas capture locals
   ```
   Re-setting hooks every render keeps the loop's history (only switching between hooked and unhooked
   restarts it). Hooks must stay valid until the end of the render call that uses them.
4. `tv` is RGBA8 with `render_target = true`, exactly `k ×` the chain size (same aspect): the loop's CRT must
   fill it without letterboxing and, like upstream's `HowlaroundFeedback`, without ScanlineGrid supersampling.
5. `ntscrt_flush_caches` already calls `howl->flush()`; hosts should flush on look edits (the loop's history
   holds TV pictures made with the old look).

- 2026-11-10, agent A, docs/params.md: `look["shader"]["preset"]` is the upstream preset **id**
  (`glow_gauss`, `royale`, … from Presets.swift), not a .slangp path — that's what every bundled preset
  stores. The schema's `crt.preset` option values changed accordingly (each option carries its `slangp`).
  Affects agent D (CrtStage) and the hosts.
- 2026-11-10, agent A, docs/params.md (+ stages.h comment now stale): `look["crop"]` uses upstream's keys
  `ratio`/`x`/`y`/`scale` (+ legacy `position`), plus `enabled`; a crop object without `enabled` is on
  (upstream only saves a crop that's on). Wave-0 `aspect`/`center`/`zoom` are still accepted.
- 2026-11-10, agent A, docs/params.md: "missing keys = upstream defaults" made concrete — notably ntsc and
  glitch default **on** when their key/`enabled` is absent (upstream AppState), ntsc settings overlay the
  house defaults. `NtscStage::active({})` is true.
- 2026-11-10, agent A, docs/params.md: schema additions — top-level `constants` and `digital_formats`;
  optional param fields (`unit`, `percent`, `scale`, `encode: "number"` for 0/1-stored bools, `structure`,
  `position`, `neutral`, `integral`, option `applies`/`legacy`/`hint`/`slangp`); hosts write only params
  whose `visible_if` holds; choice options append-only and ids unique after `[^A-Za-z0-9]`→`_`
  (coordinator rule; the generator enforces both).
- 2026-11-10, agent A, docs/params.md: feedback_json gains `"enabled"` (`fb:/enabled`, schema
  `screenloop.enabled`) and the schema exposes upstream's `fb:/seconds`. Affects agent D (Howlaround::active
  should treat `enabled: false` as off) and the hosts.
- 2026-11-10, agent A, rust/ntscrs-capi: added `ntsc_process_frame_ex` (ntsc-rs scale_factor + seed offset);
  the upstream C API is unchanged. ntsc uses ctx.render_scale as ntsc-rs's scale_factor only when
  `scale_with_video_size` is off (upstream has no equivalent: it always passes 1,1), and adds ctx.seed to
  `random_seed`.

- 2026-11-10, agent D, `core/gpu/gpu.h`: added `static const char *Device::last_create_error()`.
  Reason: `Device::create` must not throw when no GPU exists; it returns nullptr and this gives the reason
  (the façade can copy it into `ntscrt_last_error`). Also comment-only portability notes (RWTexture2D reads
  only for R32F, cbuffer layout, structured-buffer stride inference, BGRA8 storage fallback, Metal index map).
- 2026-11-10, agent D, `cmake/Shaders.cmake`: signature unchanged (`ntscrt_add_shaders(<target> <file.slang>...)`).
  Kernel names are `"<file stem>/<entry point>"`; every `[shader("compute")]` function is an entry point;
  stems must be unique per target.

- 2026-11-10, coordinator, `ntscrt.h` + docs/params.md: alpha handling. `ntscrt_render_args` gains
  `source_premultiplied` (appended; zero-initialised args = straight alpha, the old behaviour for opaque
  sources). Look gains `"alpha": {"mode": "clip"|"spill"}` (schema `alpha.mode`, `"preset": false`, a new
  optional schema field hosts honour when applying presets). OFX sets output premultiplication = source's;
  FxPlug passes premultiplied. `facade/alpha_merge` params gained `mode`, `premult` (32-byte cbuffer).

- 2026-11-11, agent C (upstream sync to v0.20.0), `core/src/internal.h`: `FrameContext` gains
  `int line_height = 1`. Reason: upstream 0.19's Digital video "One field" sends the chain the field itself
  (`DownscaleSpec.lineHeight` 2: half the lines, each two picture rows). The façade sets it for the frame
  (`digital::chain_line_height(look)` from `core/stages/digital/fields.h`, only when the Digital stage runs,
  so never under Screen Loop); the Downscale halves its lines; the CRT stage fits and supersamples by the
  picture's shape (`in.height * line_height`), as upstream ScanlineGrid / SupersampledPass do.
- 2026-11-11, agent D (responsiveness), `ntscrt.h`: appended `ntscrt_flush_caches_mask(inst, bits)` with
  `NTSCRT_CACHE_SOURCE|DIGITAL|GLITCH|CRT|SCREENLOOP|ALL` (ABI version unchanged: additive), so hosts flush only
  what an edit can affect. Documented that `NTSCRT_ERR_GPU`, `NTSCRT_ERR_MISSING_RESOURCE` and
  `NTSCRT_ERR_SOURCE_UNAVAILABLE` returns still wrote a complete degraded frame (CRT shader skipped, or CPU stages
  after a lost GPU) and that a render returning OK clears `ntscrt_last_error`. Hosts should show those as a
  warning, not fail the frame (OFX and ntscrt-cli do; the FxPlug host still treats them as failures).
- 2026-11-11, agent D, `core/gpu/gpu.h`: `virtual bool Device::lost() const` (default false; D3D11 reports
  device removal). `core/stages/stages.h`: `Crop::apply(look, ImageBGRA8 &&in, out)` (moves when nothing is cut);
  `CrtStage::fallback_status()`, `fallback_reason()`, `chain_load_attempts()`. `core/src/pixel_convert.h`: an
  empty alpha vector means fully opaque. Look history: `FrameContext::look_at` references stay valid for at
  least 256 further lookups in the same render (stages read them on the spot).
- 2026-11-11, agent G (shared device), `core/gpu/gpu.h`: `static std::shared_ptr<Device> Device::shared(Backend)`
  (process-wide own device per backend, weak-referenced so it dies with its last user; a lost one is replaced on the
  next call; `NTSCRT_SHARED_DEVICE=0` = a device per call) and `std::recursive_mutex &Device::context_mutex()`.
  Every Device method is now thread-safe (atomic on the context / command buffer); code that drives
  `native_context()` itself (librashader) holds `context_mutex()` for the sequence. `last_create_error()` is per
  thread. `download()` waits for earlier work itself (documented; callers no longer `finish()` first). The façade
  uses `shared()` whenever the host gave no device. `core/src/testing.h`: `Stats::gpu_device`.
  CRT chains of presets without history are shared per (device, preset) across CrtStages on D3D11 only (librashader's
  Metal runtime writes uniforms into one non-renamed MTLBuffer at encode time, so Metal chains stay per stage)
  (`NTSCRT_SHARED_CHAINS=0` turns that off); `core/stages/digital/parallel.h`: `shutdown_threads()`,
  `threads_running()`, called by `ntscrt_destroy` for the last instance.
- 2026-11-11, agent G (bit depth): the façade picks the CRT output texture format per host picture: RGBA16F for
  `NTSCRT_RGBA16` / `RGBA16F` / `RGBA32F` destinations (and RGBA16F/32F Metal textures) when the look's CRT shader is
  on, BGRA8 otherwise (8-bit hosts and the bypass are unchanged; `NTSCRT_OUTPUT_8BIT=1` forces BGRA8). The CRT
  stage's supersample target follows its output's format. `core/src/pixel_convert.h`: `from_rgba16f()`.
  `facade/alpha_merge` saturates RGB (a no-op for 8-bit input).
- 2026-11-11, agent H (host protocol), `ntscrt.h`: appended `ntscrt_source_frames_needed(args, &first)` (ABI
  version unchanged: additive). Reason: hosts that declare an effect's inputs up front (OpenFX getFramesNeeded,
  FxPlug scheduleInputs) need the source frames the temporal stages can fetch, which only the core knows (Digital
  video's stream back to its last real keyframe, datamosh-dropped keys included; Screen Loop's window start).
  Pure function of look/feedback/fps/frame/seed. Implementation in the new `core/src/frames_needed.cpp` (one line
  added to `core/CMakeLists.txt`) and `digital::first_source_frame` (new `core/stages/digital/frames_needed.h`,
  defined in `digital.cpp` next to the renderer it mirrors).
- 2026-11-11, agent J (Screen Loop seeks), `core/stages/howlaround/howl_hooks.h`: `HowlHooks` gains two optional
  members. `chain_input_begin(camera, pass)` is chain_input in two halves: it may read the camera back at once and
  returns the second half, which waits for the CPU work (NTSC, on a worker) and does the rest; Howlaround begins
  and finishes passes in frame order with at most two begun and unfinished, all finished before `camera()` returns,
  so the loop's history passes overlap NTSC with the next cameras (same pictures). `tv_keeps_state()` says the
  loop's CRT copy ran a temporal preset: no checkpoints then. `core/stages/stages.h`:
  `bool CrtStage::ran_temporal_preset() const` (a preset with feedback / history ran since construction or
  flush()). The façade keeps a second pair of `NtscStage`s for the loop's worker passes. `howl_engine.h`: the
  engine keeps checkpoints of its runs (every 30 frames and at each window start, LRU within
  `NTSCRT_SCREENLOOP_CHECKPOINT_MB`, default 256, 0 = none) and resumes a seek from the closest one at or before
  the frame instead of re-running the window; flushed with the loop's history (`NTSCRT_CACHE_SCREENLOOP`). A run
  that met a missing past source takes no checkpoints after it; the engine assumes the host can fetch every
  past frame it rendered (as the main input).
- 2026-11-11, agent I (shared host params), `docs/params.md`: the param model both hosts follow is now one library,
  `hostparams/` (`ntscrt::host`, target `ntscrt_hostparams`; `ntscrt_host/params.h`, `ntscrt_host/file_log.h`),
  replacing `ofx/ofx_params.*` (deleted) and the logic in `fxplug/model/param_model.*` (now FxPlug ids, layout,
  menu-index / RGB values and plug-in state over it). Reason: the hosts built the same preset differently. The
  rules the shared model settles (OpenFX's, except clamping to the schema range) are tabled in docs/params.md
  "One model for both plug-ins". Visible to hosts: feedback JSON is always an object with `"enabled"` (FxPlug
  passed NULL for Screen Loop off; the core treats both as off); `released_preset_order()` moved to
  `hostparams/src/params.cpp` (scripts that parse it follow); bundled presets can be listed without a core
  instance (`ntscrt::host::bundled_presets_json(resource_dir)`, same JSON as `ntscrt_bundled_presets_json`).
  No ABI change. FxPlug's hidden *Base Look* / *Base Screen Loop* params (ids 8, 9) are kept but no longer read.
- 2026-11-11, agent R (host GPU frames), `ntscrt.h`: appended `ntscrt_host_gpu_api`, `ntscrt_host_gpu`,
  `ntscrt_gpu_image`, the status `NTSCRT_ERR_HOST_GPU` (6, a macro so the enum is untouched),
  `ntscrt_host_gpu_supported`, `ntscrt_render_host_gpu`, `ntscrt_host_gpu_read` / `_write` and
  `ntscrt_last_timings` (ABI version unchanged: additive; existing functions unchanged). Reason: Resolve hands
  OpenFX plug-ins CUDA (Windows) / Metal (macOS) images when they ask; rendering them without a trip through CPU
  memory needs the core to copy GPU to GPU into its own device (docs/dev/HOST-GPU.md). `core/src/ntscrt.cpp`:
  `render_once` takes an optional direct GPU source (used when no CPU stage is on) and times its phases; the
  shared renderer and the instance's GPU state are reachable from other façade files through the new private
  `core/src/facade.h`; `ntscrt_instance` gains the bridge and the phase times. New files only otherwise
  (`core/src/host_gpu*.{h,cpp,mm}`, `phase_times.h`, `core/src/shaders/hostgpu_*.slang`, `tests/hostgpu/`).

## 2026-11-12 — agent Q (fast transfer) — `core/gpu/gpu.h`, `core/stages/stages.h`

**Change.** `gpu::Device` gains two virtual methods with default implementations over `upload` /
`download` (in `core/gpu/device.cpp`), so other backends need nothing:
- `write(Texture&, WriteFn)`: `fn(rows, pitch)` writes the whole picture straight into the memory the GPU
  copies from (D3D11: a pooled `D3D11_USAGE_STAGING` texture with CPU write, mapped with `DO_NOT_WAIT` when a
  previous copy from it may still run, then `CopyResource`; Metal: the shared staging buffer). Write-only memory
  (may be write-combined).
- `read(const Texture&, ReadFn, int bands = 1)`: `fn(rows, pitch, y0, y1)` for bands of rows in order, straight
  from the readback memory (D3D11: one staging texture per band, a fence value after each band's copy, so
  band k is converted while the GPU copies k+1; without fences one band after `finish()`. Metal: one band from
  the shared buffer). BGRA8 textures stored as RGBA8 (no typed BGRA UAV) use the defaults.

`Crop` gains `static bool cuts(const json &look, int width, int height)`.

**Reason.** The OpenFX path spent more time moving pixels than on the GPU stages: a copy of the result into a
buffer before converting it, an intermediate CPU picture before every upload (and none needed when no CPU stage
touches the frame), allocations per frame. `write` / `read` let the façade convert straight into upload memory
and straight out of readback memory; `cuts` tells it when the crop would change the picture (then the CPU
picture is needed). No ABI change; pixels are byte-identical (docs: CHANGELOG, `bench_transfer --dump`).

## 2026-11-12 — agent S (both combined) — `core/src/facade.h`, `core/src/phase_times.h` (private)

**Change.** One direct-source abstraction and one phase clock for both: `facade::DirectSrc` (source size,
whether it applies the crop itself, and `make(dev, mode, look, alpha, pool)` returning the picture texture or
nullptr to fall back to `get_src`) replaces agent Q's `DirectSrc::put` and agent R's `DirectFn`; the CPU path's
(`crops` false: used only when the crop cuts nothing; not with `NTSCRT_FAST_TRANSFER=0`) and the host GPU
path's (`crops` true) are two instances of it. `PhaseTimes` (`phase_times.h`) is exclusive (nested scopes count
in the innermost phase), gains `loop` (Screen Loop's camera) and `readback` (GPU -> CPU with its wait, split out
of `out`) and the call's total; `ntscrt_last_timings` prints it and `testing::last_phases` sums it into agent Q's
benchmark phases. `fast_transfer()`, `readback_bands()` and `for_row_blocks()` moved to `facade::` so
`host_gpu.cpp` uses the banded `read` for its 8-bit download (`gpu-download`) and its transparent-frame read
(`gpu-cpuconvert`). No ABI change.

**Reason.** Both branches added a direct source and a phase clock at the same place in `render_once`.

## 2026-11-12 — agent U — `core/stages/stages.h` (NtscStage), `rust/ntscrs-capi` C ABI

**Change.** `NtscStage` gains two methods (nothing existing changes): `gpu_plan(ctx, width, height, plan,
stage_mask)` (the GPU port's plan of a frame, from the same settings handling as `process`) and
`debug_planes(in, ctx, stage_mask, &plane_rows)` (ntsc-rs's own YIQ planes with a subset of its stages).
`stages.h` forward-declares `NtscGpuPlan` (`stages/ntsc/gpu/ntsc_gpu.h`). The shim's header adds the
`ntsc_gpu_*` exports, `ntsc_debug_process_yiq` and their structs; `ntsc_process_frame(_ex)` are unchanged
(`process_impl` now takes its `NtscEffect` from `effect_for`, which the plan exports share). `testing::PhaseTimes`
gains `ntsc` (informational, part of `cpu_stages`). No ABI change of the core.

**Reason.** Phase 1 of the GPU port of ntsc-rs (docs/dev/NTSC-GPU.md): the pipeline asks the stage for the
frame's plan and runs it on the GPU when every pass is implemented; the parity harness compares the two paths
pass by pass.

## 2026-11-12 — agent W — `core/stages/stages.h` (NtscStage), `rust/ntscrs-capi` C ABI

**Change.** `NtscStage::gpu_plan` gains a defaulted last parameter `bool tables = true` and `NtscStage` a
method `void gpu_tables(NtscGpuPlan &) const` (the plan's row seeds, row shifts and speckle lists; with
`tables = false` the caller adds them, e.g. on another thread). `NtscGpuPlan` (`ntsc_gpu.h`) gains the tables
and their frame arguments. The shim implements `ntsc_gpu_row_shifts` / `ntsc_gpu_speckles` (stubs before;
layouts unchanged, `NtscGpuRowShift::transient_intensity` renamed `intensity` with a per-stage meaning), adds
`ntsc_gpu_simd_lanes`, pass kinds 9-12, `NTSC_GPU_SEEDS_TRACKING` and `NTSC_GPU_SHIFT_NONE`. No ABI change of
the core.

**Reason.** Phases 2-3 of the GPU port of ntsc-rs (docs/dev/NTSC-GPU.md): every stage on the GPU; the tables'
CPU time overlaps the picture's upload.
