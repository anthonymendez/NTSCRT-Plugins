// Alpha handling at the host boundary (pixel_convert): Clip / Spill modes,
// premultiplied sources, transparent-area fill.
#include <doctest/doctest.h>

#include <vector>

#include "pixel_convert.h"

using namespace ntscrt;

namespace {
// W x 1 RGBA32F image: left half opaque red, right half fully transparent
// (black in a premultiplied source, garbage green in a straight one).
struct Strip {
    int w;
    std::vector<float> px;
    explicit Strip(int w_, bool premult) : w(w_), px(size_t(w_) * 4) {
        for (int x = 0; x < w; ++x) {
            float *p = &px[size_t(x) * 4];
            if (x < w / 2) { p[0] = 1; p[1] = 0; p[2] = 0; p[3] = 1; }
            else { p[0] = 0; p[1] = premult ? 0.f : 1.f; p[2] = 0; p[3] = 0; }
        }
    }
    ntscrt_image image() { return {w, 1, ptrdiff_t(w) * 16, NTSCRT_RGBA32F, px.data()}; }
};
}  // namespace

TEST_CASE("alpha mode comes from look.alpha.mode, default clip") {
    CHECK(alpha_mode(json::object()) == AlphaMode::Clip);
    CHECK(alpha_mode(json::parse(R"({"alpha":{"mode":"clip"}})")) == AlphaMode::Clip);
    CHECK(alpha_mode(json::parse(R"({"alpha":{"mode":"spill"}})")) == AlphaMode::Spill);
}

TEST_CASE("clip: transparent areas take the edge colour, not black") {
    for (bool premult : {true, false}) {
        CAPTURE(premult);
        Strip s(16, premult);
        ImageBGRA8 im;
        std::vector<float> a;
        to_bgra8(s.image(), im, &a, AlphaMode::Clip, premult);
        for (int x = 0; x < 16; ++x) {
            CAPTURE(x);
            CHECK(im.row(0)[x * 4 + 2] == 255);  // R
            CHECK(im.row(0)[x * 4 + 1] == 0);    // G
            CHECK(a[size_t(x)] == (x < 8 ? 1.f : 0.f));
        }
    }
}

TEST_CASE("clip: premultiplied soft edge is unpremultiplied") {
    std::vector<float> px = {0.25f, 0.125f, 0.f, 0.5f};
    ntscrt_image src{1, 1, 16, NTSCRT_RGBA32F, px.data()};
    ImageBGRA8 im;
    to_bgra8(src, im, nullptr, AlphaMode::Clip, true);
    CHECK(im.row(0)[2] == 128);  // 0.5
    CHECK(im.row(0)[1] == 64);   // 0.25
}

TEST_CASE("spill: the element over black") {
    Strip s(4, false);
    ImageBGRA8 im;
    to_bgra8(s.image(), im, nullptr, AlphaMode::Spill, false);
    CHECK(im.row(0)[2] == 255);
    CHECK(im.row(0)[3 * 4 + 1] == 0);  // straight garbage green * alpha 0
}

TEST_CASE("merge_alpha") {
    float c[4] = {0.5f, 0.2f, 0.f, 1.f};
    merge_alpha(c, 0.5f, AlphaMode::Clip, true);
    CHECK(c[0] == doctest::Approx(0.25f));
    CHECK(c[3] == doctest::Approx(0.5f));

    float g[4] = {0.4f, 0.2f, 0.f, 1.f};  // glow over a transparent pixel
    merge_alpha(g, 0.f, AlphaMode::Spill, true);
    CHECK(g[3] == doctest::Approx(0.4f));
    CHECK(g[0] == doctest::Approx(0.4f));
    float h[4] = {0.4f, 0.2f, 0.f, 1.f};
    merge_alpha(h, 0.f, AlphaMode::Spill, false);
    CHECK(h[0] == doctest::Approx(1.f));
    CHECK(h[1] == doctest::Approx(0.5f));

    float k[4] = {0.f, 0.f, 0.f, 1.f};  // black text stays opaque
    merge_alpha(k, 1.f, AlphaMode::Spill, true);
    CHECK(k[3] == doctest::Approx(1.f));
}

TEST_CASE("opaque frames are unchanged by the mode") {
    std::vector<float> px = {0.2f, 0.4f, 0.6f, 1.f};
    ntscrt_image src{1, 1, 16, NTSCRT_RGBA32F, px.data()};
    ImageBGRA8 a, b;
    to_bgra8(src, a, nullptr, AlphaMode::Clip, true);
    to_bgra8(src, b, nullptr, AlphaMode::Spill, false);
    CHECK(a.px == b.px);
}
