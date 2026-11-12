// HAL round trips: upload -> dispatch -> download, formats, buffers with
// inferred strides, UAV->SRV hazards, samplers, copy, wrap_native_texture.
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>

#include "gpu_test_util.h"

using namespace gputest;

namespace {

struct Params {
    float gain[4];
    uint32_t size[2], pad[2];
};

void roundtrip_formats(gpu::Device &d) {
    const int w = 5, h = 3;  // odd sizes: partial threadgroups
    for (auto f : {gpu::Format::BGRA8, gpu::Format::RGBA8, gpu::Format::RGBA16F, gpu::Format::RGBA32F,
                   gpu::Format::R32F, gpu::Format::RG32F}) {
        CAPTURE(int(f));
        const int bpp = f == gpu::Format::RGBA32F ? 16 : (f == gpu::Format::RGBA16F || f == gpu::Format::RG32F) ? 8 : 4;
        std::vector<uint8_t> in(size_t(w) * h * bpp);
        if (f == gpu::Format::RGBA32F || f == gpu::Format::R32F || f == gpu::Format::RG32F) {
            auto *fl = reinterpret_cast<float *>(in.data());
            for (size_t i = 0; i < in.size() / 4; ++i) fl[i] = float(i) * 0.25f;
        } else if (f == gpu::Format::RGBA16F) {
            auto *u = reinterpret_cast<uint16_t *>(in.data());
            for (size_t i = 0; i < in.size() / 2; ++i) u[i] = uint16_t(0x3C00 + i);  // 1.0 + i ulp
        } else {
            for (size_t i = 0; i < in.size(); ++i) in[i] = uint8_t(i * 7);
        }
        auto t = d.create_texture({w, h, f, true, false});
        d.upload(*t, in.data(), ptrdiff_t(w) * bpp);
        std::vector<uint8_t> out(in.size());
        d.finish();
        d.download(*t, out.data(), ptrdiff_t(w) * bpp);
        CHECK(out == in);

        // Bottom-up layout (negative stride) round-trips too.
        std::vector<uint8_t> out2(in.size());
        d.upload(*t, in.data() + size_t(h - 1) * w * bpp, -ptrdiff_t(w) * bpp);
        d.download(*t, out2.data() + size_t(h - 1) * w * bpp, -ptrdiff_t(w) * bpp);
        CHECK(out2 == in);
    }
}

void dispatch_scale(gpu::Device &d) {
    const int w = 13, h = 9;
    std::vector<uint8_t> px(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t *p = &px[(size_t(y) * w + x) * 4];
            p[0] = uint8_t(10 * x);  // B
            p[1] = uint8_t(20 * y);  // G
            p[2] = 200;              // R
            p[3] = 255;
        }
    auto src = bgra(d, w, h, px);
    auto dst = d.create_texture({w, h, gpu::Format::BGRA8, true, false});
    std::vector<float> offs(size_t(h) * 4, 0.f);
    for (int y = 0; y < h; ++y) offs[size_t(y) * 4 + 3] = -0.5f;  // alpha - 0.5
    auto buf = d.create_buffer(offs.size() * sizeof(float), offs.data());
    Params p{{0.5f, 0.5f, 0.5f, 1.f}, {uint32_t(w), uint32_t(h)}, {0, 0}};
    gpu::Bindings b;
    b.constants = &p;
    b.constants_size = sizeof p;
    b.textures = {src.get()};
    b.buffers = {buf.get()};
    b.out_textures = {dst.get()};
    d.dispatch(d.kernel("roundtrip/scale"), b, w, h);
    auto out = read(d, *dst);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const uint8_t *o = &out[(size_t(y) * w + x) * 4];
            CHECK(std::abs(int(o[0]) - 5 * x) <= 1);   // B halved, stays in B
            CHECK(std::abs(int(o[1]) - 10 * y) <= 1);
            CHECK(std::abs(int(o[2]) - 100) <= 1);
            CHECK(std::abs(int(o[3]) - 128) <= 1);
        }
}

void buffer_hazards(gpu::Device &d) {
    // sum_rows writes a RWStructuredBuffer (u8); row_mean reads it as t9;
    // then the source changes and the buffer is rewritten and re-read.
    const int w = 16, h = 8;
    auto rowsum = d.create_buffer(size_t(h) * 16);
    auto zero = d.create_buffer(size_t(h) * 16);
    auto dst = d.create_texture({w, h, gpu::Format::RGBA8, true, false});
    for (int round = 0; round < 2; ++round) {
        auto px = gray_image(w, h, [&](int x, int y) { return round == 0 ? 8 * y : (x % 2 ? 200 : 100); });
        auto src = bgra(d, w, h, px);
        Params p{{1, 1, 1, 1}, {uint32_t(w), uint32_t(h)}, {0, 0}};
        gpu::Bindings a;
        a.constants = &p;
        a.constants_size = sizeof p;
        a.textures = {src.get()};
        a.out_buffers = {rowsum.get()};
        d.dispatch(d.kernel("roundtrip/sum_rows"), a, h, 1);
        gpu::Bindings m;
        m.constants = &p;
        m.constants_size = sizeof p;
        m.buffers = {zero.get(), rowsum.get()};  // t8 unused, t9 = rowsum
        m.out_textures = {dst.get()};
        d.dispatch(d.kernel("roundtrip/row_mean"), m, w, h);
        auto out = read(d, *dst);
        for (int y = 0; y < h; ++y) {
            int want = round == 0 ? 8 * y : 150;
            CHECK(std::abs(int(out[(size_t(y) * w + 3) * 4 + 1]) - want) <= 1);
        }
    }
}

void samplers_and_copy(gpu::Device &d) {
    auto src = bgra(d, 2, 1, {0, 0, 0, 255, 200, 200, 200, 255});
    auto dst = d.create_texture({2, 1, gpu::Format::BGRA8, true, false});
    Params p{{1, 1, 1, 1}, {2, 1}, {0, 0}};
    gpu::Bindings b;
    b.constants = &p;
    b.constants_size = sizeof p;
    b.textures = {src.get()};
    b.out_textures = {dst.get()};
    d.dispatch(d.kernel("roundtrip/samplers"), b, 2, 1);
    auto out = read(d, *dst);
    CHECK(out[1] == 0);                               // point at texel 0
    CHECK(std::abs(int(out[4 + 1]) - 100) <= 1);      // linear halfway

    // Same-format copy and a format-converting copy (builtin kernel).
    auto c1 = d.create_texture({2, 1, gpu::Format::BGRA8, true, false});
    d.copy(*src, *c1);
    CHECK(read(d, *c1) == read(d, *src));
    auto c2 = d.create_texture({2, 1, gpu::Format::RGBA16F, true, false});
    d.copy(*src, *c2);
    auto c3 = d.create_texture({2, 1, gpu::Format::BGRA8, true, false});
    d.copy(*c2, *c3);
    CHECK(read(d, *c3) == read(d, *src));

    // wrap_native_texture sees the same pixels.
    auto wrapped = d.wrap_native_texture(src->native());
    CHECK(wrapped->desc().width == 2);
    CHECK((wrapped->desc().format == gpu::Format::BGRA8 || wrapped->desc().format == gpu::Format::RGBA8));
    CHECK(read(d, *wrapped) == read(d, *src));
}

// write() / read(): the zero-copy upload and banded readback give the same
// bytes as upload() / download(), for any band count, repeatedly (staging reuse).
void write_read(gpu::Device &d) {
    const int w = 37, h = 23;
    for (auto f : {gpu::Format::BGRA8, gpu::Format::RGBA16F, gpu::Format::RGBA32F})
        for (bool shader_write : {false, true}) {
            CAPTURE(int(f));
            CAPTURE(shader_write);
            const int bpp = f == gpu::Format::RGBA32F ? 16 : f == gpu::Format::RGBA16F ? 8 : 4;
            const size_t tight = size_t(w) * bpp;
            auto t = d.create_texture({w, h, f, shader_write, false});
            for (int round = 0; round < 3; ++round) {
                std::vector<uint8_t> in(tight * h);
                for (size_t i = 0; i < in.size(); ++i) in[i] = uint8_t(i * 13 + round * 101);
                if (f == gpu::Format::RGBA16F)  // finite halves (no NaN payload questions)
                    for (size_t i = 1; i < in.size(); i += 2) in[i] &= 0x3b;
                if (f == gpu::Format::RGBA32F)
                    for (size_t i = 3; i < in.size(); i += 4) in[i] &= 0x3f;
                d.write(*t, [&](uint8_t *rows, ptrdiff_t pitch) {
                    CHECK(pitch >= ptrdiff_t(tight));
                    for (int y = 0; y < h; ++y) std::memcpy(rows + pitch * y, in.data() + tight * y, tight);
                });
                std::vector<uint8_t> down(in.size());
                d.download(*t, down.data(), ptrdiff_t(tight));
                CHECK(down == in);
                for (int bands : {1, 2, 5, 23, 40}) {
                    CAPTURE(bands);
                    std::vector<uint8_t> out(in.size(), 0);
                    int next = 0;
                    d.read(
                        *t,
                        [&](const uint8_t *rows, ptrdiff_t pitch, int y0, int y1) {
                            CHECK(y0 == next);  // in order, no gaps
                            CHECK(y1 > y0);
                            next = y1;
                            for (int y = y0; y < y1; ++y)
                                std::memcpy(out.data() + tight * y, rows + pitch * (y - y0), tight);
                        },
                        bands);
                    CHECK(next == h);
                    CHECK(out == in);
                }
            }
        }
    // A throwing callback leaves the device usable.
    auto t = d.create_texture({w, h, gpu::Format::BGRA8, true, false});
    CHECK_THROWS(d.read(*t, [](const uint8_t *, ptrdiff_t, int, int) { throw std::runtime_error("x"); }, 3));
    CHECK_THROWS(d.write(*t, [](uint8_t *, ptrdiff_t) { throw std::runtime_error("x"); }));
    std::vector<uint8_t> px(size_t(w) * h * 4, 7);
    d.upload(*t, px.data(), ptrdiff_t(w) * 4);
    CHECK(read(d, *t) == px);
}

void run_all(gpu::Device &d) {
    SUBCASE("formats round-trip") { roundtrip_formats(d); }
    SUBCASE("write / read (zero-copy, banded)") { write_read(d); }
    SUBCASE("dispatch with constants + structured buffer") { dispatch_scale(d); }
    SUBCASE("UAV buffer then SRV, repeated") { buffer_hazards(d); }
    SUBCASE("samplers, copy, wrap") { samplers_and_copy(d); }
    SUBCASE("unknown kernel throws") { CHECK_THROWS(d.kernel("roundtrip/nope")); }
}

}  // namespace

TEST_CASE("HAL on the default device") {
    auto d = device();
    if (!d) return;
    run_all(*d);
}

#ifdef _WIN32
TEST_CASE("HAL on WARP") {
    set_env("NTSCRT_D3D11_WARP", "1");
    auto d = device();
    set_env("NTSCRT_D3D11_WARP", nullptr);
    REQUIRE(d);  // WARP is always there on Windows 10+
    run_all(*d);
}

TEST_CASE("HAL with BGRA8 stored as RGBA8 (no typed BGRA UAV)") {
    set_env("NTSCRT_D3D11_NO_BGRA_UAV", "1");
    auto d = device();
    REQUIRE(d);
    run_all(*d);
    set_env("NTSCRT_D3D11_NO_BGRA_UAV", nullptr);
}

TEST_CASE("HAL without D3D11.4 fences (queries)") {
    set_env("NTSCRT_D3D11_NO_FENCE", "1");
    auto d = device();
    set_env("NTSCRT_D3D11_NO_FENCE", nullptr);
    REQUIRE(d);
    run_all(*d);
}

TEST_CASE("wrong backend returns null with a message, no throw") {
    std::unique_ptr<gpu::Device> d;
    CHECK_NOTHROW(d = gpu::Device::create(gpu::Backend::Metal, nullptr, nullptr));
    CHECK(!d);
    CHECK(std::string(gpu::Device::last_create_error()).find("Metal") != std::string::npos);
}
#endif
