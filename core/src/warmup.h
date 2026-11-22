// warmup.h — the shader cache warm-up behind ntscrt_warm_shader_cache[_background]
// (core/stages/crt/shader_warm.h does the building). Private to the core.
#pragma once

#include <ntscrt/ntscrt.h>

#include <memory>
#include <string>

#include "../gpu/gpu.h"

namespace ntscrt::warmup {

// Held for the length of every render call: the background warm-up only builds a
// preset when none is running and none ended in the last second. Two atomics.
struct RenderScope {
    RenderScope();
    ~RenderScope();
    RenderScope(const RenderScope &) = delete;
    RenderScope &operator=(const RenderScope &) = delete;
};

// ntscrt_warm_shader_cache_background on `dev`.
void start_background(std::shared_ptr<gpu::Device> dev, const std::string &resource_dir,
                      ntscrt_warm_progress_fn progress, void *user);
// Stops it and waits for the preset being built (the last ntscrt_destroy).
void stop_background();

}  // namespace ntscrt::warmup
