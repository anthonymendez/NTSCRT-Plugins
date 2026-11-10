// Port of upstream Tests/CrtCoreTests/DigitalVideoTests.swift (2b3013d), plus
// the same "jumped to = played" checks through DigitalStage and a fake
// FrameContext, thread-count determinism, missing history and seeds.
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <optional>
#include <vector>

#include "stages/digital/parallel.h"
#include "stages/digital/renderer.h"
#include "stages/stages.h"

using namespace ntscrt;
using namespace ntscrt::digital;

namespace {

using Pixels = std::vector<uint8_t>;
constexpr int kW = 256, kH = 192;
constexpr Size kCapture{128, 96};

// A textured background with a bright square moving right; `cut` switches to
// a different scene from that frame on.
Pixels frame(int n, std::optional<int> cut = std::nullopt) {
    Pixels px(size_t(kW) * kH * 4, 255);
    int scene = (cut && n >= *cut) ? 1 : 0;
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            size_t i = (size_t(y) * kW + x) * 4;
            int r, g, b;
            if (scene == 0) {
                r = (x * 255) / kW;
                g = ((x / 8 + y / 8) % 2) * 120 + 40;
                b = (y * 255) / kH;
            } else {
                r = 200 - (y * 150) / kH;
                g = ((x / 16) % 2) * 60 + 90;
                b = 60 + (x * 120) / kW;
            }
            int sx = 20 + 4 * n, sy = 60 + 2 * n;
            if (x >= sx && x < sx + 40 && y >= sy && y < sy + 40) {
                r = 250;
                g = 240;
                b = 30;
            }
            px[i] = uint8_t(b);
            px[i + 1] = uint8_t(g);
            px[i + 2] = uint8_t(r);
        }
    return px;
}

EarlierFrames visitor(std::optional<int> cut = std::nullopt) {
    return [cut](int i) -> std::optional<SourceFrame> {
        auto f = std::make_shared<Pixels>(frame(i, cut));
        return SourceFrame{f->data(), kW, kH, kW * 4, f};
    };
}

Settings settings(const char *format, std::vector<std::pair<const char *, double>> overrides = {}) {
    Settings s = format_by_id(format)->settings();
    for (auto &[k, v] : overrides) REQUIRE(s.set(k, v));
    return s;
}

Settings values(std::vector<std::pair<const char *, double>> v) {
    Settings s;
    for (auto &[k, x] : v) REQUIRE(s.set(k, x));
    return s;
}

Pixels render(Renderer &r, int n, const Settings &s, std::optional<int> cut = std::nullopt,
              const SettingsAt &history = nullptr, double fps = 30) {
    Pixels px = frame(n, cut);
    r.process(n, px.data(), kW, kH, kW * 4, kCapture, fps, s, history, visitor(cut));
    return px;
}

std::vector<Pixels> played(const Settings &s, int frames, std::optional<int> cut = std::nullopt,
                           const SettingsAt &history = nullptr) {
    Renderer r;
    std::vector<Pixels> out;
    for (int n = 0; n < frames; ++n) out.push_back(render(r, n, s, cut, history));
    return out;
}

double meanDifference(const Pixels &a, const Pixels &b) {
    long long s = 0;
    for (size_t i = 0; i < a.size(); i += 4)
        s += std::abs(int(a[i]) - int(b[i])) + std::abs(int(a[i + 1]) - int(b[i + 1])) +
             std::abs(int(a[i + 2]) - int(b[i + 2]));
    return double(s) / double(a.size() / 4 * 3);
}

void checkJumpsMatchPlaying(const Settings &s, const char *label, int frames = 40,
                            std::optional<int> cut = std::nullopt, const SettingsAt &history = nullptr) {
    INFO(label);
    auto straight = played(s, frames, cut, history);
    Renderer jumper;
    for (int n : {frames - 1, 3, 27, 12, frames - 2, 0, 33}) {
        if (n >= frames) continue;
        INFO("frame " << n << " jumped to");
        CHECK(render(jumper, n, s, cut, history) == straight[size_t(n)]);
    }
    // A fresh renderer per frame too (nothing cached at all).
    for (int n : {5, 19, 36}) {
        if (n >= frames) continue;
        INFO("frame " << n << " fresh");
        Renderer fresh;
        CHECK(render(fresh, n, s, cut, history) == straight[size_t(n)]);
    }
}

// ---- DigitalStage through a fake FrameContext ------------------------------------

json lookFor(const Settings &s) {
    json v = json::object();
    for (int i = 0; i < kParamCount; ++i) v[kParams[i].id] = s.v[size_t(i)];
    return json{{"digital", {{"enabled", true}, {"values", v}}},
                {"downscale", {{"enabled", true}, {"width", kCapture.width}, {"method", "area"}}}};
}

struct FakeHost {
    std::function<Settings(int64_t)> settingsAt;  // per-frame settings (keyframes)
    std::optional<int> cut;
    std::function<bool(int64_t)> available = [](int64_t) { return true; };
    std::map<int64_t, json> looks;
    uint64_t seed = 0;

    const json &lookAt(int64_t f) {
        auto it = looks.find(f);
        if (it == looks.end()) it = looks.emplace(f, lookFor(settingsAt(f))).first;
        return it->second;
    }
    Pixels render(DigitalStage &stage, int64_t n) {
        looks.clear();  // like the façade: history evaluated per render call
        ImageBGRA8 img;
        img.width = kW;
        img.height = kH;
        img.px = frame(int(n), cut);
        FrameContext ctx;
        ctx.frame = n;
        ctx.fps = 30;
        ctx.seed = seed;
        ctx.look = &lookAt(n);
        ctx.look_at = [this](int64_t f) -> const json & { return lookAt(f); };
        ctx.source_at = [this](int64_t f) -> std::shared_ptr<const ImageBGRA8> {
            if (!available(f)) return nullptr;
            auto im = std::make_shared<ImageBGRA8>();
            im->width = kW;
            im->height = kH;
            im->px = frame(int(f), cut);
            return im;
        };
        REQUIRE(stage.active(*ctx.look));
        stage.process(img, ctx);
        return img.px;
    }
    std::vector<Pixels> played(int frames) {
        DigitalStage st;
        std::vector<Pixels> out;
        for (int n = 0; n < frames; ++n) out.push_back(render(st, n));
        return out;
    }
};

void checkStageJumpsMatchPlaying(FakeHost &host, const char *label, int frames = 40) {
    INFO(label);
    auto straight = host.played(frames);
    DigitalStage jumper;
    for (int n : {frames - 1, 3, 27, 12, frames - 2, 0, 33}) {
        if (n >= frames) continue;
        INFO("stage frame " << n << " jumped to");
        CHECK(host.render(jumper, n) == straight[size_t(n)]);
    }
    for (int n : {5, 19, 36}) {
        if (n >= frames) continue;
        INFO("stage frame " << n << " fresh");
        DigitalStage fresh;
        CHECK(host.render(fresh, n) == straight[size_t(n)]);
    }
}

}  // namespace

// ---- transforms --------------------------------------------------------------------------

TEST_CASE("DCT round trip is identity") {
    float block[64], coef[64], back[64], tmp[64];
    for (int i = 0; i < 64; ++i) block[i] = float((i * 37) % 255) - 128;
    codec::forwardDCT(block, 8, coef, tmp, codec::basis());
    codec::inverseDCT(coef, back, 8, tmp, codec::basis());
    for (int i = 0; i < 64; ++i) CHECK(std::fabs(back[i] - block[i]) < 1e-3f);
    float flat[64];
    std::fill_n(flat, 64, 1.0f);
    codec::forwardDCT(flat, 8, coef, tmp, codec::basis());
    CHECK(std::fabs(coef[0] - 8) < 1e-4f);
    float mx = 0;
    for (int i = 1; i < 64; ++i) mx = std::max(mx, std::fabs(coef[i]));
    CHECK(mx < 1e-4f);
}

TEST_CASE("lower quality loses more of the picture") {
    Settings clean = values({{"frame_rate", 30}, {"sensor_noise", 0}, {"noise_reduction", 0}, {"chroma", 0},
                             {"keyframe_interval", 0}});
    std::vector<double> errors;
    for (double q : {0.95, 0.6, 0.3, 0.05}) {
        Settings s = clean;
        s[kQuality] = q;
        Renderer r;
        errors.push_back(meanDifference(render(r, 0, s), frame(0)));
    }
    CHECK(std::is_sorted(errors.begin(), errors.end()));
    CHECK(errors[0] < 6);
    CHECK(errors[3] > 3 * errors[0]);
}

// ---- timing -----------------------------------------------------------------------------------

TEST_CASE("capture timing holds frames and places keyframes") {
    Timing t(30, values({{"frame_rate", 15}, {"keyframe_interval", 2}}));
    std::vector<int> caps;
    for (int n = 0; n < 6; ++n) caps.push_back(t.captureOfFrame(n));
    CHECK(caps == std::vector<int>{0, 0, 1, 1, 2, 2});
    for (int c = 0; c < 50; ++c) CHECK(t.captureOfFrame(t.frameOfCapture(c)) == c);
    CHECK(t.keyframeEvery == 30);
    CHECK(t.kind(0, 1, 0) == FrameKind::Key);
    CHECK(t.kind(30, 1, 0) == FrameKind::DroppedKey);
    CHECK(t.kind(31, 1, 0) == FrameKind::Predicted);
    Timing fast(24, values({{"frame_rate", 60}, {"keyframe_interval", 0}}));
    CHECK(fast.captureFPS == 24);
    CHECK(fast.intraOnly());
    for (int n = 0; n < 4; ++n) CHECK(fast.captureOfFrame(n) == n);
}

TEST_CASE("lower frame rate repeats frames") {
    auto out = played(settings("webcam", {{"sensor_noise", 0}}), 4);  // 15 fps of 30
    CHECK(out[0] == out[1]);
    CHECK(out[1] != out[2]);
    CHECK(out[2] == out[3]);
}

// ---- the same frame however it's reached ------------------------------------------------------

TEST_CASE("every format looks the same jumped to as played") {
    for (auto &f : formats()) {
        double ki = 0;
        for (auto &[k, v] : f.values)
            if (std::string_view(k) == "keyframe_interval") ki = v;
        checkJumpsMatchPlaying(settings(f.id, {{"keyframe_interval", std::min(1.0, ki)}}), f.id, 40, 20);
    }
}

TEST_CASE("every format looks the same jumped to as played (DigitalStage, FrameContext)") {
    for (auto &f : formats()) {
        double ki = 0;
        for (auto &[k, v] : f.values)
            if (std::string_view(k) == "keyframe_interval") ki = v;
        Settings s = settings(f.id, {{"keyframe_interval", std::min(1.0, ki)}});
        FakeHost host;
        host.settingsAt = [s](int64_t) { return s; };
        host.cut = 20;
        checkStageJumpsMatchPlaying(host, f.id);
    }
}

TEST_CASE("data loss and datamosh look the same jumped to") {
    Settings s = values({{"frame_rate", 30}, {"keyframe_interval", 0.5}, {"quality", 0.4}, {"data_loss", 0.6},
                         {"datamosh", 0.6}, {"sensor_noise", 0.2}, {"noise_reduction", 0.3}, {"deblocking", 0.5},
                         {"fixed_bitrate", 0.5}});
    checkJumpsMatchPlaying(s, "loss + mosh", 40, 16);
}

static Settings keyedVideoCall(int n) {
    Settings s = settings("video_call", {{"keyframe_interval", 0.5}, {"frame_rate", 30}});
    s[kQuality] = 0.1 + 0.02 * double(n);
    s[kDataLoss] = n > 15 ? 0.5 : 0;
    return s;
}

TEST_CASE("keyframed settings look the same jumped to") {
    Settings base = settings("video_call", {{"keyframe_interval", 0.5}, {"frame_rate", 30}});
    SettingsAt history = [](int n) { return keyedVideoCall(n); };
    checkJumpsMatchPlaying(base, "keyframes", 40, std::nullopt, history);
}

TEST_CASE("keyframed settings look the same jumped to (DigitalStage, look_at)") {
    FakeHost host;
    host.settingsAt = [](int64_t n) { return keyedVideoCall(int(n)); };
    checkStageJumpsMatchPlaying(host, "stage keyframes");
    // Datamosh with keyframed quality (re-runs reach back past lost keys).
    FakeHost mosh;
    mosh.cut = 16;
    mosh.settingsAt = [](int64_t n) {
        Settings s = settings("datamosh", {{"keyframe_interval", 0.2}, {"datamosh", 0.7}});
        s[kQuality] = 0.2 + 0.01 * double(n);
        return s;
    };
    checkStageJumpsMatchPlaying(mosh, "stage mosh keyframes");
}

TEST_CASE("an edited keyframe elsewhere is picked up on re-render") {
    // Datamosh: frame 30 depends on everything since frame 0.
    auto make = [](double q) {
        return [q](int64_t n) {
            Settings s = settings("datamosh", {{"keyframe_interval", 0.2}});
            s[kQuality] = n < 10 ? q : 0.4;
            return s;
        };
    };
    FakeHost host;
    host.settingsAt = make(0.4);
    DigitalStage st;
    Pixels before;
    for (int n = 0; n <= 30; ++n) before = host.render(st, n);
    host.settingsAt = make(0.05);  // an earlier keyframe edited; frame 30's own settings unchanged
    Pixels again = host.render(st, 30);
    CHECK(again != before);
    DigitalStage fresh;
    CHECK(again == host.render(fresh, 30));
    // And a jump that restores a checkpoint made under the old history.
    host.settingsAt = make(0.4);
    for (int n = 0; n <= 30; ++n) host.render(st, n);
    host.settingsAt = make(0.05);
    Pixels jumped = host.render(st, 26);
    DigitalStage fresh2;
    CHECK(jumped == host.render(fresh2, 26));
}

TEST_CASE("datamosh checkpoints spare the re-run") {
    Settings s = settings("datamosh", {{"keyframe_interval", 0.2}});  // keyframes every 6 frames, all lost
    Renderer r;
    for (int n = 0; n < 40; ++n) render(r, n, s);
    render(r, 3, s);   // jump back...
    render(r, 38, s);  // ...and forward again
    CHECK(r.lastRunLength() <= 6);
}

// ---- errors behave like a codec's ----------------------------------------------------------

TEST_CASE("lost data smears until the next keyframe") {
    Settings clean = settings("early_web", {{"keyframe_interval", 0.5}, {"frame_rate", 30}, {"fixed_bitrate", 0}});
    Settings lossy = clean;
    lossy[kDataLoss] = 0.8;
    auto a = played(clean, 31), b = played(lossy, 31);
    CHECK(a[0] == b[0]);
    CHECK(a[15] == b[15]);
    CHECK(a[30] == b[30]);
    CHECK(meanDifference(a[14], b[14]) > 0.5);
    CHECK(meanDifference(a[14], b[14]) > meanDifference(a[2], b[2]));
}

TEST_CASE("DV dropouts last one frame") {
    Settings clean = settings("minidv", {{"sensor_noise", 0}});
    Settings lossy = clean;
    lossy[kDataLoss] = 0.8;
    auto a = played(clean, 10), b = played(lossy, 10);
    std::vector<int> damaged;
    for (int n = 1; n < 10; ++n)
        if (a[size_t(n)] != b[size_t(n)]) damaged.push_back(n);
    CHECK_FALSE(damaged.empty());
    for (int n : damaged) CHECK(meanDifference(a[size_t(n)], b[size_t(n)]) < 8);
}

TEST_CASE("datamosh drags the old scene across a cut") {
    Settings normal = settings("datamosh", {{"datamosh", 0}, {"keyframe_interval", 0.5}});
    Settings moshed = settings("datamosh", {{"datamosh", 1}, {"keyframe_interval", 0.5}});
    const int cut = 15;
    auto a = played(normal, 25, cut), b = played(moshed, 25, cut);
    CHECK(a[14] == b[14]);
    Pixels newScene = frame(20, cut), oldScene = frame(20);
    CHECK(meanDifference(a[20], newScene) < meanDifference(a[20], oldScene));
    CHECK(meanDifference(b[20], oldScene) < meanDifference(b[20], newScene));
}

// ---- fitting into the chain -------------------------------------------------------------------

TEST_CASE("nearest downscale gets the capture picture back") {
    struct C {
        int w, h, cw, ch;
    };
    for (C c : {C{256, 192, 128, 96}, C{1000, 750, 320, 240}, C{480, 360, 320, 240}, C{333, 250, 320, 240}}) {
        Pixels pic(size_t(c.cw) * c.ch * 4);
        for (size_t i = 0; i < pic.size(); ++i) pic[i] = uint8_t((i * 7919) % 251);
        Pixels big(size_t(c.w) * c.h * 4, 0);
        Renderer::upscale(pic, {c.cw, c.ch}, big.data(), c.w, c.h, c.w * 4);
        bool ok = true;
        for (int y = 0; y < c.ch && ok; ++y)
            for (int x = 0; x < c.cw && ok; ++x) {
                int sx = int((double(x) + 0.5) * double(c.w) / double(c.cw));
                int sy = int((double(y) + 0.5) * double(c.h) / double(c.ch));
                size_t s = (size_t(sy) * c.w + sx) * 4, d = (size_t(y) * c.cw + x) * 4;
                ok = std::equal(big.begin() + long(s), big.begin() + long(s) + 4, pic.begin() + long(d));
            }
        CHECK(ok);
    }
}

TEST_CASE("capture size follows the downscale rule") {
    CHECK(Renderer::captureSize(1920, 1080, 320).height == 180);
    CHECK(Renderer::captureSize(2048, 2048, 320).height == 320);
    CHECK(Renderer::captureSize(200, 100, 720).width == 200);
}

TEST_CASE("quarter-wide color smears sideways") {
    Geometry g{64, 64, Chroma::QuarterWide};
    Pixels px(64 * 64 * 4, 128);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
            if (x % 4 < 2) px[(size_t(y) * 64 + x) * 4 + 2] = 255;
    Settings s = values({{"sensor_noise", 0}, {"noise_reduction", 0}});
    auto pic = Renderer::capture(px, g, s, 0);
    CHECK(pic->cr.width == 16);
    float mn = 1e9f, mx = -1e9f;
    for (int i = 0; i < 16; ++i) {
        mn = std::min(mn, pic->cr.data[size_t(i)]);
        mx = std::max(mx, pic->cr.data[size_t(i)]);
    }
    CHECK(mx - mn < 0.01f);
    auto full = Renderer::capture(px, Geometry{64, 64, Chroma::Full}, s, 0);
    CHECK(full->cr.data[0] - full->cr.data[2] > 50);
}

TEST_CASE("formats are distinct and defaults are upstream's") {
    auto &all = formats();
    CHECK(all.size() == 5);
    for (size_t i = 0; i < all.size(); ++i)
        for (size_t j = i + 1; j < all.size(); ++j)
            CHECK_FALSE((all[i].settings() == all[j].settings() && all[i].width == all[j].width));
    Settings d;
    CHECK(d[kFrameRate] == 15);
    CHECK(d[kSensorNoise] == 0.4);
    CHECK(d[kNoiseReduction] == 0.5);
    CHECK(d[kChroma] == 2);
    CHECK(d[kQuality] == 0.22);
    CHECK(d.chroma() == Chroma::HalfBoth);
    int structure = 0;
    for (auto &p : kParams) structure += p.structure ? 1 : 0;
    CHECK(structure == 6);
}

// ---- the camera ---------------------------------------------------------------------------------

static PicturePtr camera(const Pixels &px, int w, int h, std::vector<std::pair<const char *, double>> v) {
    Settings s = values({{"sensor_noise", 0}, {"noise_reduction", 0}, {"chroma", 0}});
    for (auto &[k, x] : v) s.set(k, x);
    return Renderer::capture(px, Geometry{w, h, Chroma::Full}, s, 0);
}

TEST_CASE("picture controls are neutral by default") {
    Pixels px = frame(0);
    Pixels a0 = Renderer::scale(px.data(), kW, kH, kW * 4, kCapture, Settings());
    Pixels b0 = Renderer::scale(px.data(), kW, kH, kW * 4, kCapture,
                                values({{"aliasing", 0}, {"lens_blur", 0}, {"fields", 0}}));
    CHECK(a0 == b0);
    auto a = camera(a0, kCapture.width, kCapture.height, {});
    auto b = camera(a0, kCapture.width, kCapture.height,
                    {{"exposure", 0}, {"white_balance", 0}, {"tint", 0}, {"saturation", 1}, {"black_level", 0},
                     {"sharpening", 0}});
    CHECK(a->y.data[500] == b->y.data[500]);
    CHECK(a->cr.data[777] == b->cr.data[777]);
}

TEST_CASE("overexposure clips each color on its own") {
    Pixels px(16 * 16 * 4);
    const uint8_t skin[4] = {140, 160, 210, 255};
    for (size_t i = 0; i < px.size(); ++i) px[i] = skin[i % 4];
    auto pic = camera(px, 16, 16, {{"exposure", 2}});
    float y = pic->y.data[0], cb = pic->cb.data[0], cr = pic->cr.data[0];
    float r = y + 1.402f * (cr - 128), g = y - 0.344136f * (cb - 128) - 0.714136f * (cr - 128);
    float b = y + 1.772f * (cb - 128);
    CHECK(std::fabs(r - 255) < 0.5f);
    CHECK(std::fabs(g - 255) < 0.5f);
    CHECK(std::fabs(b - 255) < 0.5f);
    auto one = camera(px, 16, 16, {{"exposure", 0.5}});
    float r1 = one->y.data[0] + 1.402f * (one->cr.data[0] - 128);
    CHECK(std::fabs(r1 - 255) < 0.5f);
    CHECK(one->y.data[0] - 0.344136f * (one->cb.data[0] - 128) - 0.714136f * (one->cr.data[0] - 128) < 254);
}

TEST_CASE("black level lifts black and sharpening overshoots") {
    const int w = 32, h = 16;
    Pixels px(size_t(w) * h * 4, 255);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c) px[(size_t(y) * w + x) * 4 + c] = x < 16 ? 40 : 200;
    auto lifted = camera(px, w, h, {{"black_level", 0.1}});
    CHECK(std::fabs(lifted->y.data[0] - (25.5f + 40 * 0.9f)) < 0.6f);
    auto sharp = camera(px, w, h, {{"sharpening", 1}});
    auto row = [&](int x) { return sharp->y.data[size_t(8) * sharp->y.width + x]; };
    CHECK(row(15) < 35);
    CHECK(row(16) > 205);
    CHECK(std::fabs(row(2) - 40) < 0.6f);
}

TEST_CASE("aliasing turns fine stripes into false color") {
    const int w = 640, h = 64;
    Pixels px(size_t(w) * h * 4, 255);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t v = (x * 7 / 3) % 2 == 0 ? 230 : 25;
            for (int c = 0; c < 3; ++c) px[(size_t(y) * w + x) * 4 + c] = v;
        }
    auto chroma = [&](double aliasing) {
        Pixels small = Renderer::scale(px.data(), w, h, w * 4, {160, 16}, values({{"aliasing", aliasing}}));
        auto pic = camera(small, 160, 16, {});
        float s = 0;
        for (int i = 0; i < 160 * 16; ++i)
            s += std::fabs(pic->cb.data[size_t(i)] - 128) + std::fabs(pic->cr.data[size_t(i)] - 128);
        return s / float(160 * 16);
    };
    CHECK(chroma(0) < 2);
    CHECK(chroma(1) > 20);
}

TEST_CASE("one field doubles every other line") {
    Pixels px = frame(3);
    Pixels small = Renderer::scale(px.data(), kW, kH, kW * 4, kCapture, values({{"fields", 1}}));
    const size_t rb = size_t(kCapture.width) * 4;
    for (int y = 0; y + 1 < kCapture.height; y += 2)
        CHECK(std::equal(small.begin() + long(y * rb), small.begin() + long((y + 1) * rb),
                         small.begin() + long((y + 1) * rb)));
}

// ---- the Lanczos stand-in for vImage --------------------------------------------------------------

TEST_CASE("Lanczos scaler: flat stays flat, area-averages fine detail, identity at 1:1") {
    Pixels flat(64 * 48 * 4);
    for (size_t i = 0; i < flat.size(); ++i) flat[i] = uint8_t(i % 4 == 3 ? 255 : 77 + (i % 4));
    Pixels out(20 * 15 * 4);
    Renderer::scaleLanczos(flat.data(), 64, 48, 64 * 4, out.data(), 20, 15);
    for (size_t i = 0; i < out.size(); ++i) CHECK(int(out[i]) == int(flat[i % 4]));
    // 1-px checkerboard to a quarter: mid gray (the filter area-averages).
    Pixels cb(64 * 64 * 4);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
            for (int c = 0; c < 4; ++c) cb[(size_t(y) * 64 + x) * 4 + c] = ((x + y) % 2) ? 255 : 0;
    Pixels q(16 * 16 * 4);
    Renderer::scaleLanczos(cb.data(), 64, 64, 64 * 4, q.data(), 16, 16);
    for (int y = 2; y < 14; ++y)
        for (int x = 2; x < 14; ++x) CHECK(std::abs(int(q[(size_t(y) * 16 + x) * 4]) - 128) <= 3);
}

// ---- determinism ------------------------------------------------------------------------------------

TEST_CASE("results don't depend on the thread count") {
    Settings s = values({{"frame_rate", 30}, {"keyframe_interval", 0.5}, {"quality", 0.4}, {"data_loss", 0.6},
                         {"datamosh", 0.6}, {"sensor_noise", 0.5}, {"noise_reduction", 0.3}, {"deblocking", 0.5},
                         {"fixed_bitrate", 0.5}, {"lens_blur", 0.4}, {"aliasing", 0.3}, {"sharpening", 0.5}});
    std::vector<std::vector<Pixels>> runs;
    for (int t : {1, 3, 8}) {
        set_thread_count(t);
        CHECK(thread_count() == t);
        runs.push_back(played(s, 24, 12));
    }
    set_thread_count(0);
    CHECK(runs[0] == runs[1]);
    CHECK(runs[0] == runs[2]);
}

TEST_CASE("parallel_for covers every index once, nested calls run inline") {
    std::vector<int> hits(1000, 0);
    parallel_for(1000, [&](int i) {
        hits[size_t(i)] += 1;
        if (i == 0) parallel_for(10, [&](int) {});
    });
    CHECK(std::all_of(hits.begin(), hits.end(), [](int h) { return h == 1; }));
}

TEST_CASE("missing earlier frames degrade gracefully and deterministically") {
    Settings s = values({{"frame_rate", 30}, {"keyframe_interval", 0.5}, {"data_loss", 0.4}, {"datamosh", 1},
                         {"sensor_noise", 0}});
    FakeHost host;
    host.settingsAt = [s](int64_t) { return s; };
    host.available = [](int64_t f) { return f % 3 != 1; };  // some frames can't be had
    std::vector<Pixels> a, b;
    for (int n : {20, 7, 33}) {
        DigitalStage st1, st2;
        a.push_back(host.render(st1, n));
        b.push_back(host.render(st2, n));
    }
    CHECK(a == b);
    // No history at all (a host without fetch_source): no crash, stable.
    host.available = [](int64_t) { return false; };
    DigitalStage st;
    Pixels x = host.render(st, 12);
    DigitalStage st2;
    CHECK(x == host.render(st2, 12));
}

TEST_CASE("seed varies errors and noise; seed 0 is upstream's pattern") {
    Settings s = values({{"frame_rate", 30}, {"keyframe_interval", 0}, {"data_loss", 0.8}, {"sensor_noise", 0.5}});
    Pixels base = frame(5);
    auto run = [&](uint64_t seed) {
        Renderer r;
        Pixels px = base;
        r.process(5, px.data(), kW, kH, kW * 4, kCapture, 30, s, nullptr, visitor(), seed);
        return px;
    };
    Renderer plain;
    CHECK(render(plain, 5, s) == run(0));
    CHECK(run(0) != run(12345));
    CHECK(run(12345) == run(12345));
    CHECK(codec::lostBlocks(0.5, 3, Geometry{128, 96, Chroma::HalfBoth}, 0) ==
          codec::lostBlocks(0.5, 3, Geometry{128, 96, Chroma::HalfBoth}, seed_mix(0)));
}

// ---- cost -------------------------------------------------------------------------------------------

TEST_CASE("codec is fast enough for playback") {
    auto perFrame = [](const char *format, int width, int height) {
        Renderer r;
        Settings s = settings(format, {{"frame_rate", 30}});
        Pixels px(size_t(1280) * 720 * 4);
        for (size_t i = 0; i < px.size(); ++i) px[i] = uint8_t((i * 31 + i / 5000) & 255);
        auto t0 = std::chrono::steady_clock::now();
        for (int n = 0; n < 20; ++n) {
            Pixels p = px;
            r.process(n, p.data(), 1280, 720, 1280 * 4, {width, height}, 30, s);
        }
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 20;
    };
    double call = perFrame("video_call", 320, 180);
    double dv = perFrame("minidv", 720, 404);
    std::printf("DIGITAL-PERF video call 320: %.1f ms/frame, MiniDV 720: %.1f ms/frame (from 1280x720, %d threads)\n",
                call, dv, thread_count());
    // Upstream asserts < 30 / < 40 ms on an M-series Mac; generous here
    // (shared CI / build machines).
    CHECK(call < 120);
    CHECK(dv < 160);
}
