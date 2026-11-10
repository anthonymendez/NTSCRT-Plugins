// facade/alpha_merge (the last step of ntscrt_render_metal) run through the
// HAL on the local backend: RGB from a BGRA8 texture, alpha from a float source.
#include <doctest/doctest.h>

#include <cmath>
#include <vector>

#include "gpu/gpu.h"

using namespace ntscrt::gpu;

TEST_CASE("alpha_merge keeps pipeline RGB and source alpha") {
#ifdef _WIN32
    auto dev = Device::create(Backend::D3D11, nullptr, nullptr);
#else
    auto dev = Device::create(Backend::Metal, nullptr, nullptr);
#endif
    if (!dev) { MESSAGE("no GPU: ", Device::last_create_error()); return; }
    const int w = 5, h = 3;
    std::vector<uint8_t> rgb(w * h * 4);
    std::vector<float> src(w * h * 4);
    for (int i = 0; i < w * h; ++i) {
        rgb[i * 4 + 0] = uint8_t(10 * i);  // B
        rgb[i * 4 + 1] = 100;              // G
        rgb[i * 4 + 2] = 200;              // R
        rgb[i * 4 + 3] = 255;
        src[i * 4 + 3] = float(i) / (w * h);
    }
    auto t_rgb = dev->create_texture({w, h, Format::BGRA8, true, false});
    auto t_src = dev->create_texture({w, h, Format::RGBA32F, true, false});
    auto t_dst = dev->create_texture({w, h, Format::RGBA32F, true, false});
    dev->upload(*t_rgb, rgb.data(), w * 4);
    dev->upload(*t_src, src.data(), w * 16);
    for (uint32_t use_alpha : {1u, 0u}) {
        struct { uint32_t w, h, use_alpha, pad; } p{uint32_t(w), uint32_t(h), use_alpha, 0};
        Bindings b;
        b.constants = &p;
        b.constants_size = sizeof p;
        b.textures = {t_rgb.get(), t_src.get()};
        b.out_textures = {t_dst.get()};
        dev->dispatch(dev->kernel("facade/alpha_merge"), b, w, h, 1);
        dev->finish();
        std::vector<float> out(w * h * 4);
        dev->download(*t_dst, out.data(), w * 16);
        for (int i = 0; i < w * h; ++i) {
            CHECK(out[i * 4 + 0] == doctest::Approx(200 / 255.0).epsilon(1e-3));
            CHECK(out[i * 4 + 1] == doctest::Approx(100 / 255.0).epsilon(1e-3));
            CHECK(out[i * 4 + 2] == doctest::Approx(10 * i / 255.0).epsilon(1e-3));
            CHECK(out[i * 4 + 3] == doctest::Approx(use_alpha ? src[i * 4 + 3] : 1.0f));
        }
    }
}
