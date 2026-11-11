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
