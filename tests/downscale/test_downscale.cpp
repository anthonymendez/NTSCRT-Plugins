// Downscaler methods vs. a CPU reference of upstream Downscaler.swift's MSL.
#include <cmath>
#include <algorithm>
#include <functional>

#include "../gpu/gpu_test_util.h"
#include "stages/downscale/downscale_kernels.h"
#include "stages/stages.h"

using namespace gputest;

namespace {

using Img = std::vector<float>;  // w*h*4, 0..1

Img to_float(const std::vector<uint8_t> &px) {
    Img f(px.size());
    for (size_t i = 0; i < px.size(); ++i) f[i] = px[i] / 255.f;
    return f;
}
Img quantize(Img f) {
    for (auto &v : f) v = std::round(std::clamp(v, 0.f, 1.f) * 255.f) / 255.f;
    return f;
}

float w_bilinear(float t) { return std::max(0.f, 1.f - std::fabs(t)); }
float w_nearestaa(float t) { return std::exp(t * t * -4.0816f); }
float w_bicubic(float t) {
    float x = std::fabs(t);
    const float B = 1.f / 3, C = 1.f / 3;
    float x2 = x * x, x3 = x2 * x;
    if (x < 1) return ((12 - 9 * B - 6 * C) * x3 + (-18 + 12 * B + 6 * C) * x2 + (6 - 2 * B)) / 6;
    if (x < 2) return ((-B - 6 * C) * x3 + (6 * B + 30 * C) * x2 + (-12 * B - 48 * C) * x + (8 * B + 24 * C)) / 6;
    return 0;
}
float sinc(float x) {
    if (std::fabs(x) < 1e-6f) return 1;
    float xp = x * 3.14159265f;
    return std::sin(xp) / xp;
}
float w_lanczos(float t) { return std::fabs(t) >= 3 ? 0.f : sinc(t) * sinc(t / 3); }

// One separable pass along x (horizontal) or y.
Img pass(const Img &src, int sw, int sh, int dw, int dh, bool horiz, std::function<float(float)> wf, float radius) {
    int ow = horiz ? dw : dw, oh = horiz ? sh : dh;
    int iw = horiz ? sw : dw;
    Img out(size_t(ow) * oh * 4);
    for (int y = 0; y < oh; ++y)
        for (int x = 0; x < ow; ++x) {
            int n = horiz ? sw : sh, m = horiz ? dw : dh, g = horiz ? x : y;
            float scale = std::max(1.f, float(n) / float(m));
            float center = (float(g) + 0.5f) * float(n) / float(m) - 0.5f;
            float support = radius * scale;
            int lo = int(std::ceil(center - support)), hi = int(std::floor(center + support));
            float acc[4] = {}, ws = 0;
            for (int k = lo; k <= hi; ++k) {
                float w = wf((float(k) - center) / scale);
                int c = std::clamp(k, 0, n - 1);
                int sx = horiz ? c : x, sy = horiz ? y : c;
                for (int ch = 0; ch < 4; ++ch) acc[ch] += src[(size_t(sy) * iw + sx) * 4 + ch] * w;
                ws += w;
            }
            for (int ch = 0; ch < 4; ++ch) out[(size_t(y) * ow + x) * 4 + ch] = acc[ch] / std::max(ws, 1e-6f);
        }
    return out;
}

Img reference(const Img &src, int sw, int sh, int dw, int dh, DownscaleMethod m) {
    Img out(size_t(dw) * dh * 4);
    if (m == DownscaleMethod::Nearest) {
        for (int y = 0; y < dh; ++y)
            for (int x = 0; x < dw; ++x) {
                int sx = std::min(sw - 1, int((x + 0.5f) / dw * sw)), sy = std::min(sh - 1, int((y + 0.5f) / dh * sh));
                for (int c = 0; c < 4; ++c) out[(size_t(y) * dw + x) * 4 + c] = src[(size_t(sy) * sw + sx) * 4 + c];
            }
        return out;
    }
    if (m == DownscaleMethod::Area) {
        for (int y = 0; y < dh; ++y)
            for (int x = 0; x < dw; ++x) {
                float sx0 = float(x) * sw / dw, sx1 = float(x + 1) * sw / dw;
                float sy0 = float(y) * sh / dh, sy1 = float(y + 1) * sh / dh;
                float acc[4] = {}, ws = 0;
                for (int yy = int(std::floor(sy0)); yy < int(std::ceil(sy1)); ++yy) {
                    float fy = std::clamp(std::min(yy + 1.f, sy1) - std::max(float(yy), sy0), 0.f, 1.f);
                    for (int xx = int(std::floor(sx0)); xx < int(std::ceil(sx1)); ++xx) {
                        float fx = std::clamp(std::min(xx + 1.f, sx1) - std::max(float(xx), sx0), 0.f, 1.f);
                        int cx = std::clamp(xx, 0, sw - 1), cy = std::clamp(yy, 0, sh - 1);
                        for (int c = 0; c < 4; ++c) acc[c] += src[(size_t(cy) * sw + cx) * 4 + c] * fx * fy;
                        ws += fx * fy;
                    }
                }
                for (int c = 0; c < 4; ++c) out[(size_t(y) * dw + x) * 4 + c] = acc[c] / std::max(ws, 1e-6f);
            }
        return out;
    }
    std::function<float(float)> wf = m == DownscaleMethod::NearestAA ? w_nearestaa
                                   : m == DownscaleMethod::Bilinear  ? w_bilinear
                                   : m == DownscaleMethod::Bicubic   ? w_bicubic
                                                                     : w_lanczos;
    float r = m == DownscaleMethod::Bicubic ? 2.f : m == DownscaleMethod::Lanczos ? 3.f : 1.f;
    Img mid = quantize(pass(src, sw, sh, dw, dh, true, wf, r));  // scratch is BGRA8 like upstream
    return pass(mid, sw, sh, dw, dh, false, wf, r);
}

std::vector<uint8_t> pattern(int w, int h) {
    std::vector<uint8_t> px(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t *p = &px[(size_t(y) * w + x) * 4];
            p[0] = uint8_t((x * 37 + y * 11) % 256);
            p[1] = uint8_t(((x / 3 + y / 2) % 2) * 220 + 10);
            p[2] = uint8_t(std::min(255, x * 255 / std::max(1, w - 1)));
            p[3] = 255;
        }
    return px;
}

}  // namespace

TEST_CASE("downscale methods match the CPU reference") {
    auto d = device();
    if (!d) return;
    const int sw = 97, sh = 61;
    auto px = pattern(sw, sh);
    auto src = bgra(*d, sw, sh, px);
    Img fsrc = to_float(px);
    struct Size { int w, h; };
    for (auto m : {DownscaleMethod::Nearest, DownscaleMethod::NearestAA, DownscaleMethod::Bilinear,
                   DownscaleMethod::Bicubic, DownscaleMethod::Lanczos, DownscaleMethod::Area}) {
        for (Size s : {Size{31, 20}, Size{48, 30}, Size{97, 61}, Size{130, 70}}) {
            CAPTURE(int(m));
            CAPTURE(s.w);
            auto dst = d->create_texture({s.w, s.h, gpu::Format::BGRA8, true, false});
            std::shared_ptr<gpu::Texture> scratch;
            downscale_encode(*d, *src, *dst, m, scratch);
            auto out = read(*d, *dst);
            Img ref = reference(fsrc, sw, sh, s.w, s.h, m);
            int worst = 0;
            for (size_t i = 0; i < out.size(); ++i)
                worst = std::max(worst, std::abs(int(out[i]) - int(std::lround(std::clamp(ref[i], 0.f, 1.f) * 255))));
            CHECK(worst <= 2);
        }
    }
}

TEST_CASE("Downscaler stage: width-only, height follows aspect, look params") {
    CHECK(downscale_height(320, 1920, 1080) == 180);
    CHECK(downscale_height(256, 1920, 1080) == 144);
    CHECK(downscale_height(256, 640, 480) == 192);
    CHECK(downscale_height(10, 1920, 1080) == 16);   // at least 16
    CHECK(downscale_height(320, 1000, 999) == 320);  // rounded to even
    CHECK(parse_downscale_method("nearest+") == DownscaleMethod::NearestAA);
    CHECK(parse_downscale_method("bogus") == DownscaleMethod::Nearest);

    auto d = device();
    if (!d) return;
    Downscaler ds(*d);
    auto src = bgra(*d, 160, 90, pattern(160, 90));
    FrameContext ctx;
    json look = {{"downscale", {{"enabled", true}, {"method", "area"}, {"width", 64}}}};
    ctx.look = &look;
    auto out = ds.run(src, ctx);
    CHECK(out->desc().width == 64);
    CHECK(out->desc().height == 36);
    look["downscale"]["enabled"] = false;
    CHECK(ds.run(src, ctx) == src);
    json none = json::object();  // upstream defaults: on, 320, nearest
    ctx.look = &none;
    CHECK(ds.run(src, ctx)->desc().width == 320);
}
