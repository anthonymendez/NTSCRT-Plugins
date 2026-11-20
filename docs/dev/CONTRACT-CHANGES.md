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

## 2026-11-12 — agent X — `core/stages/ntsc/gpu/ntsc_gpu.h`, `core/src/testing.h` (private), timings text

**Change.** `ntsc_gpu_route(dev, why, g = nullptr)` gains a defaulted `NtscGpu *` (its fused-`mad` probe takes
part in the decision) and is **on by default** (unset / `auto` / `1`; `0` off; `force` also on software devices
and on GPUs without fused `mad` or doubles); `NtscGpuEnv` / `ntsc_gpu_env()` / `ntsc_gpu_env_text()` and
`cpu_ntsc_fuses()` are new; `ntsc_gpu_requested()` now means "not `0`". `NtscGpuPlan` gains a one-entry cache
key (`Key`, `key_valid`, `cached`): `NtscStage::gpu_plan` returns the plan (and `gpu_tables` the tables) it
already holds for the same settings, frame, seed, size, scale and stage mask. `testing::simulate_ntsc_gpu_errors`
(fault injection). `ntscrt_last_timings` text: ` ntsc_route=gpu (<why>)` / ` ntsc_route=cpu (<why>)` on every
frame with NTSC on (before: only with the variable set, without a reason for `gpu`), ` ntsc_cached=1`,
` ntsc_loop=gpu:N,cpu:M` (Screen Loop history passes), ` ntsc_gpu_ms=` (with `NTSCRT_PROFILE=1`). The OpenFX
plug-in logs the `ntsc_route` text once per instance when it changes. No C ABI change.

**Reason.** Phase 4 of the GPU port (docs/dev/NTSC-GPU.md): GPU NTSC by default where it is exact, inside
Screen Loop and on host GPU frames (`gpu-direct` with NTSC on).

## 2026-11-12 — agent Z — `core/stages/stages.h` (DigitalStage), `core/gpu/gpu_route.h`, `ntsc_gpu.h`, `core/src/testing.h` (private), timings text

**Change.** `DigitalStage` gains `std::shared_ptr<gpu::Texture> process_gpu(DigitalGpu &, const
std::shared_ptr<gpu::Texture> &, const FrameContext &)` (the stage on a picture on the GPU:
`core/stages/digital/gpu/digital_gpu.h`) and `std::string profile_note() const`. `digital::Renderer` gains
`code()` (`process()` without the full-size pictures), `scalePlan()` / `ScalePlan`, `ninths()`,
`upscaleMaps()`, `Profile` / `lastProfile()` / `profile()`, and `render()` a defaulted `double *deblock_ms`.
New `core/gpu/gpu_route.h`: `GpuRouteEnv`, `gpu_route_env[_text|_label]()`, `gpu_adapter_description()`,
`gpu_is_software()` (moved from `ntsc_gpu.h`, same signature, still visible through it); `NtscGpuEnv` is now an
alias of `GpuRouteEnv` (same enumerators). `testing::simulate_digital_gpu_errors` (fault injection).
`ntscrt_last_timings` text: ` digital_route=gpu (<why>)` / ` digital_route=cpu (<why>)` on every frame Digital
video runs, then ` digital_ms=scale:..,download:..,earlier:..,camera:..,codec:..,deblock:..,upsample:..,upload:..,upscale:..
digital_captures=N`. A Digital look takes the direct picture path (`path=gpu-direct` with host GPU frames) when
its route is on. The OpenFX plug-in logs `Digital video route: ...` once per instance when it changes. No C ABI
change.

**Reason.** Digital video's full-size steps on the GPU (docs/dev/DIGITAL-GPU.md), with the route decided like
NTSC's (one shared rule, one switch per stage: `NTSCRT_DIGITAL_GPU`).

## 2026-11-12 — agent A2 — `core/stages/stages.h` (CrtStage), `core/stages/howlaround/howl_hooks.h`: comments only

**Change.** No signature or ABI change. `CrtStage::ran_temporal_preset()` now means "a preset whose pictures
depend on earlier frames ran": crtsim no longer counts, since the CRT stage resets its feedback every frame
(docs/dev/CRT.md), so a Screen Loop with crtsim keeps its checkpoints (`HowlHooks::tv_keeps_state` is false for
every bundled preset). A crtsim frame is two librashader frames (the reset frame, then the frame) on the same
chain, on Metal with a `Device::finish()` between them.

**Reason.** A crtsim frame depended on the frames rendered before it (librashader's `clear_history` never clears
`PassFeedback`), so hosts rendering in any order got different pictures.

## 2026-11-19 — agent D2 — OpenFX param set: rules for projects saved with older plug-in versions

**Change.** No param, id, type or option changed. Written down: the rules that keep projects made with an
earlier plug-in version (0.3.0, 0.4.0, the 0.5.0 release candidates) opening and editing normally, and the
OpenFX plug-in's `update_dependents` now writes a control's enabled / secret state only when it changes.

**Old projects.** Resolve keeps an effect in a saved project by plug-in id and identity major
(`kOfxIdentityMajor` = 1) and restores every persistent param by name, as saved (params added later take their
default). So:
- Never remove, rename or retype a schema param or a `host_*` param. A control that goes away stays as a
  hidden legacy param (`legacy: true`, `visible_if` on its own value, as `digital.quality` / `exposure` /
  `black_level` / `lens_blur` from 0.4.0) and the core migrates it (`core/stages/digital/migrate.h`); a control
  with a new meaning gets a new id (`digital.quality2`).
- Choice params store an index: options are only ever appended, never reordered or removed (the Preset menus:
  released presets first, `released_preset_order`; Horizontal resolution's formats after Custom).
- A changed default only reaches new instances; a changed range must still accept every value saved before.
  Turning `animatable` on (29 params in 0.5.0-rc.1) loads the saved static value; don't turn it off for a param
  that may hold keyframes.
- Nothing that runs without the user asking (instance creation, beginInstanceEdit, instanceChanged with reason
  plugin / time, getClipPreferences, render) writes a param value. Presets, file loads and choice applies run only
  on a user edit. Display-only state (labels, the status line, enabled / secret) is written only when it differs
  from what was last written: each write is a property set Resolve acts on in the open Inspector, and a host can
  send a notification for every param when it opens an effect.
- `tests/ofx` "old projects: opening the panel keeps the saved values and stays bounded" restores a 0.4.0
  instance's values and keys, opens the panel and notifies every param twice: no value may change and at most
  one enabled / secret write per conditional property (310,728 before this change, 51 after).

**Reason.** User report after 0.5.0: projects that used earlier versions locked up Resolve when the clip was
selected and the Effects tab opened. Every instanceChanged re-set all ~480 enabled / secret properties
(306 `enabled_if`, 178 `visible_if`). Not reproduced in Resolve's own Inspector (the headless runs were blocked
by an open Resolve session); `NTSCRT_OFX_LOG_VERBOSE=1` now logs beginInstanceEdit / endInstanceEdit next to every
instanceChanged, so the user's log shows what Resolve does when the tab opens.

## 2026-11-20 — agent I2 — CRT resources: `shaders/crt/crtsim-single.slangp` (ours), `NTSCRT_SIM_SINGLE`

**Change.** No signature or ABI change. The resources gain `shaders/crt/crtsim-single.slangp` and
`shaders/crt/shaders/crtsim-single/{prime,composite}.slang` (from `core/stages/crt/slang/`, copied by
`copy_resources.cmake` next to the vendored files). The CRT stage loads it for the `sim` preset (and the path
`crt/crtsim.slangp`): one librashader frame per frame instead of two, same bytes (docs/dev/CRT.md, "One chain
frame"). sim's chain no longer reads earlier frames, so on D3D11 it is shared between instances like the other
presets. `NTSCRT_SIM_SINGLE=0` (read when a chain is made) restores crtsim's own preset and the two-frame rule.

**Reason.** The two chain frames gave one picture twice: half of sim's CRT time.

## 2026-11-20 — agent G2 — animated presets without keyframes: `host_animation` (OpenFX), `kAnimation` = 14 (FxPlug), `hostparams/ntscrt_host/params.h`

**Change.** One param added to each plug-in, nothing removed, renamed, retyped or reordered (D2's rules hold):
- OpenFX: `host_animation`, a string param in the Presets group, secret, not animatable, persistent, default `""`.
- FxPlug: `kAnimation = 14` (host extras range), a hidden, not animatable string param, default `""`.
- `hostparams` (`ntscrt::host`): `animation_text`, `animation_without`, class `Animation`, `plan_preset_procedural`,
  and `PresetPlan::animation`. `preset_keys` / `plan_preset` are unchanged (same keys: the per-pass curves moved
  into a helper both use).

**What a preset writes now.** Choosing an animated preset (bundled or a loaded file) writes no keyframes: every
look param presets set loses its keyframes (as before) and gets the timeline's first-frame value as a plain
value, and the animation param gets `animation_text` (JSON: `kind: "ntscrt_animation"`, the upgraded preset with
the timeline values every key gives alike left out of its keys, the ids of the params that can move, `off`: the
ids taken out of it, and `keyed`: the stretch keyframes would have covered: `{fps, first, frames}`, frames from
the effect's first frame). 8 to 21 KB for the bundled presets, against ~3,000 keyframes before (57,840 in
0.5.0-rc.1/rc.2). A still look writes `""` (whatever moved stops); a Screen Loop file leaves it alone.
`NTSCRT_PRESET_KEYFRAMES=1` (both plug-ins, read at each preset) still writes keyframes as 0.5.3 did, for A/B tests.

**Rendering.** Each frame the plug-in computes the moving params' values from the text (`Animation::value`, at
the frame rate the effect renders with, n = host time minus the effect's origin, as for the keyframes):
exactly the values the keyframes gave, i.e. the same sparse keys within each pass, interpolated linearly the way
Resolve / FxPlug / the mock host interpolate them, over the same stretch, with the first / last values held
before / after it as the keyframes did. So the pictures are the keyframed version's frame for frame (see
"Pictures" below). Looks of other frames (the Glitch receiver's and Digital video's histories) use the same values.
A param counts as keyed for cache flushing (`cache_mask`, `glitch_keyed`) while the animation moves it.

**User keyframes and edits on top (the rule).**
- A control **with keyframes** is never animated: its keyframes win. That covers projects from older versions
  (their preset keyframes, and no animation text) and keyframes the user adds afterwards.
- A **hand edit** of a control the animation moves takes that control out of the animation (`off`): the user's
  value holds on every frame; the other controls keep moving. The same for values a choice's `applies` or a
  Digital video format writes. (Least surprising: the value you set is the value you see, everywhere, as with a
  plain still control; nothing else stops moving. The alternative, the edit overriding only at the playhead,
  can't be told apart from "my edit did nothing" on any other frame.)
- Choosing **(none)** (OpenFX) / **(Custom)** (FxPlug) in the Preset menu stops the whole animation, keeping the
  controls' values. Choosing another preset replaces it. Undo restores the param like any other.
- The Inspector shows the first frame's values (no keyframe diamonds); enabled / hidden states follow those
  still values, not the animation (no Inspector writes while playing).
- Save preset writes the look at the playhead with the animation's values in it, as it did with the keyframes.

**Old projects.** A project saved before this change has no `host_animation`: it loads as `""` (D2: a param
added later takes its default), so nothing is animated procedurally and every keyframe renders as before.
Nothing migrates or deletes keyframes; choosing a preset clears the params it sets, as every version did.

**Pictures.** `test_ofx` "presets: the procedural animation renders exactly like the keyframes it replaces":
252 frames, 6 presets, 60 / 24 / 29.97 fps, a clip from frame 0 and an Adjustment Clip an hour in, frames across
the keyed stretch, before and after it: every pixel value identical to the keyframed render. `tests/hostparams`
compares the values with `preset_keys` + linear interpolation for all 16 bundled animated presets at 7 frame rates
and 6 positions / clip lengths (equal, including fractional times). Exactness relies on the host interpolating
keyframes linearly as `a + (b - a) * u` in doubles; Resolve was compared in headless renders (see the report of
this change). The stretch is kept only so the pictures stay the same: dropping `keyed` (plan without a frame
rate) makes the animation loop for the whole effect.

**Reason.** Keyframes made choosing a preset slow, made the Inspector heavy (0.5.1's old-project lock-up), arrived
in Resolve in pieces during the first render, and didn't follow clip-length / frame-rate changes.

## 2026-11-20 — agent J2 — timings text (Digital video), `digital::Renderer` (not a contract file)

**Change.** `ntscrt_last_timings`: after ` digital_captures=N` now ` digital_from=C digital_checkpoints=n:MB+t:MB`
(the capture the run went on from, -1 from a keyframe; checkpoints and the re-run trail kept). `digital::Renderer`:
`CheckpointConfig`, `code()` takes a defaulted `Size source` (the frames' size: part of the stream key),
`lastResumedFrom()`, `checkpointCount()` / `checkpointBytes()`, `trailCount()` / `trailBytes()`. `stages.h` and the
C ABI unchanged. The OpenFX plug-in no longer flushes `NTSCRT_CACHE_DIGITAL` when the host aborts a render (the
core drops the stream that met a missing frame itself).

**Reason.** Checkpoints of the codec state (docs/dev/DIGITAL-CHECKPOINTS.md).
