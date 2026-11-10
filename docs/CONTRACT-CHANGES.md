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
