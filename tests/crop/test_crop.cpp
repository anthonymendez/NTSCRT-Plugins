// Crop geometry: port of upstream Tests/CrtCoreTests/SourceCropTests.swift
// (rectangle tests verbatim; the GPU/video pixel tests become CPU Crop::apply
// tests on the same four-quadrant picture).
#include <doctest/doctest.h>

#include <algorithm>
#include <vector>

#include "stages/crop/crop_internal.h"
#include "stages/stages.h"

using namespace ntscrt;
using crop::Cut;
using crop::Ratio;
using crop::SourceCrop;

namespace {
SourceCrop C(Ratio r, double x = 0.5, double y = 0.5, double scale = 1) { return SourceCrop{r, x, y, scale}; }
SourceCrop P(Ratio r, double position) { return SourceCrop{r, position, position, 1}; }
const Ratio square{1, 1};

enum class Hue { Red, Blue, Green, Yellow, Other };

// Red top left, blue top right, green bottom left, yellow bottom right.
ImageBGRA8 quadrants(int w, int h) {
    ImageBGRA8 im;
    im.resize(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t r, g, b;
            bool left = x < w / 2, top = y < h / 2;
            if (left && top) { r = 230; g = 20; b = 20; }
            else if (!left && top) { r = 20; g = 20; b = 230; }
            else if (left) { r = 20; g = 200; b = 20; }
            else { r = 230; g = 220; b = 20; }
            uint8_t *p = im.row(y) + x * 4;
            p[0] = b; p[1] = g; p[2] = r; p[3] = 255;
        }
    return im;
}

Hue hue(const ImageBGRA8 &im, double fx, double fy) {
    int x = std::min(im.width - 1, int(fx * im.width));
    int y = std::min(im.height - 1, int(fy * im.height));
    const uint8_t *p = im.row(y) + x * 4;
    double r = p[2] / 255.0, g = p[1] / 255.0, b = p[0] / 255.0;
    if (r > 0.6 && g > 0.6 && b < 0.35) return Hue::Yellow;
    if (r > 0.6 && g < 0.35 && b < 0.35) return Hue::Red;
    if (b > 0.6 && r < 0.35 && g < 0.35) return Hue::Blue;
    if (g > 0.5 && r < 0.35 && b < 0.35) return Hue::Green;
    return Hue::Other;
}

json crop_look(const std::string &ratio, double x, double y, double scale = 1, bool enabled = true) {
    return json{{"crop", {{"enabled", enabled}, {"ratio", ratio}, {"x", x}, {"y", y}, {"scale", scale}}}};
}
}  // namespace

TEST_CASE("the crop keeps all it can and cuts one axis") {
    auto tall = C({9, 16}).rect(1920, 1080);
    CHECK(tall.height == 1080);
    CHECK(tall.y == 0);
    CHECK(tall.width == 608);  // 607.5, to the even number
    CHECK(tall.x == (1920 - 608) / 2);
    auto wide = C({16, 9}).rect(1080, 1920);
    CHECK(wide.width == 1080);
    CHECK(wide.height == 608);
    CHECK(wide.y == (1920 - 608) / 2);
    auto sq = C(square).rect(640, 480);
    CHECK(sq.width == 480);
    CHECK(sq.height == 480);
    CHECK(sq.x == 80);
}

TEST_CASE("position slides the crop along the axis it cuts") {
    CHECK(P(square, 0).rect(640, 480).x == 0);
    CHECK(P(square, 1).rect(640, 480).x == 160);
    auto top = P({2, 1}, 0).rect(640, 480);
    auto bottom = P({2, 1}, 1).rect(640, 480);
    CHECK(top.height == 320);
    CHECK(top.y == 0);
    CHECK(bottom.y == 160);
    CHECK(P(square, 7).rect(640, 480).x == 160);
    CHECK(P(square, -1).rect(640, 480).x == 0);
}

TEST_CASE("a picture already that shape is left whole") {
    auto c = C({16, 9});
    CHECK(c.cut(1920, 1080) == Cut::None);
    CHECK(c.cut(1918, 1080) == Cut::None);  // within half a percent: no sliver cut
    auto r = c.rect(1918, 1080);
    CHECK(r.width == 1918);
    CHECK(r.height == 1080);
    CHECK(c.cut(1440, 1080) == Cut::Height);
    CHECK(C({1, 2}).cut(1080, 1080) == Cut::Width);
}

TEST_CASE("zooming in shrinks the crop and frees both ways") {
    auto middle = C(square, 0.5, 0.5, 2).rect(640, 360);
    CHECK(middle.width == 180);
    CHECK(middle.height == 180);
    CHECK(middle.x == (640 - 180) / 2);
    CHECK(middle.y == (360 - 180) / 2);
    auto corner = C(square, 1, 1, 2).rect(640, 360);
    CHECK(corner.x == 460);
    CHECK(corner.y == 180);
    auto own = C({16, 9});
    CHECK_FALSE(own.crops(640, 360));
    auto zoomed = own;
    zoomed.scale = 1.25;
    CHECK(zoomed.crops(640, 360));
    CHECK(zoomed.rect(640, 360).width == 512);
    CHECK(C(square, 0.5, 0.5, 0.3).rect(640, 360).width == 360);
    CHECK(C(square, 0.5, 0.5, 100).rect(640, 360).width == 46);
}

TEST_CASE("a pixel rectangle maps back to the crop") {
    auto c = C(square);
    auto placed = c.placed(100, 60, 180, 640, 360);
    CHECK(placed.scale == doctest::Approx(2).epsilon(1e-9));
    auto r = placed.rect(640, 360);
    CHECK(r.x == 100);
    CHECK(r.y == 60);
    CHECK(r.width == 180);
    auto past = c.placed(600, -40, 180, 640, 360).rect(640, 360);
    CHECK(past.x == 460);
    CHECK(past.y == 0);
    CHECK(c.placed(0, 0, 5000, 640, 360).scale == 1);
}

TEST_CASE("ratio labels round-trip") {
    std::vector<Ratio> all{{1, 1}, {3, 4}, {2, 3}, {9, 16}, {1, 2}, {4, 3}, {3, 2}, {16, 9}, {2, 1}};
    for (auto &r : all) {
        auto back = crop::parse_ratio(r.label());
        REQUIRE(back);
        CHECK(*back == r);
    }
    CHECK_FALSE(crop::parse_ratio("nope"));
    CHECK_FALSE(crop::parse_ratio("0:4"));
    CHECK(crop::parse_ratio(" 4 : 3 "));
}

TEST_CASE("look keys: upstream form, legacy position, enabled flag") {
    CHECK_FALSE(crop::from_look(json::object()));
    CHECK_FALSE(crop::from_look(json{{"crop", {{"ratio", "nope"}}}}));
    // Upstream: a crop object with a valid ratio means on.
    auto up = crop::from_look(json{{"crop", {{"ratio", "1:1"}, {"x", 0}, {"y", 0.25}, {"scale", 1}}}});
    REQUIRE(up);
    CHECK(up->x == 0);
    CHECK(up->y == 0.25);
    // Legacy single position.
    auto legacy = crop::from_look(json{{"crop", {{"ratio", "2:1"}, {"position", 1.0}}}});
    REQUIRE(legacy);
    CHECK(legacy->x == 1);
    CHECK(legacy->y == 1);
    // Clamped like upstream loadLook.
    auto clamped = crop::from_look(json{{"crop", {{"ratio", "1:1"}, {"x", 3}, {"y", -2}, {"scale", 50}}}});
    REQUIRE(clamped);
    CHECK(clamped->x == 1);
    CHECK(clamped->y == 0);
    CHECK(clamped->scale == crop::kMaxScale);
    // Host form with an explicit flag.
    CHECK_FALSE(crop::from_look(crop_look("1:1", 0.5, 0.5, 1, false)));
    CHECK(crop::from_look(crop_look("1:1", 0.5, 0.5, 1, true)));
}

TEST_CASE("a still is cropped (CPU)") {
    ImageBGRA8 still = quadrants(640, 360), out;
    // Square from the right: x 280..640 — blue over yellow, past a sliver of the left half.
    Crop::apply(crop_look("1:1", 1, 1), still, out);
    CHECK(out.width == 360);
    CHECK(out.height == 360);
    CHECK(hue(out, 0.6, 0.25) == Hue::Blue);
    CHECK(hue(out, 0.6, 0.75) == Hue::Yellow);
    CHECK(hue(out, 0.05, 0.25) == Hue::Red);
    // A portrait picture cut to landscape at the top: red and blue only.
    ImageBGRA8 tall = quadrants(360, 640), top;
    Crop::apply(crop_look("16:9", 0, 0), tall, top);
    CHECK(top.width == 360);
    CHECK(top.height == 202);
    CHECK(hue(top, 0.25, 0.9) == Hue::Red);
    CHECK(hue(top, 0.75, 0.9) == Hue::Blue);
    // Already the shape: the same pixels back.
    ImageBGRA8 same;
    Crop::apply(crop_look("16:9", 0.5, 0.5), still, same);
    CHECK(same.width == 640);
    CHECK(same.px == still.px);
    // Zoomed in on its own shape, bottom-right corner: yellow.
    ImageBGRA8 zoom;
    Crop::apply(crop_look("16:9", 1, 1, 2), still, zoom);
    CHECK(zoom.width == 320);
    CHECK(zoom.height == 180);
    CHECK(hue(zoom, 0.5, 0.5) == Hue::Yellow);
    // 1:2 from the top-left: red over green.
    ImageBGRA8 strip;
    Crop::apply(json{{"crop", {{"ratio", "1:2"}, {"position", 0.0}}}}, quadrants(640, 360), strip);
    CHECK(strip.width == 180);
    CHECK(strip.height == 360);
    CHECK(hue(strip, 0.5, 0.25) == Hue::Red);
    CHECK(hue(strip, 0.5, 0.75) == Hue::Green);
    // Off: unchanged.
    ImageBGRA8 off;
    Crop::apply(crop_look("1:1", 1, 1, 1, false), still, off);
    CHECK(off.px == still.px);
    // In place.
    ImageBGRA8 inplace = quadrants(640, 360);
    Crop::apply(crop_look("1:1", 0, 0), inplace, inplace);
    CHECK(inplace.width == 360);
    CHECK(hue(inplace, 0.25, 0.25) == Hue::Red);
}
