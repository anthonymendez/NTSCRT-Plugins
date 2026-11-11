// howl_hooks.h — how the Screen Loop asks the façade for the TV's picture of a
// frame it has to recompute (the run-up, or the history before a seek).
// Included by stages.h; see docs/dev/CONTRACT-CHANGES.md (2026-11-10, agent G).
//
// Upstream renders the loop strictly in order; in a plugin the host asks for
// frames in any order, so Howlaround::camera() may need the TV picture of past
// frames that were never rendered. Only the façade can run the full chain
// (Digital, NTSC, Downscale, Glitch, the loop's own CRT copy), so it lends
// Howlaround these two functions.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include "../../gpu/gpu.h"

namespace ntscrt {

// One trip of the picture round the loop, and the counters the chain must use
// for it (upstream HowlaroundLoop.runUp / Mp4Exporter conventions).
struct HowlPass {
    int64_t frame = 0;          // the effect frame it belongs to: look_at(frame), source_at(frame)
    bool run_up = false;        // a run-up trip before the window's first frame:
                                //   upstream runs it with NO Digital stage, glitch standing still
    int64_t ntsc_frame = 1;     // NTSC frame counter (run-up: 100000 + k, else frame + 1)
    int64_t shader_frame = 1;   // CRT frameCount (same values as ntsc_frame)
    double glitch_time = 0;     // receiver time in seconds (run-up: window start / fps, else frame / fps)
};

struct HowlHooks {
    // The camera's picture (scene-sized, the scene texture's format) -> chain
    // input, exactly as the main path does it for pass.frame: download, Digital
    // (skipped when run_up), NTSC (ntsc_frame), upload, Downscale, Glitch
    // (glitch_time). Must not touch Howlaround.
    std::function<std::shared_ptr<gpu::Texture>(const std::shared_ptr<gpu::Texture> &camera,
                                                const HowlPass &pass)> chain_input;
    // Chain input -> the TV's picture, drawn by the loop's OWN copy of the CRT
    // chain (upstream HowlaroundFeedback: a separate LRShaderChain, or the
    // bypass when the shader is off; no supersampling) into `tv`, which
    // Howlaround allocates at a fixed whole multiple of the chain size
    // (same aspect, so no letterbox). shader_frame is the CRT frame count.
    std::function<void(const gpu::Texture &chain, gpu::Texture &tv, const HowlPass &pass)> render_tv;
};

}  // namespace ntscrt
