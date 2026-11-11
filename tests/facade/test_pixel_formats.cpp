// Host pixel formats at the façade boundary: 16-bit unorm and half-float
// round trips (every alpha value), half-float edge cases (subnormals, NaN,
// infinities, out-of-range values), and two façade paths the cache tests
// don't cover: render_scale != 1 and crop with a transparent source.
#include <doctest/doctest.h>
#include <ntscrt/ntscrt.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "pixel_convert.h"
#include "testing.h"

using namespace ntscrt;

namespace {

float half_bits_to_float(uint16_t h) {  // reference decoder, independent of the core's
    const int s = h >> 15, e = (h >> 10) & 31, m = h & 1023;
    float v;
    if (e == 0) v = std::ldexp(float(m), -24);
    else if (e == 31) v = m ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
    else v = std::ldexp(float(m + 1024), e - 25);
    return s ? -v : v;
}

// to_bgra8 then from_bgra8 in the same format and alpha convention (what a
// render with every stage off does to the host's pixels).
template <class T> std::vector<T> round_trip(const std::vector<T> &px, int w, int h, ntscrt_pixel_format f,
                                             AlphaMode mode = AlphaMode::Clip, bool premult = false) {
    ntscrt_image src{w, h, ptrdiff_t(w) * 4 * ptrdiff_t(sizeof(T)), f, const_cast<T *>(px.data())};
    ImageBGRA8 im;
    std::vector<float> alpha;
    to_bgra8(src, im, &alpha, mode, premult);
    std::vector<T> out(px.size());
    ntscrt_image dst{w, h, src.row_bytes, f, out.data()};
    from_bgra8(im, &alpha, dst, mode, premult);
    return out;
}

ntscrt_instance *make() {
    ntscrt_create_info ci{NTSCRT_ABI_VERSION, NTSCRT_GPU_AUTO, nullptr, nullptr, NTSCRT_BUILT_RESOURCES_DIR};
    ntscrt_instance *inst = nullptr;
    REQUIRE(ntscrt_create(&ci, &inst) == NTSCRT_OK);
    return inst;
}

bool gpu_or_skip(ntscrt_instance *inst) {
    if (testing::stats(inst).has_gpu) return true;
    const char *v = std::getenv("NTSCRT_REQUIRE_GPU");
    if (v && *v && std::string(v) != "0") FAIL("no GPU device and NTSCRT_REQUIRE_GPU is set");
    MESSAGE("no GPU device: skipped");
    return false;
}

const char *kAllOff = R"("downscale":{"enabled":false},"ntsc":{"enabled":false},"glitch":{"enabled":false},)"
                      R"("digital":{"enabled":false},"shader":{"enabled":false})";

}  // namespace

TEST_CASE("RGBA16: every alpha value survives, colour to within an 8-bit step") {
    const int w = 256, h = 256;  // one pixel per 16-bit alpha value
    std::vector<uint16_t> px(size_t(w) * h * 4);
    for (int i = 0; i < w * h; ++i) {
        px[i * 4 + 0] = uint16_t(i * 2654435761u >> 16);
        px[i * 4 + 1] = uint16_t(65535 - i);
        px[i * 4 + 2] = uint16_t((i * 40503u) & 0xffff);
        px[i * 4 + 3] = uint16_t(i);
    }
    auto out = round_trip(px, w, h, NTSCRT_RGBA16);
    int alpha_off = 0, colour_off = 0;
    for (int i = 0; i < w * h; ++i) {
        alpha_off += out[i * 4 + 3] != px[i * 4 + 3];
        if (px[i * 4 + 3] <= 128) continue;  // ~transparent: Clip refills its colour from the neighbours
        for (int c = 0; c < 3; ++c)  // 8-bit internal: at most half a step (128.5) off, plus rounding
            colour_off += std::abs(int(out[i * 4 + c]) - int(px[i * 4 + c])) > 129;
    }
    CHECK(alpha_off == 0);
    CHECK(colour_off == 0);
}

TEST_CASE("RGBA16F: every half alpha in [0, 1] round-trips; subnormals flush to zero") {
    std::vector<uint16_t> vals;
    for (uint32_t b = 0; b <= 0x3c00; ++b) vals.push_back(uint16_t(b));  // +0 .. 1.0, subnormals included
    const int w = int(vals.size()), h = 1;
    std::vector<uint16_t> px(size_t(w) * 4);
    for (int i = 0; i < w; ++i) {
        px[i * 4 + 0] = 0x3800;  // 0.5
        px[i * 4 + 1] = 0x3400;  // 0.25
        px[i * 4 + 2] = 0x3c00;  // 1.0
        px[i * 4 + 3] = vals[i];
    }
    auto out = round_trip(px, w, h, NTSCRT_RGBA16F);
    int normal_off = 0, subnormal_off = 0;
    for (int i = 0; i < w; ++i) {
        const uint16_t in = vals[i], o = out[i * 4 + 3];
        if (in >= 0x0400 || in == 0) normal_off += o != in;  // exact
        else  // subnormal: within the smallest normal (6.1e-5) of the input, never negative / NaN
            subnormal_off += !(std::abs(half_bits_to_float(o) - half_bits_to_float(in)) <= 6.11e-5f);
    }
    CHECK(normal_off == 0);
    CHECK(subnormal_off == 0);
    // Opaque colour: 0.5, 0.25, 1.0 are exact in 8 bits? 0.5 -> 128/255, not exact; within half a step.
    for (int i = w - 4; i < w; ++i) {
        CHECK(half_bits_to_float(out[i * 4 + 0]) == doctest::Approx(0.5).epsilon(0.004));
        CHECK(half_bits_to_float(out[i * 4 + 1]) == doctest::Approx(0.25).epsilon(0.008));
        CHECK(out[i * 4 + 2] == 0x3c00);
    }
}

namespace {
const uint16_t kSpecials[] = {
    0x7e00,  // NaN
    0xfe00,  // -NaN
    0x7c00,  // +inf
    0xfc00,  // -inf
    0x0001,  // smallest subnormal
    0x8001,  // negative subnormal
    0x8000,  // -0
    0xbc00,  // -1
    0x4000,  // 2
    0x7bff,  // 65504 (largest half)
};
bool is_nan_half(uint16_t h) { return (h & 0x7c00) == 0x7c00 && (h & 0x3ff); }

// Every special value in each channel (NaN in alpha only when nan_alpha), the others 0.5 / alpha 1,
// through every alpha convention: the output must be finite and in [0, 1].
void check_specials(bool nan_alpha) {
    for (bool premult : {false, true})
        for (AlphaMode mode : {AlphaMode::Clip, AlphaMode::Spill}) {
            CAPTURE(premult);
            CAPTURE(int(mode));
            std::vector<uint16_t> px;
            for (uint16_t sp : kSpecials)
                for (int c = 0; c < 4; ++c) {
                    if (c == 3 && is_nan_half(sp) != nan_alpha) continue;
                    if (c != 3 && nan_alpha) continue;
                    for (int j = 0; j < 4; ++j) px.push_back(j == c ? sp : (j == 3 ? 0x3c00 : 0x3800));
                }
            const int w = int(px.size() / 4);
            auto out = round_trip(px, w, 1, NTSCRT_RGBA16F, mode, premult);
            for (int i = 0; i < w; ++i)
                for (int c = 0; c < 4; ++c) {
                    const float v = half_bits_to_float(out[i * 4 + c]);
                    INFO("input pixel ", px[i * 4], " ", px[i * 4 + 1], " ", px[i * 4 + 2], " ", px[i * 4 + 3],
                         " (half bits): channel ", c, " = ", v);
                    CHECK(std::isfinite(v));
                    CHECK(v >= 0.f);
                    CHECK(v <= 1.f);
                }
        }
}
}  // namespace

TEST_CASE("RGBA16F: NaN, infinities, negatives and > 1 come out finite and in range") {
    check_specials(false);  // NaN in colour, every special but NaN in alpha
}

// to_bgra8 maps a NaN source alpha to 0 (transparent): it used to keep it
// (std::clamp passes NaN through), which came back as an infinite alpha.
TEST_CASE("RGBA16F: a NaN alpha comes out finite") {
    check_specials(true);
}

TEST_CASE("façade: RGBA16 / RGBA16F frames render with every stage off as the 8-bit conversion does") {
    ntscrt_instance *inst = make();
    const int w = 32, h = 16;
    const std::string look = std::string("{") + kAllOff + "}";
    for (ntscrt_pixel_format f : {NTSCRT_RGBA16, NTSCRT_RGBA16F}) {
        CAPTURE(int(f));
        std::vector<uint16_t> src(size_t(w) * h * 4), dst(src.size(), 0x1234);
        const uint16_t alphas16[] = {0, 16384, 32768, 49152, 65535};
        const uint16_t alphas16f[] = {0x0000, 0x3400, 0x3800, 0x3a00, 0x3c00};  // 0, .25, .5, .75, 1
        for (int i = 0; i < w * h; ++i)
            for (int c = 0; c < 3; ++c)  // any colour: [0, 1] for unorm, 0 .. 1.0 bit patterns for half
                src[i * 4 + c] = f == NTSCRT_RGBA16 ? uint16_t((i * 7919 + c * 31337) & 0xffff)
                                                    : uint16_t((i * 7 + c * 311) % 0x3c01);
        for (int i = 0; i < w * h; ++i) src[i * 4 + 3] = (f == NTSCRT_RGBA16 ? alphas16 : alphas16f)[i % 5];
        ntscrt_image si{w, h, ptrdiff_t(w) * 8, f, src.data()}, di{w, h, ptrdiff_t(w) * 8, f, dst.data()};
        ntscrt_render_args a{};
        a.fps = 30;
        a.look_json = look.c_str();
        a.render_scale = 1;
        REQUIRE(ntscrt_render_cpu(inst, &a, &si, &di) == NTSCRT_OK);
        auto expect = round_trip(src, w, h, f);
        int off = 0;
        for (size_t i = 0; i < dst.size(); ++i) off += dst[i] != expect[i];
        CHECK(off == 0);
    }
    ntscrt_destroy(inst);
}

TEST_CASE("façade: render_scale reaches the stages (pixel-size ntsc-rs settings)") {
    ntscrt_instance *inst = make();
    const int w = 96, h = 64;
    std::vector<uint8_t> src(size_t(w) * h * 4);
    for (int i = 0; i < w * h; ++i) {
        const int x = i % w, y = i / w;
        src[i * 4 + 0] = uint8_t((x / 8 + y / 8) % 2 ? 230 : 20);
        src[i * 4 + 1] = uint8_t(x * 255 / w);
        src[i * 4 + 2] = uint8_t(y * 255 / h);
        src[i * 4 + 3] = 255;
    }
    auto render = [&](const std::string &look, double scale) {
        std::vector<uint8_t> out(src.size());
        ntscrt_image si{w, h, ptrdiff_t(w) * 4, NTSCRT_RGBA8, src.data()}, di{w, h, ptrdiff_t(w) * 4, NTSCRT_RGBA8, out.data()};
        ntscrt_render_args a{};
        a.frame_index = 3;
        a.fps = 30;
        a.look_json = look.c_str();
        a.render_scale = scale;
        ntscrt_flush_caches(inst);
        REQUIRE(ntscrt_render_cpu(inst, &a, &si, &di) == NTSCRT_OK);
        return out;
    };
    const std::string off = R"("downscale":{"enabled":false},"glitch":{"enabled":false},"digital":{"enabled":false},"shader":{"enabled":false})";
    // Settings in pixels (scale_with_video_size off): a proxy render scales them, so it differs.
    const std::string fixed = "{" + off + R"(,"ntsc":{"enabled":true,"settings":{"scale_with_video_size":false}}})";
    CHECK(render(fixed, 0.5) != render(fixed, 1.0));
    // Presets scale with the picture already (scale_with_video_size on): the host scale is not applied twice.
    const std::string sized = "{" + off + R"(,"ntsc":{"enabled":true,"settings":{"scale_with_video_size":true}}})";
    CHECK(render(sized, 0.5) == render(sized, 1.0));
    // Same scale twice: deterministic.
    CHECK(render(fixed, 0.5) == render(fixed, 0.5));
    ntscrt_destroy(inst);
}

TEST_CASE("façade: crop with a transparent source (Clip opaque, Spill from brightness)") {
    ntscrt_instance *inst = make();
    if (!gpu_or_skip(inst)) { ntscrt_destroy(inst); return; }
    const int w = 64, h = 36;
    std::vector<uint8_t> src(size_t(w) * h * 4);
    for (int i = 0; i < w * h; ++i) {
        const int x = i % w;
        const bool on = x >= w / 2;  // right half opaque orange, left half fully transparent
        src[i * 4 + 0] = 220; src[i * 4 + 1] = 120; src[i * 4 + 2] = 30;
        src[i * 4 + 3] = on ? 255 : 0;
    }
    for (const char *mode : {"clip", "spill"}) {
        CAPTURE(mode);
        std::vector<uint8_t> out(src.size(), 7);
        ntscrt_image si{w, h, ptrdiff_t(w) * 4, NTSCRT_RGBA8, src.data()}, di{w, h, ptrdiff_t(w) * 4, NTSCRT_RGBA8, out.data()};
        const std::string look = std::string("{") + kAllOff + R"(,"crop":{"enabled":true,"ratio":"1:1"},"alpha":{"mode":")" +
                                 mode + R"("}})";
        ntscrt_render_args a{};
        a.fps = 30;
        a.look_json = look.c_str();
        a.render_scale = 1;
        REQUIRE(ntscrt_render_cpu(inst, &a, &si, &di) == NTSCRT_OK);
        int bad = 0, opaque = 0, clear = 0;
        for (int i = 0; i < w * h; ++i) {
            const uint8_t *p = &out[i * 4];
            const int mx = std::max({p[0], p[1], p[2]});
            if (std::string(mode) == "clip") {
                bad += p[3] != 255;  // source alpha doesn't line up with a crop: opaque
            } else {
                // straight output, alpha = brightest channel: the colour's max is full scale, or it's clear
                bad += !(p[3] == 0 || mx >= 254);
                opaque += p[3] >= 200;
                clear += p[3] == 0;
            }
        }
        CHECK(bad == 0);
        if (std::string(mode) == "spill") {
            CHECK(opaque > 0);  // the bright picture
            CHECK(clear > 0);   // the black letterbox bars
        }
    }
    ntscrt_destroy(inst);
}
