// stages.h — CONTRACT (Wave 0): the interface of every pipeline stage.
// Each owner implements its class in core/stages/<name>/*.cpp (replacing the
// Wave-0 stub) and may add private headers there. Don't change these
// signatures without a note in docs/dev/CONTRACT-CHANGES.md.
//
// Per-frame order (port of upstream Mp4Exporter.exportStill, see docs/pipeline.md):
//   src(BGRA8) -> Crop -> [Howlaround camera] -> Digital -> Ntsc -> upload
//   -> Downscale -> Glitch -> Crt (librashader or bypass) -> output (= src size)
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../gpu/gpu.h"
#include "../src/internal.h"
#include "howlaround/howl_hooks.h"

namespace ntscrt {

struct NtscGpuPlan;  // stages/ntsc/gpu/ntsc_gpu.h

// ---- Crop (agent A) ---------------------------------------------------------
// Look key "crop" (NTSCRT extension; upstream keeps crop in app state):
//   {"enabled":bool,"aspect":"16:9"|"4:3"|"1:1"|...,"center":[x,y],"zoom":f}
// Output is the cropped picture at source pixel density (smaller than src).
// The CRT stage later fits it into the output frame (letterbox, black bars).
struct Crop {
    static void apply(const json &look, const ImageBGRA8 &in, ImageBGRA8 &out);
    // Same, but moves `in` into `out` when the crop cuts nothing (no copy).
    static void apply(const json &look, ImageBGRA8 &&in, ImageBGRA8 &out);
    // Does the look's crop cut a width x height picture (apply would change it)?
    static bool cuts(const json &look, int width, int height);
};

// ---- Digital video codec (agent C) — CPU, temporal ----------------------------
class DigitalStage {
public:
    DigitalStage();
    ~DigitalStage();
    bool active(const json &look) const;  // look["digital"]["enabled"]
    // In place. Uses ctx.source_at / ctx.look_at for previous frames.
    void process(ImageBGRA8 &inout, const FrameContext &ctx);
    void flush();
    // Where the last process() call's time went (appended 2026-11-12, digital GPU
    // work): " digital_ms=scale:..,camera:..,..." for the timings note.
    std::string profile_note() const;
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};

// ---- NTSC signal via ntsc-rs (agent A) — CPU, pure per frame -------------------
class NtscStage {
public:
    NtscStage();
    ~NtscStage();
    bool active(const json &look) const;  // look["ntsc"]["enabled"]
    void process(ImageBGRA8 &inout, const FrameContext &ctx);  // uses ntsc_frame(ctx)
    static std::string descriptors_json();  // ntsc-rs settings schema
    // GPU port (appended 2026-11-12, docs/dev/NTSC-GPU.md): the plan of this
    // frame for a width x height picture with the look's settings, as
    // process() would render it. `stage_mask` keeps only some stages (bit per
    // NTSC_ST_*; debugging). Throws like process() on bad settings.
    // `tables` false: without the passes' row tables and speckle lists, which
    // gpu_tables() then adds (another thread may, e.g. during the upload; it
    // only reads this stage's loaded settings). NtscGpu::run needs them.
    void gpu_plan(const FrameContext &ctx, int width, int height, NtscGpuPlan &out,
                  uint32_t stage_mask = 0xffffffffu, bool tables = true);
    void gpu_tables(NtscGpuPlan &plan) const;
    // Debug / parity: ntsc-rs's own Y, I, Q planes for `in` (before the
    // deinterlace) with only `stage_mask`'s stages; plane_rows rows each.
    std::vector<float> debug_planes(const ImageBGRA8 &in, const FrameContext &ctx, uint32_t stage_mask,
                                    int *plane_rows = nullptr);
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};

// ---- Downscale (agent D) — GPU ------------------------------------------------
class Downscaler {
public:
    explicit Downscaler(gpu::Device &dev);
    ~Downscaler();
    // Returns the chain input (retro raster). If look["downscale"] disabled,
    // returns `in` unchanged (shared ptr copy).
    std::shared_ptr<gpu::Texture> run(const std::shared_ptr<gpu::Texture> &in,
                                      const FrameContext &ctx);
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};

// ---- Glitch TV/VCR receiver (agent B) — CPU simulation + GPU compose ----------
class GlitchStage {
public:
    explicit GlitchStage(gpu::Device &dev);
    ~GlitchStage();
    bool active(const json &look) const;
    // Returns a new texture of the same size as chain_input (or chain_input if
    // inactive / healthy set — must be pixel-identical then).
    std::shared_ptr<gpu::Texture> run(const std::shared_ptr<gpu::Texture> &chain_input,
                                      const FrameContext &ctx);
    void flush();
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};

// ---- CRT shader via librashader, or bypass (agent D) — GPU ----------------------
class CrtStage {
public:
    CrtStage(gpu::Device &dev, const std::string &resource_dir);
    ~CrtStage();
    // Renders chain_input into `out` (host output size), letterboxing to the
    // chain input's aspect. Uses upstream ScanlineGrid supersampling rules.
    // look["shader"]["enabled"]==false -> ShaderBypass path.
    void render(const gpu::Texture &chain_input, gpu::Texture &out, const FrameContext &ctx);
    void flush();
    // When the shader can't run (preset missing or broken, librashader
    // unavailable, chain failed) render() draws the ShaderBypass picture
    // instead and these say why: an ntscrt_status code (0 = the shader ran or
    // was off) and a message. A failing preset isn't retried until flush().
    int fallback_status() const;
    const std::string &fallback_reason() const;
    size_t chain_load_attempts() const;  // tests: how often a chain was (re)built
    // A preset that keeps state between frames (feedback / history) ran since
    // construction or flush(): its pictures depend on the frames before them.
    bool ran_temporal_preset() const;
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};

// ---- Screen Loop / howlaround (agent G) — GPU, sequential ---------------------
// See core/stages/howlaround/howl_engine.h (random access) and
// docs/dev/CONTRACT-CHANGES.md (2026-11-10, agent G) for the façade wiring.
class Howlaround {
public:
    explicit Howlaround(gpu::Device &dev);
    ~Howlaround();
    bool active(const json *feedback) const;
    // The façade lends the loop its chain so past frames can be recomputed
    // (run-up, seeks). Without hooks the TV shows the camera picture directly.
    void set_hooks(HowlHooks hooks);
    // Replaces the scene with the camera's view of the simulated TV (runs the
    // run-up on first use / after seeks, pulling frames via ctx.source_at).
    std::shared_ptr<gpu::Texture> camera(const std::shared_ptr<gpu::Texture> &scene,
                                         const json &feedback, const FrameContext &ctx);
    // After Glitch (before the CRT): this frame's CHAIN INPUT, which the loop
    // draws through its own CRT copy (hooks.render_tv) as the TV's picture.
    void after_frame(const gpu::Texture &chain_input, const json &feedback, const FrameContext &ctx);
    void flush();
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};

}  // namespace ntscrt
