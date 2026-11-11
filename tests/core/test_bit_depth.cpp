// 16-bit and float hosts get the CRT stage's output at RGBA16F precision
// (no 8-bit step at the end); 8-bit hosts and the shader bypass keep the
// 8-bit picture. A smooth gradient through a glow preset shows the gain.
#include <doctest/doctest.h>
#include <ntscrt/ntscrt.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "testing.h"

namespace {

ntscrt_instance *make() {
    ntscrt_create_info ci{NTSCRT_ABI_VERSION, NTSCRT_GPU_AUTO, nullptr, nullptr, NTSCRT_BUILT_RESOURCES_DIR};
    ntscrt_instance *inst = nullptr;
    REQUIRE(ntscrt_create(&ci, &inst) == NTSCRT_OK);
    return inst;
}

uint16_t to_half(float x) {
    uint32_t f;
    std::memcpy(&f, &x, 4);
    uint32_t s = (f >> 16) & 0x8000u;
    int e = int((f >> 23) & 0xffu) - 127 + 15;
    uint32_t m = f & 0x7fffffu;
    if (e <= 0) return uint16_t(s);
    if (e >= 31) return uint16_t(s | 0x7c00u);
    return uint16_t(s | (uint32_t(e) << 10) | ((m + 0x1000u) >> 13));
}
float from_half(uint16_t h) {
    uint32_t s = (h >> 15) & 1u, e = (h >> 10) & 0x1fu, m = h & 0x3ffu, f;
    if (e == 0) f = s << 31;  // (no subnormals in this test's range)
    else f = (s << 31) | ((e + 127 - 15) << 23) | (m << 13);
    float r;
    std::memcpy(&r, &f, 4);
    return r;
}

constexpr int W = 1024, H = 192;

// Horizontal grey ramp 0..1 (alpha 1, or `alpha` in the left half).
std::vector<float> ramp(float alpha = 1.f) {
    std::vector<float> px(size_t(W) * H * 4);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            float *p = &px[(size_t(y) * W + x) * 4];
            p[0] = p[1] = p[2] = float(x) / float(W - 1);
            p[3] = x < W / 2 ? alpha : 1.f;
        }
    return px;
}

// Renders the float ramp into a `fmt` destination; returns RGBA floats.
std::vector<float> render(ntscrt_instance *inst, const std::string &look, ntscrt_pixel_format fmt,
                          const std::vector<float> &src_px, ntscrt_status *st = nullptr) {
    ntscrt_render_args a{};
    a.frame_index = 1;
    a.fps = 30;
    a.look_json = look.c_str();
    a.render_scale = 1;
    ntscrt_image src{W, H, ptrdiff_t(W) * 16, NTSCRT_RGBA32F, const_cast<float *>(src_px.data())};
    const size_t bpp = fmt == NTSCRT_RGBA8 ? 4 : fmt == NTSCRT_RGBA32F ? 16 : 8;
    std::vector<uint8_t> buf(size_t(W) * H * bpp);
    ntscrt_image dst{W, H, ptrdiff_t(W * bpp), fmt, buf.data()};
    ntscrt_status s = ntscrt_render_cpu(inst, &a, &src, &dst);
    if (st) *st = s;
    else REQUIRE(s == NTSCRT_OK);
    std::vector<float> out(size_t(W) * H * 4);
    for (size_t i = 0; i < out.size(); ++i) {
        if (fmt == NTSCRT_RGBA8) out[i] = buf[i] / 255.f;
        else if (fmt == NTSCRT_RGBA16) out[i] = reinterpret_cast<const uint16_t *>(buf.data())[i] / 65535.f;
        else if (fmt == NTSCRT_RGBA16F) out[i] = from_half(reinterpret_cast<const uint16_t *>(buf.data())[i]);
        else out[i] = reinterpret_cast<const float *>(buf.data())[i];
    }
    return out;
}

struct Levels {
    size_t distinct = 0;     // distinct green values over the whole picture
    double off_grid = 0;     // fraction of pixels not on the 1/255 grid
    double flat = 0;         // fraction of neighbours along the ramp (middle rows) with the same value: bands
    double small_step = 0;   // 10th percentile of the non-zero steps there (8-bit: 1/255)
};
Levels levels(const std::vector<float> &px) {
    Levels l;
    std::set<float> d;
    size_t off = 0, n = 0;
    for (size_t i = 1; i < px.size(); i += 4) {
        d.insert(px[i]);
        const double v = px[i] * 255.0;
        off += std::fabs(v - std::round(v)) > 0.02;
        ++n;
    }
    l.distinct = d.size();
    l.off_grid = double(off) / double(n);
    std::vector<double> steps;
    size_t pairs = 0, flat = 0;
    for (int y = H / 2 - 8; y < H / 2 + 8; ++y)
        for (int x = W / 4; x < 3 * W / 4 - 1; ++x) {  // away from the curved edges
            const double s = std::fabs(px[(size_t(y) * W + x + 1) * 4 + 1] - px[(size_t(y) * W + x) * 4 + 1]);
            ++pairs;
            if (s == 0) ++flat;
            else steps.push_back(s);
        }
    l.flat = double(flat) / double(pairs);
    if (!steps.empty()) {
        std::nth_element(steps.begin(), steps.begin() + steps.size() / 10, steps.end());
        l.small_step = steps[steps.size() / 10];
    }
    return l;
}

const std::string kGlow =
    R"({"shader":{"enabled":true,"preset":"glow_gauss"},"ntsc":{"enabled":false},"glitch":{"enabled":false}})";

}  // namespace

TEST_CASE("float and 16-bit hosts get more than 8-bit precision from the CRT stage") {
    ntscrt_instance *inst = make();
    if (!ntscrt::testing::stats(inst).has_gpu) {
        ntscrt_destroy(inst);
        return;
    }
    const auto src = ramp();
    const auto o8 = render(inst, kGlow, NTSCRT_RGBA8, src);
    const auto of = render(inst, kGlow, NTSCRT_RGBA32F, src);
    const auto oh = render(inst, kGlow, NTSCRT_RGBA16F, src);
    const auto o16 = render(inst, kGlow, NTSCRT_RGBA16, src);
    const Levels l8 = levels(o8), lf = levels(of), lh = levels(oh), l16 = levels(o16);
    std::printf("glow_gauss ramp: distinct levels 8-bit %zu, float %zu, half %zu, 16-bit %zu; off the 1/255 grid "
                "float %.0f%%; flat neighbours 8-bit %.0f%%, float %.0f%%; small steps 8-bit %.2f, float %.3f levels\n",
                l8.distinct, lf.distinct, lh.distinct, l16.distinct, lf.off_grid * 100, l8.flat * 100, lf.flat * 100,
                l8.small_step * 255, lf.small_step * 255);
    CHECK(l8.distinct <= 256);
    CHECK(l8.off_grid == 0);
    CHECK(lf.distinct > 8 * l8.distinct);  // far more levels
    CHECK(lh.distinct > 8 * l8.distinct);
    CHECK(l16.distinct > 8 * l8.distinct);
    CHECK(lf.off_grid > 0.75);                 // not an 8-bit picture
    CHECK(lf.small_step * 255 < 0.25);         // steps far finer than one 8-bit level
    CHECK(l8.small_step * 255 > 0.99);

    // Same look: the float picture rounds to the 8-bit one (±1 level from where the 8-bit
    // target rounded in the shader's intermediate steps).
    double sum = 0, worst = 0;
    for (size_t i = 0; i < o8.size(); ++i) {
        if (i % 4 == 3) continue;
        const double d = std::fabs(o8[i] - of[i]);
        sum += d;
        worst = std::max(worst, d);
    }
    const double mean = sum / double(o8.size() * 3 / 4);
    std::printf("float vs 8-bit: mean |diff| %.5f (%.2f levels), max %.5f (%.2f levels)\n", mean, mean * 255,
                worst, worst * 255);
    CHECK(mean * 255 < 0.6);
    CHECK(worst * 255 < 2.5);
    // In range: clamped like an 8-bit target (no values above 1 or below 0).
    for (float v : of) {
        CHECK_MESSAGE((v >= 0.f && v <= 1.f), "value out of 0..1");
        if (!(v >= 0.f && v <= 1.f)) break;
    }
    ntscrt_destroy(inst);
}

TEST_CASE("the shader bypass stays the 8-bit picture on float hosts") {
    ntscrt_instance *inst = make();
    const auto src = ramp();
    const std::string off = R"({"shader":{"enabled":false},"ntsc":{"enabled":false},"glitch":{"enabled":false}})";
    const auto o8 = render(inst, off, NTSCRT_RGBA8, src);
    const auto of = render(inst, off, NTSCRT_RGBA32F, src);
    CHECK(o8 == of);  // k/255 exactly, as before
    ntscrt_destroy(inst);
}

TEST_CASE("alpha is merged at full precision too") {
    ntscrt_instance *inst = make();
    if (!ntscrt::testing::stats(inst).has_gpu) {
        ntscrt_destroy(inst);
        return;
    }
    const auto src = ramp(0.5f);
    for (const char *mode : {"clip", "spill"}) {
        CAPTURE(mode);
        const std::string look = std::string(R"({"shader":{"enabled":true,"preset":"glow_gauss"},"ntsc":{"enabled":false},)") +
                                 R"("glitch":{"enabled":false},"alpha":{"mode":")" + mode + R"("}})";
        const auto o8 = render(inst, look, NTSCRT_RGBA8, src);
        const auto of = render(inst, look, NTSCRT_RGBA32F, src);
        double worst_a = 0;
        size_t n_half = 0;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const size_t i = (size_t(y) * W + x) * 4 + 3;
                worst_a = std::max(worst_a, double(std::fabs(o8[i] - of[i])));
                n_half += x < W / 2 && std::fabs(of[i] - 0.5f) < 1e-6f;
            }
        CHECK(worst_a * 255 < 2.5);
        if (std::string(mode) == "clip") CHECK(n_half == size_t(W / 2) * H);  // source alpha, exactly
    }
    ntscrt_destroy(inst);
}
