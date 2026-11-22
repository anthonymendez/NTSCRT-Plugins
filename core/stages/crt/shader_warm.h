// shader_warm.h — fills librashader's shader cache (shader_cache.h) ahead of the first
// render: builds every bundled CRT preset's chain on the render device, through the
// same cache folder, key and chain files a render uses, and throws the chains away.
// Used by the installer's warm-up (ntscrt_warm_shader_cache) and the plug-in's
// background warm-up (core/src/warmup.cpp). Private to the core.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "../../gpu/gpu.h"

namespace ntscrt::crt {

struct WarmOptions {
    std::function<void(const std::string &)> progress;  // one line per preset, then a summary
    // Called before each preset: false stops the warm-up there (the background warm-up
    // waits in it while renders run).
    std::function<bool()> before_preset;
    // Build on one low-priority thread of librashader's own (ntscrt_libra_run_isolated),
    // never in the thread pool renders compile on. Off: the whole pool (the installer's).
    bool isolated = false;
    // Nothing to do when the key folder's warm.txt says every preset is in it.
    bool skip_if_complete = false;
};

struct WarmResult {
    int presets = 0, built = 0, failed = 0;
    uint64_t found = 0, compiled = 0;  // shader objects read from / added to the cache
    double ms = 0;
    bool cache_on = false;
    bool already_complete = false;  // skip_if_complete, and it was
    bool stopped = false;           // before_preset said stop
    std::string dir;                // the key folder
    std::string why;                // when nothing could be done: why
};

WarmResult warm_shader_cache(gpu::Device &dev, const std::string &resource_dir, const WarmOptions &opt);

}  // namespace ntscrt::crt
