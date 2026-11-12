// Port of upstream Tests/CrtCoreTests/DigitalVideoTests.swift and Video1Tests.swift
// (v0.20.0, c6ab639), plus
// the same "jumped to = played" checks through DigitalStage and a fake
// FrameContext, thread-count determinism, missing history and seeds.
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <optional>
#include <set>
#include <vector>

#include "stages/digital/fields.h"
#include "stages/digital/migrate.h"
#include "stages/digital/parallel.h"
#include "stages/digital/renderer.h"
#include "stages/digital/video1.h"
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
                             {"keyframe_interval", 0}, {"fields", 0}});
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

TEST_CASE("quality damage is evenly spaced along the slider") {
    // No cliff at the bottom, no dead zone at the top.
    Settings clean = values({{"frame_rate", 30}, {"sensor_noise", 0}, {"noise_reduction", 0}, {"chroma", 0},
                             {"keyframe_interval", 0}});
    std::vector<double> errors;
    for (double q : {0.0, 0.1, 0.25, 0.5, 0.75, 1.0}) {
        Settings s = clean;
        s[kQuality] = q;
        Renderer r;
        errors.push_back(meanDifference(render(r, 0, s), frame(0)));
    }
    INFO(errors[0] << " " << errors[1] << " " << errors[2] << " " << errors[3] << " " << errors[4] << " " << errors[5]);
    const double range = errors.front() - errors.back();
    for (size_t i = 1; i < errors.size(); ++i) CHECK(errors[i - 1] > errors[i]);
    CHECK(errors[0] - errors[1] < 0.2 * range);
    CHECK(errors[3] - errors[5] > 0.25 * range);
}

TEST_CASE("old quality values keep their quantizer") {
    for (int i = 0; i <= 20; ++i) {
        double old = 0.05 * i;
        double q = 2 + 93 * old;
        float oldScale = float(q < 50 ? 5000 / q : 200 - 2 * q) / 100;
        double now = codec::qualityFromJPEGScale(old);
        INFO(old);
        CHECK(std::fabs(codec::tableScale(now) - oldScale) <= oldScale * 1e-4f);
        CHECK(std::fabs(migrate::quality_from_jpeg_scale(old) - now) < 1e-12);
    }
    json v1 = migrate::migrated(json{{"quality", 0.22}}, 1);
    CHECK(std::fabs(v1["quality"].get<double>() - 0.749) < 0.001);
    CHECK(migrate::migrated(json{{"quality", 0.22}}, 2)["quality"] == 0.22);  // today's looks are left alone
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
    CHECK(all.size() == 6);
    for (size_t i = 0; i < all.size(); ++i)
        for (size_t j = i + 1; j < all.size(); ++j)
            CHECK_FALSE((all[i].settings() == all[j].settings() && all[i].width == all[j].width));
    Settings d;
    CHECK(d[kFrameRate] == 15);
    CHECK(d[kSensorNoise] == 0.4);
    CHECK(d[kNoiseReduction] == 0.5);
    CHECK(d[kChroma] == 2);
    CHECK(d[kQuality] == 0.749);
    CHECK(d[kFields] == 1);  // One field by default (0.20)
    CHECK(d[kCodec] == 0);
    CHECK(d[kCCDResponse] == 0);
    CHECK(d[kLowLightNoise] == 0);     // 0.21: neutral, so older looks are unchanged
    CHECK(d[kSharpeningWidth] == 1);   // 0.21: 1 px = the sharpening as before
    CHECK(d.chroma() == Chroma::HalfBoth);
    CHECK_FALSE(d.set("exposure", 1));  // gone (migrated to CCD response)
    CHECK_FALSE(d.set("lens_blur", 1));
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
    // (Fields default to One field, the stage's look, not neutral: Progressive here.)
    Pixels a0 = Renderer::scale(px.data(), kW, kH, kW * 4, kCapture, values({{"fields", 0}}));
    Pixels b0 = Renderer::scale(px.data(), kW, kH, kW * 4, kCapture, values({{"aliasing", 0}, {"fields", 0}}));
    CHECK(a0 == b0);
    auto a = camera(a0, kCapture.width, kCapture.height, {});
    auto b = camera(a0, kCapture.width, kCapture.height,
                    {{"ccd_response", 0}, {"white_balance", 0}, {"tint", 0}, {"saturation", 1}, {"sharpening", 0}});
    CHECK(a->y.data[500] == b->y.data[500]);
    CHECK(a->cr.data[777] == b->cr.data[777]);
    // And neutral really is the picture as it came in.
    const int i = 500;
    float b_ = float(a0[size_t(i) * 4]), g_ = float(a0[size_t(i) * 4 + 1]), r_ = float(a0[size_t(i) * 4 + 2]);
    CHECK(std::fabs(a->y.data[size_t(i / kCapture.width) * a->y.width + i % kCapture.width] -
                    (0.299f * r_ + 0.587f * g_ + 0.114f * b_)) < 0.01f);
}

// RGB back from a picture's pixel k (full color resolution).
static std::array<float, 3> rgbOf(const Picture &p, size_t k = 0) {
    float y = p.y.data[k], cb = p.cb.data[k] - 128, cr = p.cr.data[k] - 128;
    return {y + 1.402f * cr, y - 0.344136f * cb - 0.714136f * cr, y + 1.772f * cb};
}

static Pixels patch(uint8_t r, uint8_t g, uint8_t b, int n = 32) {
    Pixels px;
    for (int i = 0; i < n * n; ++i) px.insert(px.end(), {b, g, r, uint8_t(255)});
    return px;
}

TEST_CASE("CCD response clips warm highlights red first") {
    // A warm wall in sun, in the corner of a darker frame the camera exposes for.
    Pixels px = patch(70, 70, 70);
    for (int y = 0; y < 6; ++y)
        for (int x = 0; x < 6; ++x) {
            size_t i = (size_t(y) * 32 + x) * 4;
            px[i] = 120;
            px[i + 1] = 170;
            px[i + 2] = 215;
        }
    auto out = rgbOf(*camera(px, 32, 32, {{"ccd_response", 1}}));
    INFO(out[0] << " " << out[1] << " " << out[2]);
    CHECK(std::fabs(out[0] - 255) < 1.5f);  // red clipped
    CHECK(out[2] < 225);                    // blue under the clip
    CHECK(out[1] > 200);
}

TEST_CASE("CCD response lifts blacks and exposes dark scenes up") {
    Pixels px = patch(0, 0, 0, 32);
    for (size_t i = 0; i < px.size() / 2; i += 4) px[i] = px[i + 1] = px[i + 2] = 60;
    auto dark = camera(px, 32, 32, {{"ccd_response", 1}});
    float blackY = dark->y.data[size_t(31) * dark->y.width + 5];
    CHECK(blackY > 15);  // black lifted
    CHECK(blackY < 45);  // but not washed out
    CHECK(dark->y.data[size_t(2) * dark->y.width + 5] > 90);  // 60 gray exposed up
    auto half = camera(px, 32, 32, {{"ccd_response", 0.5}});
    CHECK(half->y.data[size_t(31) * half->y.width + 5] < blackY);  // strength scales it
    auto white = rgbOf(*camera(patch(250, 250, 250), 32, 32, {{"ccd_response", 1}}));
    CHECK(std::max({white[0], white[1], white[2]}) <= 255.6f);
}

TEST_CASE("sharpening overshoots") {
    const int w = 32, h = 16;
    Pixels px(size_t(w) * h * 4, 255);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c) px[(size_t(y) * w + x) * 4 + c] = x < 16 ? 40 : 200;
    auto sharp = camera(px, w, h, {{"sharpening", 1}});
    auto row = [&](int x) { return sharp->y.data[size_t(8) * sharp->y.width + x]; };
    CHECK(row(15) < 35);
    CHECK(row(16) > 205);
    CHECK(std::fabs(row(2) - 40) < 0.6f);
}

// Upstream DigitalVideoTests.testHaloWidth (0.21).
TEST_CASE("halo width") {
    const int w = 32, h = 16;
    Pixels px(size_t(w) * h * 4, 255);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c) px[(size_t(y) * w + x) * 4 + c] = x < 16 ? 40 : 200;
    auto row = [&](const PicturePtr &p) {
        std::vector<float> r(static_cast<size_t>(w));
        for (int x = 0; x < w; ++x) r[size_t(x)] = p->y.data[size_t(8) * p->y.width + x];
        return r;
    };
    const auto tight = row(camera(px, w, h, {{"sharpening", 1}}));
    CHECK(row(camera(px, w, h, {{"sharpening", 1}, {"sharpening_width", 1}})) == tight);  // 1 px is the sharpening as it always was
    CHECK(std::fabs(tight[13] - 40) < 0.6f);  // a 1 px halo doesn't reach 3 px out
    const auto wide = row(camera(px, w, h, {{"sharpening", 1}, {"sharpening_width", 3}}));
    CHECK(wide[13] < 30);  // a 3 px halo does
    CHECK(wide[18] > 210);
    CHECK(std::fabs(wide[2] - 40) < 0.6f);  // flat areas untouched
    const auto between = row(camera(px, w, h, {{"sharpening", 1}, {"sharpening_width", 2.5}}));
    CHECK(between[13] < 40 - 0.6f);
    CHECK(between[13] > wide[13]);  // fractional widths fall between
}

// Upstream DigitalVideoTests.testLowLightNoiseGrowsInTheDark (0.21).
TEST_CASE("low-light noise grows in the dark") {
    const int w = 64, h = 32;
    auto flat = [&](uint8_t v) {
        Pixels px(size_t(w) * h * 4, 255);
        for (int i = 0; i < w * h; ++i)
            for (int c = 0; c < 3; ++c) px[size_t(i) * 4 + c] = v;
        return px;
    };
    auto lumas = [&](const PicturePtr &p) {
        std::vector<std::vector<float>> ys(static_cast<size_t>(h), std::vector<float>(static_cast<size_t>(w)));
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) ys[size_t(y)][size_t(x)] = p->y.data[size_t(y) * p->y.width + x];
        return ys;
    };
    auto spread = [&](const PicturePtr &p) {
        auto ys = lumas(p);
        float sum = 0;
        for (auto &r : ys)
            for (float v : r) sum += v;
        const float mean = sum / float(w * h);
        float sq = 0;
        for (auto &r : ys)
            for (float v : r) sq += (v - mean) * (v - mean);
        return std::sqrt(sq / float(w * h));
    };
    // The Y plane, coded size (padded), for exact comparisons.
    auto yplane = [](const PicturePtr &p) { return p->y.data; };
    const Pixels night = flat(25), day = flat(180);
    const float plain = spread(camera(night, w, h, {{"sensor_noise", 0.1}}));
    const float gained = spread(camera(night, w, h, {{"sensor_noise", 0.1}, {"low_light_noise", 1}}));
    INFO("a dark scene gets the gain's noise: " << plain << " -> " << gained);
    CHECK(gained > plain * 3);
    {
        // Grain, not streaks: nearby rows' noise is unrelated at any shift.
        auto ys = lumas(camera(night, w, h, {{"sensor_noise", 0.1}, {"low_light_noise", 1}}));
        float sum = 0;
        for (auto &r : ys)
            for (float v : r) sum += v;
        const float mean = sum / float(w * h);
        float worst = 0;
        for (int k = 1; k <= 8; ++k)
            for (int d = -3; d <= 3; ++d) {
                float sab = 0, saa = 0, sbb = 0;
                for (int y = 0; y < h - k; ++y)
                    for (int x = 3; x < w - 3; ++x) {
                        const float a = ys[size_t(y)][size_t(x)] - mean, b = ys[size_t(y + k)][size_t(x + d)] - mean;
                        sab += a * b;
                        saa += a * a;
                        sbb += b * b;
                    }
                worst = std::max(worst, std::fabs(sab / std::sqrt(saa * sbb)));
            }
        INFO("rows repeat nearby rows, shifted: " << worst);
        CHECK(worst < 0.2f);
    }
    // A flash-lit night shot: a bright subject filling the middle of a black
    // frame still reads as night (the exposure meter wouldn't).
    Pixels lit = flat(20);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (std::abs(x - w / 2) < 14)
                for (int c = 0; c < 3; ++c) lit[(size_t(y) * w + x) * 4 + c] = 235;
    CHECK(Renderer::meteredMean(lit, w, h, 1, 1, 1) > Renderer::gainKnee());  // the scenario: meters as bright
    CHECK(Renderer::sensorGain(Renderer::medianLuma(lit, w, h)) > 4);
    const float half = spread(camera(night, w, h, {{"sensor_noise", 0.1}, {"low_light_noise", 0.5}}));
    CHECK((half > plain && half < gained));
    CHECK(yplane(camera(day, w, h, {{"sensor_noise", 0.1}})) ==
          yplane(camera(day, w, h, {{"sensor_noise", 0.1}, {"low_light_noise", 1}})));  // daylight needs no gain
    CHECK(yplane(camera(night, w, h, {{"low_light_noise", 1}})) == yplane(camera(night, w, h, {})));  // nothing without sensor noise
}

// Plug-in check of the 0.21 noise-seed change: rows no longer replay the row
// above (upstream's SplitMix row seeds, unhashed, were row 0's stream advanced).
TEST_CASE("sensor noise rows are independent (hashed row seeds)") {
    const int w = 64, h = 32;
    Pixels px(size_t(w) * h * 4, 255);
    for (size_t i = 0; i < px.size(); i += 4) px[i] = px[i + 1] = px[i + 2] = 128;
    auto p = camera(px, w, h, {{"sensor_noise", 1}});
    // Unhashed (capture 0, seed 0), row y's seed was y steps of SplitMix from 0 and
    // a pixel takes six draws, so row 6 was row 0 one pixel along (exactly, on a
    // flat picture). No row now repeats a nearby row at a small shift.
    int repeats = 0;
    for (int k = 1; k <= 8; ++k)
        for (int y = k; y < h; ++y)
            for (int d = -3; d <= 3; ++d) {
                int same = 0;
                for (int x = 3; x < w - 3; ++x)
                    same += p->y.data[size_t(y) * p->y.width + x] == p->y.data[size_t(y - k) * p->y.width + x + d] ? 1 : 0;
                repeats += same > (w - 6) / 2 ? 1 : 0;
            }
    CHECK(repeats == 0);
}

// Fine gray stripes, finer than a 160-wide camera can hold.
static Pixels stripes(double period = 7.0 / 3, int w = 640, int h = 64) {
    Pixels px(size_t(w) * h * 4, 255);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t v = uint8_t(128 + 100 * std::cos(2 * 3.14159265358979323846 * double(x) / period));
            for (int c = 0; c < 3; ++c) px[(size_t(y) * w + x) * 4 + c] = v;
        }
    return px;
}

static Pixels sampled(const Pixels &px, double aliasing, int w = 640, int h = 64) {
    return Renderer::scale(px.data(), w, h, w * 4, {160, 16}, values({{"aliasing", aliasing}}));
}

TEST_CASE("aliasing turns fine stripes into false color") {
    const Pixels px = stripes();
    auto chroma = [&](double aliasing) {
        auto pic = camera(sampled(px, aliasing), 160, 16, {});
        float s = 0;
        for (int i = 0; i < 160 * 16; ++i)
            s += std::fabs(pic->cb.data[size_t(i)] - 128) + std::fabs(pic->cr.data[size_t(i)] - 128);
        return s / float(160 * 16);
    };
    CHECK(chroma(0) < 2);                   // filtered properly: gray stays gray
    CHECK(chroma(0.5) > 20);                // aliased: false color
    CHECK(chroma(1) > chroma(0.5) * 1.2f);  // above 50% the boost makes it stronger
}

TEST_CASE("aliasing rainbows survive quarter-width color") {
    // Stripes just off the sensor's own pitch beat slowly: the false color
    // has to turn through every hue and survive DV's 4:1:1 averaging.
    Pixels small = sampled(stripes(4.0 * 1.04), 1);
    auto pic = Renderer::capture(small, Geometry{160, 16, Chroma::QuarterWide},
                                 values({{"sensor_noise", 0}, {"noise_reduction", 0}}), 0);
    std::set<int> quadrants;
    float strength = 0;
    for (int x = 0; x < pic->cb.width - 2; ++x) {
        float cb = pic->cb.data[size_t(4) * pic->cb.width + x] - 128;
        float cr = pic->cr.data[size_t(4) * pic->cr.width + x] - 128;
        strength += std::fabs(cb) + std::fabs(cr);
        if (std::fabs(cb) + std::fabs(cr) > 8) quadrants.insert((cb >= 0 ? 1 : 0) + (cr >= 0 ? 2 : 0));
    }
    CHECK(quadrants.size() == 4);  // hue goes all the way round
    CHECK(strength / float(pic->cb.width - 2) > 15);
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
                         {"fixed_bitrate", 0.5}, {"ccd_response", 0.6}, {"aliasing", 0.7}, {"sharpening", 0.5}});
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

// ---- Microsoft Video 1 (port of upstream Video1Tests.swift) ------------------------------------------

namespace {
using Block = std::array<video1::RGB, 16>;
template <class F>
Block block(F f) {
    Block b{};
    for (int i = 0; i < 16; ++i) b[size_t(i)] = f(i);
    return b;
}
}  // namespace

TEST_CASE("Video 1: flat blocks are one color and two-tone blocks are exact") {
    using video1::RGB;
    Block flat = block([](int) { return RGB{100, 150, 200}; });
    CHECK(video1::codeBlock(flat, nullptr, 48).mode == video1::Mode::Fill);
    // Half black, half white: two colors reproduce it exactly.
    Block edge = block([](int i) { return i % 4 < 2 ? RGB{0, 0, 0} : RGB{255, 255, 255}; });
    auto coded = video1::codeBlock(edge, nullptr, 48);
    CHECK(coded.mode == video1::Mode::Two);
    CHECK(coded.pixels == edge);
    // Four different quarters need eight colors.
    Block quads = block([](int i) {
        int q = (i / 8) * 2 + (i % 4) / 2;
        const RGB base[4] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0}};
        RGB c = base[q];
        return (i % 2 == 0) ? c : RGB{c.x * 0.3f, c.y * 0.3f, c.z * 0.3f};
    });
    CHECK(video1::codeBlock(quads, nullptr, 48).mode == video1::Mode::Eight);
}

TEST_CASE("Video 1: colors are fifteen-bit") {
    using video1::RGB;
    Block px = block([](int i) { return RGB{float(i * 13 % 256), float(i * 29 % 256), float(i * 7 % 256)}; });
    for (const RGB &c : video1::codeBlock(px, nullptr, 1).pixels)
        for (float v : {c.x, c.y, c.z}) CHECK(std::fabs(std::round(v * 31 / 255) * 255 / 31 - v) < 1e-3f);
}

TEST_CASE("Video 1: unchanged blocks are skipped") {
    using video1::RGB;
    Block px = block([](int i) { return RGB{float(i * 9), 80, 160}; });
    CHECK(video1::codeBlock(px, &px, 4).mode == video1::Mode::Skip);
    Block moved = block([](int i) { return RGB{float((15 - i) * 9), 80, 160}; });
    CHECK(video1::codeBlock(moved, &px, 4).mode != video1::Mode::Skip);
}

TEST_CASE("Video 1: the hidden controls do nothing") {
    Settings s = values({{"codec", 1}, {"datamosh", 1}, {"deblocking", 1}, {"chroma", 3}});
    CHECK(s.chroma() == Chroma::Full);  // Video 1 keeps full color
    CHECK(s.effective(kDatamosh) == 0);
    CHECK(s.effective(kDeblocking) == 0);
    s[kCodec] = 0;
    CHECK(s.effective(kDatamosh) == 1);
    CHECK(s.chroma() == Chroma::QuarterWide);
    CHECK_FALSE(param_applies(kDatamosh, CodecKind::BlockColors));
    CHECK(param_applies(kQuality, CodecKind::BlockColors));
}

TEST_CASE("Video 1: more quality never hurts") {
    const int w = 96, h = 72;
    Pixels px(size_t(w) * h * 4, 255);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            size_t i = (size_t(y) * w + x) * 4;
            auto wave = [&](double fx, double fy, double p) { return std::sin(double(x) * fx + double(y) * fy + p); };
            double v = 0.5 * wave(0.05, 0.03, 0) + 0.3 * wave(0.21, -0.17, 1) + 0.2 * wave(0.9, 0.7, 2);
            px[i] = uint8_t(128 + 100 * v);
            px[i + 1] = uint8_t(128 + 100 * (0.6 * wave(0.04, 0.08, 3) + 0.4 * wave(0.5, 0.3, 4)));
            px[i + 2] = uint8_t(128 + 100 * (0.7 * wave(0.07, -0.02, 5) + 0.3 * wave(1.1, 0.4, 6)));
        }
    Geometry g{w, h, Chroma::Full};
    auto src = Renderer::capture(px, g, values({{"sensor_noise", 0}, {"noise_reduction", 0}, {"codec", 1}}), 0);
    std::vector<float> errors;
    for (double q : {0.0, 0.1, 0.5, 1.0}) {
        CodecState st(g);
        auto out = video1::step(st, src, FrameKind::Key, 0, {q, 0, 0}, 0);
        float e = 0;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                e += std::fabs(out->y.data[size_t(y) * out->y.width + x] - src->y.data[size_t(y) * src->y.width + x]);
        errors.push_back(e / float(w * h));
    }
    INFO(errors[0] << " " << errors[1] << " " << errors[2] << " " << errors[3]);
    for (size_t i = 1; i < errors.size(); ++i) CHECK(errors[i - 1] >= errors[i]);
    CHECK(errors[0] > 2 * errors[3]);
}

TEST_CASE("Video 1: lost blocks stay stale until they are coded again") {
    // Through the renderer: a moving scene, keyframes every 10 frames, heavy
    // loss. Damage appears, and every keyframe (which arrives whole) clears it.
    Settings clean = format_by_id("cd_rom")->settings();
    clean[kFrameRate] = 30;
    clean[kKeyframeInterval] = 1.0 / 3;
    Settings lossy = clean;
    lossy[kDataLoss] = 0.8;
    auto a = played(clean, 21), b = played(lossy, 21);
    CHECK(a[0] == b[0]);  // keyframes arrive whole
    CHECK(a[10] == b[10]);
    CHECK(a[20] == b[20]);
    bool damaged = false;
    for (int n = 1; n < 10; ++n) damaged = damaged || a[size_t(n)] != b[size_t(n)];
    CHECK(damaged);  // loss shows between keyframes
}

TEST_CASE("Video 1 looks the same jumped to as played, with loss") {
    Settings s = format_by_id("cd_rom")->settings();
    s[kFrameRate] = 30;
    s[kKeyframeInterval] = 0.5;
    s[kDataLoss] = 0.4;
    checkJumpsMatchPlaying(s, "video 1 + loss", 40, 16);
}

// ---- One field down the chain (port of upstream FieldLinesTests.swift) ------------------------------

TEST_CASE("one field: the chain gets half the lines, the camera the whole frame") {
    json look = {{"digital", {{"enabled", true}, {"values", {{"fields", 1}}}}},
                 {"downscale", {{"enabled", true}, {"width", 480}}}};
    CHECK(chain_line_height(look) == 2);
    CHECK(field_lines(600, 2) == 300);
    CHECK(field_lines(12, 2) == 8);  // upstream max(8, h / 2)
    CHECK(field_lines(600, 1) == 600);
    // Fields default to One field; Progressive, the stage off or no Downscale: square lines.
    json dflt = {{"digital", {{"enabled", true}}}};
    CHECK(chain_line_height(dflt) == 2);
    look["digital"]["values"]["fields"] = 0;
    CHECK(chain_line_height(look) == 1);
    look["digital"]["values"]["fields"] = 1;
    look["downscale"]["enabled"] = false;
    CHECK(chain_line_height(look) == 1);
    look["downscale"]["enabled"] = true;
    look["digital"]["enabled"] = false;
    CHECK(chain_line_height(look) == 1);
}

// ---- looks saved before 0.19 (migrate.h) -------------------------------------------------------------

TEST_CASE("migration: version 1 looks map onto today's controls") {
    json v1 = {{"version", 1},
               {"digital", {{"enabled", true}, {"values", {{"quality", 0.22}, {"exposure", 0.3}, {"black_level", 0.04},
                                                           {"lens_blur", 0.3}, {"fields", 1}}}}},
               {"timeline", {{"keys", json::array({json{{"t", 0}, {"digital", {{"quality", 0.22}}}}})}}}};
    json v = migrate::values_for(v1);
    CHECK(std::fabs(v["quality"].get<double>() - 0.749) < 0.001);
    CHECK(std::fabs(v["ccd_response"].get<double>() - 0.5) < 1e-9);  // max(0.3 / 0.6, 0.04 / 0.08)
    CHECK_FALSE(v.contains("exposure"));
    CHECK_FALSE(v.contains("lens_blur"));
    json up = migrate::upgraded(v1);
    CHECK(up["version"] == 2);
    CHECK(std::fabs(up["timeline"]["keys"][0]["digital"]["quality"].get<double>() - 0.749) < 0.001);
    // A 0.4.0 plug-in look (no "version", the old controls present) is version 1 too.
    json plugin040 = {{"digital", {{"enabled", true}, {"values", {{"quality", 0.22}, {"exposure", 0}, {"black_level", 0},
                                                                  {"lens_blur", 0}}}}}};
    CHECK(migrate::look_version(plugin040) == 1);
    CHECK(std::fabs(migrate::values_for(plugin040)["quality"].get<double>() - 0.749) < 0.001);
    CHECK(migrate::values_for(plugin040)["ccd_response"] == 0);
    // Today's looks (and the plug-ins' looks without "version") are left alone.
    json now = {{"digital", {{"enabled", true}, {"values", {{"quality", 0.3}, {"ccd_response", 0}}}}}};
    CHECK(migrate::look_version(now) == 2);
    CHECK(migrate::values_for(now)["quality"] == 0.3);
}

TEST_CASE("migration: the 0.4.0 controls kept as legacy params") {
    // A 0.4.0 instance loaded today: Quality (new curve) at its default, the
    // old Quality and Exposure under /digital/legacy.
    json look = {{"digital", {{"enabled", true},
                              {"values", {{"quality", migrate::kQualityDefault}, {"ccd_response", 0}}},
                              {"legacy", {{"quality", 0.4}, {"exposure", 0.6}}}}}};
    json v = migrate::values_for(look);
    CHECK(std::fabs(v["quality"].get<double>() - migrate::quality_from_jpeg_scale(0.4)) < 1e-12);
    CHECK(v["ccd_response"] == 1.0);
    // Moving today's controls wins over the legacy ones.
    look["digital"]["values"]["quality"] = 0.5;
    look["digital"]["values"]["ccd_response"] = 0.2;
    v = migrate::values_for(look);
    CHECK(v["quality"] == 0.5);
    CHECK(v["ccd_response"] == 0.2);
    // And the stage renders a migrated look like the same look written today.
    json old = lookFor(Settings());
    old["digital"]["values"].erase("ccd_response");
    old["digital"]["values"]["quality"] = 0.22;
    old["digital"]["values"]["exposure"] = 0.3;
    old["version"] = 1;
    json today = lookFor(Settings());
    today["digital"]["values"] = migrate::values_for(old);
    auto run = [](const json &l) {
        ImageBGRA8 img;
        img.width = kW;
        img.height = kH;
        img.px = frame(2);
        FrameContext ctx;
        ctx.frame = 2;
        ctx.look = &l;
        ctx.look_at = [&l](int64_t) -> const json & { return l; };
        DigitalStage st;
        st.process(img, ctx);
        return img.px;
    };
    CHECK(run(old) == run(today));
    CHECK(run(old) != run(lookFor(Settings())));
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
