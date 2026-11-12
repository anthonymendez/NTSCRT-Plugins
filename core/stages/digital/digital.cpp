// digital.cpp — the Digital video stage behind the core's stage contract.
// Port of upstream Sources/CrtCore/DigitalStage.swift's role: runs on the
// shared CPU BGRA8 buffer ahead of ntsc-rs (upstream's NtscStage prefilter),
// rewriting the picture in place at its own size (the capture picture scaled
// back up with hard pixels, so the Downscale to the capture size returns it).
#include "../stages.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "frames_needed.h"
#include "migrate.h"
#include "renderer.h"

namespace ntscrt {

namespace {

// The look's Digital values, migrated from older versions (migrate.h).
digital::Settings parse_settings(const json &look) {
    digital::Settings s;
    const json v = digital::migrate::values_for(look);
    for (auto it = v.begin(); it != v.end(); ++it) {
        const json &val = it.value();
        if (val.is_number()) s.set(it.key(), val.get<double>());
        else if (val.is_boolean()) s.set(it.key(), val.get<bool>() ? 1.0 : 0.0);
    }
    return s;
}

// Upstream DigitalFrameInput.captureSize: the Downscale's size when it's on
// (width from the look, height from the picture's aspect, even), else the
// picture's own, capped at 1280 wide. With One field the camera still
// captures the whole frame (the picture height, not the field's lines).
digital::Size capture_size(const json &look, int width, int height) {
    bool on = true;
    double dw = 320;
    auto ds = look.find("downscale");
    if (ds != look.end() && ds->is_object()) {
        auto e = ds->find("enabled");
        if (e != ds->end() && e->is_boolean()) on = e->get<bool>();
        auto w = ds->find("width");
        if (w != ds->end() && w->is_number()) dw = w->get<double>();
    }
    if (on) {
        int cw = std::max(1, int(std::lround(dw)));
        double aspect = std::max(0.05, double(width) / double(std::max(1, height)));
        return {cw, std::max(16, 2 * int(std::round(double(cw) / aspect / 2)))};
    }
    return digital::Renderer::captureSize(width, height, std::min(width, 1280));
}

}  // namespace

struct DigitalStage::Impl {
    digital::Renderer renderer;
};

DigitalStage::DigitalStage() : impl_(std::make_unique<Impl>()) {}
DigitalStage::~DigitalStage() = default;

bool DigitalStage::active(const json &look) const {
    auto d = look.find("digital");
    return d != look.end() && d->is_object() && d->value("enabled", false);
}

void DigitalStage::process(ImageBGRA8 &inout, const FrameContext &ctx) {
    if (inout.width <= 0 || inout.height <= 0 || inout.px.size() < size_t(inout.width) * inout.height * 4) return;
    static const json empty = json::object();
    const json &look = ctx.look ? *ctx.look : empty;
    digital::Settings settings = parse_settings(look);

    digital::SettingsAt settingsAt;
    if (ctx.look_at) settingsAt = [&ctx](int f) { return parse_settings(ctx.look_at(f)); };
    digital::EarlierFrames earlier;
    if (ctx.source_at) {
        earlier = [&ctx](int f) -> std::optional<digital::SourceFrame> {
            auto img = ctx.source_at(f);
            if (!img || img->width <= 0 || img->height <= 0 ||
                img->px.size() < size_t(img->width) * img->height * 4)
                return std::nullopt;
            return digital::SourceFrame{img->px.data(), img->width, img->height, img->width * 4, img};
        };
    }
    const int64_t n = digital_frame(ctx);
    impl_->renderer.process(int(std::clamp<int64_t>(n, 0, INT32_MAX / 2)), inout.px.data(), inout.width,
                            inout.height, inout.width * 4, capture_size(look, inout.width, inout.height), ctx.fps,
                            settings, settingsAt, earlier, ctx.seed);
}

void DigitalStage::flush() { impl_->renderer.flush(); }

std::string DigitalStage::profile_note() const {
    const digital::Renderer::Profile &p = impl_->renderer.lastProfile();
    char b[256];
    std::snprintf(b, sizeof b,
                  " digital_ms=scale:%.3f,earlier:%.3f,camera:%.3f,codec:%.3f,deblock:%.3f,upsample:%.3f,upscale:%.3f"
                  " digital_captures=%d",
                  p.scale, p.earlier, p.camera, p.codec, p.deblock, p.upsample, p.upscale, p.captures);
    return b;
}

namespace digital {
int64_t first_source_frame(const json &look, double fps, int64_t n, uint64_t seed) {
    auto d = look.find("digital");
    if (n <= 0 || d == look.end() || !d->is_object() || !d->value("enabled", false)) return std::max<int64_t>(0, n);
    const Settings s = parse_settings(look);
    const Timing timing(fps > 0 ? fps : 30.0, s);
    const int c = timing.captureOfFrame(int(std::clamp<int64_t>(n, 0, INT32_MAX / 2)));
    int start;
    if (timing.intraOnly()) {
        start = std::max(0, c - 1);
    } else {
        const int g = timing.keyframeEvery;
        const uint64_t mix = seed_mix(seed);
        int b = (c / g) * g;
        while (b > 0 && timing.kind(b, s.effective(kDatamosh), mix) != FrameKind::Key) b -= g;
        start = b;
    }
    return std::min<int64_t>(n, timing.frameOfCapture(start));
}
}  // namespace digital

}  // namespace ntscrt
