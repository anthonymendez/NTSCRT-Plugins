// Shared helpers for GPU tests (tests/gpu, tests/downscale, tests/crt).
#pragma once

#include <doctest/doctest.h>

#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "gpu/gpu.h"

namespace gputest {

using namespace ntscrt;

inline gpu::Backend native_backend() {
#ifdef _WIN32
    return gpu::Backend::D3D11;
#else
    return gpu::Backend::Metal;
#endif
}

// True when NTSCRT_REQUIRE_GPU is set to anything but "" / "0" (CI sets it).
inline bool gpu_required() {
    const char *v = std::getenv("NTSCRT_REQUIRE_GPU");
    return v && *v && std::string(v) != "0";
}

// Own device (hardware, WARP fallback). Tests skip themselves when null, unless
// NTSCRT_REQUIRE_GPU=1: then a missing device fails the test instead of passing silently.
inline std::unique_ptr<gpu::Device> device() {
    auto d = gpu::Device::create(native_backend(), nullptr, nullptr);
    if (!d) {
        if (gpu_required()) FAIL("no GPU device and NTSCRT_REQUIRE_GPU is set: " << gpu::Device::last_create_error());
        MESSAGE("no GPU device: " << gpu::Device::last_create_error());
    }
    return d;
}

inline void set_env(const char *k, const char *v) {
#ifdef _WIN32
    _putenv_s(k, v ? v : "");
#else
    if (v) setenv(k, v, 1); else unsetenv(k);
#endif
}

// BGRA8 texture from tightly packed bytes.
inline std::shared_ptr<gpu::Texture> bgra(gpu::Device &d, int w, int h, const std::vector<uint8_t> &px,
                                          bool rt = false) {
    auto t = d.create_texture({w, h, gpu::Format::BGRA8, true, rt});
    d.upload(*t, px.data(), ptrdiff_t(w) * 4);
    return t;
}

inline std::vector<uint8_t> read(gpu::Device &d, const gpu::Texture &t) {
    d.finish();
    std::vector<uint8_t> px(size_t(t.desc().width) * t.desc().height * 4);
    d.download(t, px.data(), ptrdiff_t(t.desc().width) * 4);
    return px;
}

// Gray image: B=G=R=gray(x,y), A=255.
template <class F> std::vector<uint8_t> gray_image(int w, int h, F gray) {
    std::vector<uint8_t> px(size_t(w) * h * 4, 255);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            auto g = uint8_t(gray(x, y));
            uint8_t *p = &px[(size_t(y) * w + x) * 4];
            p[0] = p[1] = p[2] = g;
        }
    return px;
}

// Green channel as ints, [y][x].
inline std::vector<std::vector<int>> green(const std::vector<uint8_t> &px, int w, int h) {
    std::vector<std::vector<int>> g(h, std::vector<int>(w));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) g[y][x] = px[(size_t(y) * w + x) * 4 + 1];
    return g;
}

}  // namespace gputest
