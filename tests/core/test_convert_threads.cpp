// The host <-> internal pixel conversions run on the thread pool for large
// pictures: the result must not depend on the thread count.
#include <doctest/doctest.h>
#include <ntscrt/ntscrt.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

#include "pixel_convert.h"
#include "stages/digital/parallel.h"

using namespace ntscrt;

namespace {
constexpr int W = 1031, H = 517;  // > 2^18 pixels, odd sizes

std::vector<uint8_t> source(ntscrt_pixel_format f, bool with_alpha) {
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> u(-0.1f, 1.1f);
    const size_t bpp = f == NTSCRT_RGBA32F ? 16 : (f == NTSCRT_RGBA16 || f == NTSCRT_RGBA16F) ? 8 : 4;
    std::vector<uint8_t> px(size_t(W) * H * bpp);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            float v[4] = {u(rng), u(rng), u(rng), 1.f};
            if (with_alpha && y % 7 == 3) v[3] = x % 5 == 0 ? 0.f : (x % 3) * 0.4f;  // some rows: holes, partial
            uint8_t *p = &px[(size_t(y) * W + x) * bpp];
            for (int c = 0; c < 4; ++c) {
                const float cl = v[c] < 0 ? 0 : v[c] > 1 ? 1 : v[c];
                if (f == NTSCRT_RGBA8) p[c] = uint8_t(cl * 255.f + 0.5f);
                else if (f == NTSCRT_RGBA16) reinterpret_cast<uint16_t *>(p)[c] = uint16_t(cl * 65535.f + 0.5f);
                else if (f == NTSCRT_RGBA32F) reinterpret_cast<float *>(p)[c] = v[c];
                else reinterpret_cast<uint16_t *>(p)[c] = c == 3 && cl == 1.f ? uint16_t(0x3c00) : uint16_t(0x3400 + (uint16_t(cl * 1000) & 0x3ff));
            }
        }
    return px;
}

struct Out {
    std::vector<uint8_t> bgra, alpha_bytes, host8, hosth;
};

Out run(ntscrt_pixel_format f, bool with_alpha, AlphaMode mode, bool premult) {
    auto src_px = source(f, with_alpha);
    const size_t bpp = src_px.size() / (size_t(W) * H);
    ntscrt_image src{W, H, ptrdiff_t(W * bpp), f, src_px.data()};
    Out o;
    ImageBGRA8 img;
    std::vector<float> alpha;
    to_bgra8(src, img, &alpha, mode, premult);
    o.bgra = img.px;
    o.alpha_bytes.resize(alpha.size() * 4);
    if (!alpha.empty()) std::memcpy(o.alpha_bytes.data(), alpha.data(), o.alpha_bytes.size());
    // back to the host format, from the 8-bit picture and from a half-float one
    std::vector<uint8_t> dst(src_px.size());
    ntscrt_image d{W, H, ptrdiff_t(W * bpp), f, dst.data()};
    from_bgra8(img, &alpha, d, mode, premult);
    o.host8 = dst;
    std::vector<uint16_t> half(size_t(W) * H * 4);
    for (size_t i = 0; i < half.size(); ++i) half[i] = uint16_t(0x3000 + (i * 7919) % 0x0fff);
    from_rgba16f(half.data(), W, H, &alpha, d, mode, premult);
    o.hosth = dst;
    return o;
}
}  // namespace

TEST_CASE("pixel conversions give the same bytes on 1 thread and on many") {
    for (ntscrt_pixel_format f : {NTSCRT_RGBA8, NTSCRT_RGBA16, NTSCRT_RGBA16F, NTSCRT_RGBA32F})
        for (bool with_alpha : {false, true})
            for (AlphaMode mode : {AlphaMode::Clip, AlphaMode::Spill})
                for (bool premult : {false, true}) {
                    CAPTURE(int(f));
                    CAPTURE(with_alpha);
                    CAPTURE(int(mode));
                    CAPTURE(premult);
                    digital::set_thread_count(1);
                    const Out a = run(f, with_alpha, mode, premult);
                    digital::set_thread_count(8);
                    const Out b = run(f, with_alpha, mode, premult);
                    CHECK(a.bgra == b.bgra);
                    CHECK(a.alpha_bytes == b.alpha_bytes);
                    CHECK(a.host8 == b.host8);
                    CHECK(a.hosth == b.hosth);
                    CHECK(a.alpha_bytes.empty() == !with_alpha);  // opaque sources store no alpha
                }
    digital::set_thread_count(0);
}

namespace {
uint8_t ref_q8(float v) { return uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); }
uint16_t ref_q16(float v) { return uint16_t(std::lround(std::clamp(v, 0.f, 1.f) * 65535.f)); }
}  // namespace

TEST_CASE("8- and 16-bit rounding matches lround (every value the conversions can meet)") {
    // to_bgra8 from float: all floats in [-0, 1.0] (+ a few outside) when
    // NTSCRT_TEST_EXHAUSTIVE is set, else every 61st bit pattern.
    const char *ex = std::getenv("NTSCRT_TEST_EXHAUSTIVE");
    const uint32_t stride = ex && *ex == '1' ? 1 : 61;
    const int w = 1024, h = 1024;
    std::vector<float> px(size_t(w) * h * 4);
    std::vector<uint32_t> bits;
    for (uint64_t b = 0; b <= 0x3f800000u; b += stride) bits.push_back(uint32_t(b));
    for (float extra : {-1.f, -0.f, 1.5f, 1e30f, std::numeric_limits<float>::quiet_NaN(),
                        std::numeric_limits<float>::infinity()}) {
        uint32_t b;
        std::memcpy(&b, &extra, 4);
        bits.push_back(b);
    }
    size_t bad = 0;
    for (size_t off = 0; off < bits.size(); off += size_t(w) * h * 3) {
        size_t n = 0;
        for (size_t i = 0; i < size_t(w) * h; ++i) {
            for (int c = 0; c < 3; ++c, ++n) {
                const uint32_t b = off + n < bits.size() ? bits[off + n] : 0;
                std::memcpy(&px[i * 4 + c], &b, 4);
            }
            px[i * 4 + 3] = 1.f;
        }
        ntscrt_image src{w, h, ptrdiff_t(w) * 16, NTSCRT_RGBA32F, px.data()};
        ImageBGRA8 img;
        to_bgra8(src, img, nullptr, AlphaMode::Clip, false);
        for (size_t i = 0; i < size_t(w) * h; ++i)
            for (int c = 0; c < 3; ++c)
                bad += img.px[i * 4 + (2 - c)] != ref_q8(px[i * 4 + c]);
    }
    CHECK(bad == 0);
    // 16-bit hosts get k/255 (from_bgra8) and half floats (from_rgba16f): all of them.
    std::vector<uint16_t> halves;
    for (uint32_t hbits = 0; hbits < 0x10000; ++hbits) halves.push_back(uint16_t(hbits));
    const int hw = 256, hh = int(halves.size() / 3 / hw) + 1;
    std::vector<uint16_t> hp(size_t(hw) * hh * 4, 0x3c00);
    for (size_t i = 0; i < halves.size(); ++i) hp[(i / 3) * 4 + i % 3] = halves[i];
    std::vector<uint16_t> out(hp.size());
    ntscrt_image d{hw, hh, ptrdiff_t(hw) * 8, NTSCRT_RGBA16, out.data()};
    from_rgba16f(hp.data(), hw, hh, nullptr, d, AlphaMode::Clip, false);
    size_t bad16 = 0;
    for (size_t i = 0; i < halves.size(); ++i) {
        // what from_rgba16f feeds the 16-bit write: the half's value clamped to 0..1 (NaN -> 0)
        const uint16_t hb = halves[i];
        float f;
        const uint32_t s = (hb >> 15) & 1u, e = (hb >> 10) & 0x1fu, m = hb & 0x3ffu;
        if (e == 0) f = (s ? -1.f : 1.f) * std::ldexp(float(m), -24);
        else if (e == 31) f = m ? std::numeric_limits<float>::quiet_NaN() : (s ? -1.f : 1.f) * INFINITY;
        else f = (s ? -1.f : 1.f) * std::ldexp(float(m | 0x400u), int(e) - 25);
        const float u = f > 0.f ? (f < 1.f ? f : 1.f) : 0.f;
        bad16 += out[(i / 3) * 4 + i % 3] != ref_q16(u);
    }
    CHECK(bad16 == 0);
    ImageBGRA8 k;
    k.resize(256, 1);
    for (int i = 0; i < 256; ++i) k.px[size_t(i) * 4] = k.px[size_t(i) * 4 + 1] = k.px[size_t(i) * 4 + 2] = uint8_t(i);
    std::vector<uint16_t> o16(256 * 4);
    ntscrt_image d16{256, 1, 256 * 8, NTSCRT_RGBA16, o16.data()};
    from_bgra8(k, nullptr, d16, AlphaMode::Clip, false);
    for (int i = 0; i < 256; ++i) CHECK(o16[size_t(i) * 4] == ref_q16(i / 255.f));
}
