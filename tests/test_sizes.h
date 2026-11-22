// How big the slowest GPU cases run (docs/TESTING.md "Full and CI sizes").
//
// On a hardware GPU they run full size (the default 240-frame Screen Loop window, the playheads
// of the bug reports, ...). On a software rasterizer (WARP: GitHub's Windows runners have no GPU,
// so every pass runs on the runner's 4 vCPUs) they run scaled down: a shorter window, nearer
// playheads, fewer frames. The checks are the same at either size (the property under test
// doesn't depend on it: the expected counts are computed from the size used), only the numbers
// change. NTSCRT_TEST_FULL_SIZE=1 (or 0) forces full (or CI) sizes anywhere.
#pragma once

#include <doctest/doctest.h>

#include <cstdlib>
#include <string>

#include "gpu/gpu.h"
#include "gpu/gpu_route.h"

namespace testsizes {

inline bool full() {
    static const bool v = [] {
        if (const char *e = std::getenv("NTSCRT_TEST_FULL_SIZE"); e && *e) return std::string(e) != "0";
#ifdef _WIN32
        const auto backend = ntscrt::gpu::Backend::D3D11;
#else
        const auto backend = ntscrt::gpu::Backend::Metal;
#endif
        auto d = ntscrt::gpu::Device::create(backend, nullptr, nullptr);  // NTSCRT_D3D11_WARP=1: WARP
        return d && !ntscrt::gpu_is_software(*d);
    }();
    return v;
}

// `big` on a hardware GPU (or NTSCRT_TEST_FULL_SIZE=1), else `small`.
template <class T> T pick(T big, T small) { return full() ? big : small; }

}  // namespace testsizes
