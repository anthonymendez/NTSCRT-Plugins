// renderer.cpp — port of upstream DigitalVideoParams.swift and
// DigitalVideoRenderer.swift (upstream v0.21.0, 8b26092).
//
// Differences from upstream, all deliberate:
// - DigitalParam.startingValues (a new window's settings) are not ported: see
//   renderer.h.
// - vImageScale_ARGB8888(kvImageHighQualityResampling) is replaced by a
//   separable Lanczos-5 resampler (scaleLanczos), see the note there.
// - Settings history: instead of an opaque history id, every run records the
//   fingerprint of the settings each capture was coded under; a checkpoint
//   (or the stream itself) is reused only while those still hold, so an
//   edited keyframe anywhere before the frame triggers a correct re-run.
//   Without keyframes this is exactly upstream's behaviour, except that new
//   settings during forward playback re-run from the last keyframe instead of
//   "acting from here on" (so a frame always looks the same however reached).
// - ctx seed mixes into the error/noise RNGs (seed 0 = upstream patterns).
#include "renderer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "../../src/sim_segment.h"
#include "parallel.h"
#include "video1.h"

namespace ntscrt::digital {

static inline float clampf(float v, float lo, float hi) { return std::min(hi, std::max(lo, v)); }

namespace {
using ProfileClock = std::chrono::steady_clock;
// Adds the time since `t` to `acc` (ms) and restarts `t`.
inline void lap(double &acc, ProfileClock::time_point &t) {
    const auto now = ProfileClock::now();
    acc += std::chrono::duration<double, std::milli>(now - t).count();
    t = now;
}
}  // namespace

// ---- params ------------------------------------------------------------------------

// Upstream DigitalParam.all (ids, ranges, defaults; structureIDs).
const ParamInfo kParams[kParamCount] = {
    {"frame_rate", 1, 60, 15, false, true},
    {"fields", 0, 1, 1, true, true},
    {"aliasing", 0, 1, 0, false, true},
    {"sensor_noise", 0, 1, 0.4, false, false},
    {"low_light_noise", 0, 1, 0, false, false},
    {"noise_reduction", 0, 1, 0.5, false, false},
    {"ccd_response", 0, 1, 0, false, false},
    {"white_balance", -1, 1, 0, false, false},
    {"tint", -1, 1, 0, false, false},
    {"saturation", 0, 2, 1, false, false},
    {"sharpening", 0, 1, 0, false, false},
    {"sharpening_width", 1, 4, 1, false, false},
    {"codec", 0, 1, 0, true, true},
    {"chroma", 0, 3, 2, true, true},
    {"quality", 0, 1, 0.749, false, false},
    {"keyframe_interval", 0, 10, 0, false, true},
    {"fixed_bitrate", 0, 1, 0, false, false},
    {"deblocking", 0, 1, 0, false, false},
    {"data_loss", 0, 1, 0, false, false},
    {"datamosh", 0, 1, 0, false, false},
};

std::optional<Param> param_by_id(std::string_view id) {
    for (int i = 0; i < kParamCount; ++i)
        if (id == kParams[i].id) return Param(i);
    return std::nullopt;
}

bool param_applies(Param p, CodecKind codec) {
    // Video 1 keeps full-resolution color, has no deblocking, and no motion
    // for a datamosh to drag.
    if (codec == CodecKind::BlockColors) return p != kChroma && p != kDeblocking && p != kDatamosh;
    return true;
}

Settings::Settings() {
    for (int i = 0; i < kParamCount; ++i) v[size_t(i)] = kParams[i].defaultValue;
}

bool Settings::set(std::string_view id, double value) {
    auto p = param_by_id(id);
    if (!p) return false;
    v[size_t(*p)] = value;
    return true;
}

CodecKind Settings::codec() const {
    return std::round((*this)[kCodec]) == 1 ? CodecKind::BlockColors : CodecKind::DCT;
}

Chroma Settings::chroma() const {
    if (codec() == CodecKind::BlockColors) return Chroma::Full;
    double r = std::round((*this)[kChroma]);
    if (r >= 0 && r <= 3) return Chroma(int(r));
    return Chroma::HalfBoth;
}

double Settings::effective(Param p) const {
    return !param_applies(p, codec()) && p != kChroma ? 0.0 : (*this)[p];
}

Settings Settings::holdingStructure(const Settings &base) const {
    Settings s = *this;
    for (int i = 0; i < kParamCount; ++i)
        if (kParams[i].structure) s.v[size_t(i)] = base.v[size_t(i)];
    return s;
}

uint64_t Settings::fingerprint() const {
    uint64_t h = 0xCBF29CE484222325ull;
    for (double d : v) {
        uint64_t bits;
        if (d == 0) d = 0;  // -0 == 0
        std::memcpy(&bits, &d, 8);
        h ^= bits;
        h *= 0x100000001B3ull;
        h ^= h >> 29;
    }
    return h;
}

Settings Format::settings() const {
    Settings s;
    for (auto &[k, val] : values) s.set(k, val);
    return s;
}

const std::vector<Format> &formats() {
    static const std::vector<Format> all = {
        {"webcam", "Webcam (2005)", 320,
         {{"frame_rate", 15}, {"aliasing", 0.15}, {"sensor_noise", 0.4}, {"noise_reduction", 0.5},
          {"ccd_response", 0.5}, {"white_balance", -0.15}, {"saturation", 0.85}, {"sharpening", 0.25},
          {"chroma", 2}, {"quality", 0.749}, {"keyframe_interval", 0}, {"fixed_bitrate", 0}, {"deblocking", 0},
          {"data_loss", 0}, {"datamosh", 0}}},
        {"video_call", "Video call", 320,
         {{"frame_rate", 15}, {"sensor_noise", 0.25}, {"noise_reduction", 0.6}, {"ccd_response", 0.3},
          {"saturation", 0.9}, {"chroma", 2}, {"quality", 0.749}, {"keyframe_interval", 4}, {"fixed_bitrate", 0.8},
          {"deblocking", 0.6}, {"data_loss", 0.15}, {"datamosh", 0}}},
        {"minidv", "MiniDV", 720,
         {{"frame_rate", 30}, {"fields", 1}, {"aliasing", 0.6}, {"sensor_noise", 0.12}, {"noise_reduction", 0},
          {"ccd_response", 1}, {"white_balance", 0.4}, {"tint", 0.2}, {"saturation", 1.15}, {"sharpening", 0.6},
          {"chroma", 3}, {"quality", 0.894}, {"keyframe_interval", 0}, {"fixed_bitrate", 0.3}, {"deblocking", 0},
          {"data_loss", 0}, {"datamosh", 0}}},
        {"early_web", "Early web video", 320,
         {{"frame_rate", 15}, {"sensor_noise", 0}, {"noise_reduction", 0}, {"chroma", 2}, {"quality", 0.651},
          {"keyframe_interval", 5}, {"fixed_bitrate", 0.7}, {"deblocking", 0}, {"data_loss", 0}, {"datamosh", 0}}},
        {"cd_rom", "CD-ROM (Video 1)", 240,
         {{"codec", 1}, {"frame_rate", 12}, {"sensor_noise", 0}, {"noise_reduction", 0}, {"quality", 0.57},
          {"keyframe_interval", 1.25}, {"fixed_bitrate", 0}, {"data_loss", 0}}},
        {"datamosh", "Datamosh", 480,
         {{"frame_rate", 30}, {"sensor_noise", 0}, {"noise_reduction", 0}, {"chroma", 2}, {"quality", 0.826},
          {"keyframe_interval", 1}, {"fixed_bitrate", 0.3}, {"deblocking", 0}, {"data_loss", 0}, {"datamosh", 1}}},
    };
    return all;
}

const Format *format_by_id(std::string_view id) {
    for (auto &f : formats())
        if (id == f.id) return &f;
    return nullptr;
}

uint64_t seed_mix(uint64_t seed) {
    if (seed == 0) return 0;
    SplitMix r(seed ^ 0x6469676974616C00ull);  // "digital"
    uint64_t m = r.next();
    return m == 0 ? 1 : m;
}

// ---- timing ---------------------------------------------------------------------------

Timing::Timing(double fps, const Settings &settings) {
    sourceFPS = std::max(1.0, fps);
    captureFPS = std::min(sourceFPS, std::max(1.0, settings[kFrameRate]));
    double seconds = settings[kKeyframeInterval];
    keyframeEvery = seconds <= 0 ? 1 : std::max(2, int(std::round(seconds * captureFPS)));
}

int Timing::captureOfFrame(int n) const {
    double ratio = captureFPS / sourceFPS;
    return ratio >= 1 ? n : int(std::floor(double(n) * ratio + 1e-9));
}

int Timing::frameOfCapture(int c) const {
    double ratio = captureFPS / sourceFPS;
    return ratio >= 1 ? c : int(std::ceil(double(c) / ratio - 1e-9));
}

FrameKind Timing::kind(int c, double datamosh, uint64_t seedMix) const {
    if (intraOnly()) return FrameKind::IntraOnly;
    if (c % keyframeEvery != 0) return FrameKind::Predicted;
    // The first keyframe of each history segment after the first (core/src/sim_segment.h)
    // is never dropped: a datamosh can't make the stream reach back further than that.
    if (c > 0 && sim_segment_start(frameOfCapture(c), sourceFPS) >
                     sim_segment_start(frameOfCapture(c - keyframeEvery), sourceFPS))
        return FrameKind::Key;
    return codec::dropsKey(c / keyframeEvery, datamosh, seedMix) ? FrameKind::DroppedKey : FrameKind::Key;
}

// ---- the stream ---------------------------------------------------------------------------

Size Renderer::captureSize(int width, int height, int captureWidth) {
    int cw = std::max(16, std::min(captureWidth, width));
    double aspect = std::max(0.05, double(width) / double(std::max(1, height)));
    return {cw, std::max(16, 2 * int(std::round(double(cw) / aspect / 2)))};
}

struct Renderer::Call {
    int n;
    Settings structure;
    Timing timing;
    Geometry geometry;
    StreamKey key;
    const SettingsAt &settingsAt;
    const EarlierFrames &earlier;
    Size size;
    std::vector<uint8_t> current;
    std::map<int, Settings> memo;

    const Settings &settingsFor(int c) {
        if (!settingsAt) return structure;
        auto it = memo.find(c);
        if (it != memo.end()) return it->second;
        return memo.emplace(c, settingsAt(timing.frameOfCapture(c)).holdingStructure(structure)).first->second;
    }
    uint64_t fp(int c) { return settingsFor(c).fingerprint(); }
};

void Renderer::flush() {
    stream_.reset();
    lastFrame_.reset();
    state_.reset();
    lastCapture_.reset();
    run_ = {};
    shown_.clear();
    checkpoints_.clear();
    checkpointOrder_.clear();
    checkpointBytes_ = 0;
}

void Renderer::process(int n, uint8_t *pixels, int width, int height, int rowBytes, Size capture,
                       double sourceFPS, const Settings &settings, const SettingsAt &settingsAt,
                       const EarlierFrames &earlier, uint64_t seed) {
    if (width <= 0 || height <= 0 || capture.width <= 0 || capture.height <= 0) return;
    n = std::max(0, n);
    const Settings &structure = settings;
    Timing timing(sourceFPS, structure);
    Geometry geometry{capture.width, capture.height, structure.chroma()};
    StreamKey key{geometry, timing, seed_mix(seed), !earlier};
    if (!stream_ || *stream_ != key) {
        stream_ = key;
        state_.reset();
        lastCapture_.reset();
        run_ = {};
    }
    Call call{n, structure, timing, geometry, key, settingsAt, earlier, capture, {}, {}};
    const int c = timing.captureOfFrame(n);
    lastRunLength_ = 0;
    profile_ = {};
    auto t = ProfileClock::now();

    // The current picture at the capture size, scaled once.
    call.current = scale(pixels, width, height, rowBytes, capture, structure);
    lap(profile_.scale, t);

    // The frame rendered again (an edit elsewhere on the timeline re-renders
    // the frame on screen): check every capture of the run. A repeat during
    // playback, or moving forward: the latest capture only.
    const bool again = lastFrame_ && *lastFrame_ == n;
    lastFrame_ = n;
    if (lastCapture_ && state_ && *lastCapture_ == c && runHolds(call, run_, again ? run_.origin : c, c)) {
        // Same frame, same settings: what's shown already.
    } else if (lastCapture_ && state_ && c > *lastCapture_ &&
               c - *lastCapture_ <= std::max(2, 2 * timing.keyframeEvery) &&
               runHolds(call, run_, *lastCapture_, *lastCapture_)) {
        // Moving forward: carry on.
        bool ok = true;
        for (int k = *lastCapture_ + 1; k <= c; ++k) {
            std::optional<std::vector<uint8_t>> src;
            if (timing.frameOfCapture(k) == n) src = call.current;
            else src = earlierCapture(call, k);
            if (!src) {
                ok = false;
                break;
            }
            advance(call, k, *src);
        }
        if (!ok) rerun(call, c);
    } else {
        rerun(call, c);
    }

    t = ProfileClock::now();
    upscale(shown_, capture, pixels, width, height, rowBytes);
    lap(profile_.upscale, t);
    profile_.captures = lastRunLength_;
}

bool Renderer::runHolds(Call &call, const Run &run, int from, int upTo) {
    if (upTo < run.origin || size_t(upTo - run.origin) >= run.fps.size()) return false;
    for (int k = std::max(from, run.origin); k <= upTo; ++k)
        if (run.fps[size_t(k - run.origin)] != call.fp(k)) return false;
    return true;
}

void Renderer::rerun(Call &call, int c) {
    const Timing &timing = call.timing;
    const int g = timing.keyframeEvery;
    int start;
    const Checkpoint *restored = nullptr;
    if (timing.intraOnly()) {
        // Each frame stands alone; only the one before (for patching lost
        // blocks) matters.
        start = std::max(0, c - 1);
    } else {
        int b = (c / g) * g;
        while (b > 0) {
            FrameKind kind = timing.kind(b, call.settingsFor(b).effective(kDatamosh), call.key.seedMix);
            if (kind == FrameKind::Key) break;
            auto it = checkpoints_.find({call.key, b});
            if (it != checkpoints_.end() && runHolds(call, it->second.run, it->second.run.origin, b)) {
                restored = &it->second;
                break;
            }
            b -= g;
        }
        start = b;
    }
    if (restored) {
        state_ = restored->state->copy();
        run_ = restored->run;
        lastCapture_ = start;
    } else {
        state_ = std::make_shared<CodecState>(call.geometry);
        run_ = Run{start, {}};
        lastCapture_.reset();
    }
    const int first = restored ? start + 1 : start;
    if (first > c) {
        // The checkpoint is the frame itself.
        const Settings &s = call.settingsFor(c);
        auto t = ProfileClock::now();
        double deblock = 0;
        shown_ = render(*state_->decoder, call.geometry, s, codec::Steps(s[kQuality], 1).inter, &deblock);
        lap(profile_.upsample, t);
        profile_.upsample -= deblock;
        profile_.deblock += deblock;
        lastCapture_ = c;
        return;
    }
    for (int k = first; k <= c; ++k) {
        // The camera took capture k at its first frame; `current` is frame
        // n, which may be a later repeat of it.
        if (timing.frameOfCapture(k) == call.n) {
            advance(call, k, call.current);
        } else {
            auto src = earlierCapture(call, k);
            advance(call, k, src ? *src : call.current);
        }
    }
}

std::optional<std::vector<uint8_t>> Renderer::earlierCapture(Call &call, int k) {
    if (!call.earlier) return call.current;
    auto t = ProfileClock::now();
    auto f = call.earlier(call.timing.frameOfCapture(k));
    if (!f || !f->data || f->width <= 0 || f->height <= 0) {
        lap(profile_.earlier, t);
        return std::nullopt;
    }
    auto out = scale(f->data, f->width, f->height, f->rowBytes, call.size, call.structure);
    lap(profile_.earlier, t);
    return out;
}

void Renderer::advance(Call &call, int k, const std::vector<uint8_t> &source) {
    if (!state_) state_ = std::make_shared<CodecState>(call.geometry);
    if (run_.fps.size() != size_t(k - run_.origin)) run_ = Run{k, {}};  // never: runs are contiguous
    const Settings &settings = call.settingsFor(k);
    auto t = ProfileClock::now();
    PicturePtr picture = capture(source, call.geometry, settings, k, call.key.seedMix);
    lap(profile_.camera, t);
    FrameKind kind = call.timing.kind(k, settings.effective(kDatamosh), call.key.seedMix);
    if (kind != FrameKind::IntraOnly && !state_->encoder) kind = FrameKind::Key;
    const codec::FrameSettings frame{settings[kQuality], settings[kFixedBitrate], settings[kDataLoss]};
    codec::StepResult result;
    switch (settings.codec()) {
    case CodecKind::DCT: result = codec::step(*state_, picture, kind, k, frame, call.key.seedMix); break;
    case CodecKind::BlockColors: result = {video1::step(*state_, picture, kind, k, frame, call.key.seedMix), 0}; break;
    }
    lap(profile_.codec, t);
    lastRunLength_ += 1;
    lastCapture_ = k;
    double deblock = 0;
    shown_ = render(*result.picture, call.geometry, settings, result.step, &deblock);
    lap(profile_.upsample, t);
    profile_.upsample -= deblock;
    profile_.deblock += deblock;
    run_.fps.push_back(settings.fingerprint());

    // Checkpoint at keyframes the decoder lost (the only places its history
    // reaches past).
    if (kind == FrameKind::DroppedKey) {
        Checkpoint cp;
        cp.state = state_->copy();
        cp.run = run_;
        cp.bytes = (state_->encoder ? state_->encoder->byteCount() : 0) * 2 + run_.fps.size() * 8;
        store(call.key, k, std::move(cp));
    }
}

void Renderer::store(const StreamKey &key, int capture, Checkpoint cp) {
    auto id = std::make_pair(key, capture);
    auto it = checkpoints_.find(id);
    if (it != checkpoints_.end()) {
        if (it->second.run.origin == cp.run.origin && it->second.run.fps == cp.run.fps) return;
        checkpointBytes_ -= it->second.bytes;
        checkpoints_.erase(it);
        checkpointOrder_.erase(std::find(checkpointOrder_.begin(), checkpointOrder_.end(), id));
    }
    checkpointBytes_ += cp.bytes;
    checkpoints_.emplace(id, std::move(cp));
    checkpointOrder_.push_back(id);
    while (checkpointBytes_ > kCheckpointBudget && !checkpointOrder_.empty()) {
        auto oldest = checkpointOrder_.front();
        checkpointOrder_.erase(checkpointOrder_.begin());
        auto o = checkpoints_.find(oldest);
        if (o != checkpoints_.end()) {
            checkpointBytes_ -= o->second.bytes;
            checkpoints_.erase(o);
        }
    }
}

// ---- pixels in ------------------------------------------------------------------------------

// Stand-in for vImageScale_ARGB8888 with kvImageHighQualityResampling, which
// Apple documents as a Lanczos filter (Lanczos5 for the high-quality flag).
// Separable Lanczos-5, widened by the scale factor when shrinking so it
// area-filters, pixel centers aligned, edges clamped, all four channels
// alike. Not bit-identical to vImage (its exact kernel, normalisation and
// fixed-point rounding are not public); expected within a few code values on
// smooth content and never measured against vImage (no macOS here).
namespace {
constexpr int kLanczosA = 5;
struct Taps {
    std::vector<int> start, count;
    std::vector<float> w;  // count[i] weights from offset i * maxTaps
    int maxTaps = 0;
};
Taps makeTaps(int srcLen, int dstLen) {
    Taps t;
    const double s = double(srcLen) / double(dstLen);
    const double fs = std::max(1.0, s);
    const double support = kLanczosA * fs;
    t.maxTaps = int(std::ceil(2 * support)) + 2;
    t.start.resize(size_t(dstLen));
    t.count.resize(size_t(dstLen));
    t.w.assign(size_t(dstLen) * t.maxTaps, 0.0f);
    const double pi = 3.14159265358979323846;
    auto lanczos = [&](double x) {
        x = std::fabs(x);
        if (x < 1e-12) return 1.0;
        if (x >= kLanczosA) return 0.0;
        double px = pi * x;
        return kLanczosA * std::sin(px) * std::sin(px / kLanczosA) / (px * px);
    };
    std::vector<double> tmp(size_t(t.maxTaps));
    for (int i = 0; i < dstLen; ++i) {
        double center = (i + 0.5) * s - 0.5;
        int lo = int(std::floor(center - support)) + 1;
        int hi = int(std::floor(center + support));
        int cnt = std::min(t.maxTaps, hi - lo + 1);
        double sum = 0;
        for (int j = 0; j < cnt; ++j) {
            tmp[size_t(j)] = lanczos((lo + j - center) / fs);
            sum += tmp[size_t(j)];
        }
        t.start[size_t(i)] = lo;
        t.count[size_t(i)] = cnt;
        for (int j = 0; j < cnt; ++j) t.w[size_t(i) * t.maxTaps + j] = float(tmp[size_t(j)] / sum);
    }
    return t;
}
}  // namespace

void Renderer::scaleLanczos(const uint8_t *src, int width, int height, int rowBytes, uint8_t *dst, int cw, int ch) {
    const Taps tx = makeTaps(width, cw), ty = makeTaps(height, ch);
    std::vector<float> mid(size_t(cw) * height * 4);
    float *m = mid.data();
    parallel_for(height, [&](int y) {
        const uint8_t *row = src + size_t(y) * rowBytes;
        float *o = m + size_t(y) * cw * 4;
        for (int x = 0; x < cw; ++x) {
            const float *w = tx.w.data() + size_t(x) * tx.maxTaps;
            int s0 = tx.start[size_t(x)], cnt = tx.count[size_t(x)];
            float a0 = 0, a1 = 0, a2 = 0, a3 = 0;
            for (int j = 0; j < cnt; ++j) {
                int sx = std::min(width - 1, std::max(0, s0 + j));
                const uint8_t *p = row + size_t(sx) * 4;
                a0 += w[j] * p[0];
                a1 += w[j] * p[1];
                a2 += w[j] * p[2];
                a3 += w[j] * p[3];
            }
            o[x * 4] = a0;
            o[x * 4 + 1] = a1;
            o[x * 4 + 2] = a2;
            o[x * 4 + 3] = a3;
        }
    });
    parallel_for(ch, [&](int y) {
        const float *w = ty.w.data() + size_t(y) * ty.maxTaps;
        int s0 = ty.start[size_t(y)], cnt = ty.count[size_t(y)];
        uint8_t *o = dst + size_t(y) * cw * 4;
        for (int x = 0; x < cw * 4; ++x) {
            float a = 0;
            for (int j = 0; j < cnt; ++j) {
                int sy = std::min(height - 1, std::max(0, s0 + j));
                a += w[j] * m[size_t(sy) * cw * 4 + x];
            }
            o[x] = uint8_t(clampf(std::round(a), 0, 255));
        }
    });
}

std::vector<uint8_t> Renderer::scale(const uint8_t *base, int width, int height, int rowBytes, Size size,
                                     const Settings &optics) {
    const int cw = size.width, ch = size.height;
    std::vector<uint8_t> out(size_t(cw) * ch * 4, 0);
    uint8_t *o = out.data();
    if (width == cw && height == ch) {
        for (int y = 0; y < height; ++y) std::memcpy(o + size_t(y) * cw * 4, base + size_t(y) * rowBytes, size_t(width) * 4);
    } else {
        scaleLanczos(base, width, height, rowBytes, o, cw, ch);
    }
    // Aliasing: blended toward point samples up to 50%, red, green and blue
    // read a third of a capture pixel apart (rainbow moire); above 50% the
    // source's finest detail is boosted at each sample.
    const float aliasing = float(optics[kAliasing]);
    if (aliasing > 0) {
        const float mix = std::min(1.0f, aliasing * 2);
        const float boost = std::max(0.0f, aliasing * 2 - 1) * 1.5f;
        const double kx = double(width) / double(cw), ky = double(height) / double(ch);
        parallel_for(ch, [&](int y) {
            int sy = std::min(height - 1, int((double(y) + 0.5) * ky));
            const uint8_t *rows[3] = {base + size_t(std::max(0, sy - 1)) * rowBytes, base + size_t(sy) * rowBytes,
                                      base + size_t(std::min(height - 1, sy + 1)) * rowBytes};
            // One channel at source pixel sx, its fine detail boosted.
            auto read = [&](int sx, int c) -> float {
                float v = float(rows[1][size_t(sx) * 4 + c]);
                if (!(boost > 0)) return v;
                float sum = 0;
                for (const uint8_t *r : rows)
                    for (int dx = -1; dx <= 1; ++dx) sum += float(r[size_t(std::min(width - 1, std::max(0, sx + dx))) * 4 + c]);
                return clampf(v + boost * (v - sum / 9), 0, 255);
            };
            for (int x = 0; x < cw; ++x) {
                double center = (double(x) + 0.5) * kx;
                int xg = std::min(width - 1, int(center));
                int xr = std::min(width - 1, std::max(0, int(center - kx / 3)));
                int xb = std::min(width - 1, int(center + kx / 3));
                size_t i = (size_t(y) * cw + x) * 4;
                float pb = read(xb, 0), pg = read(xg, 1), pr = read(xr, 2);
                o[i] = uint8_t(std::round(float(o[i]) + (pb - float(o[i])) * mix));
                o[i + 1] = uint8_t(std::round(float(o[i + 1]) + (pg - float(o[i + 1])) * mix));
                o[i + 2] = uint8_t(std::round(float(o[i + 2]) + (pr - float(o[i + 2])) * mix));
            }
        });
    }
    if (optics[kFields] >= 0.5) {
        // One field: the even lines, each shown twice.
        for (int y = 1; y < ch; y += 2) std::memcpy(o + size_t(y) * cw * 4, o + size_t(y - 1) * cw * 4, size_t(cw) * 4);
    }
    return out;
}

namespace {
// Edge-preserving smoothing (a camera's noise reduction).
std::vector<float> sigmaFilter(const std::vector<float> &p, int w, int h, float threshold, float mix) {
    std::vector<float> out = p;
    float *o = out.data();
    const float *s = p.data();
    parallel_for(h, [&](int y) {
        for (int x = 0; x < w; ++x) {
            float c = s[size_t(y) * w + x];
            float sum = 0, n = 0;
            for (int dy = -1; dy <= 1; ++dy) {
                int yy = std::min(h - 1, std::max(0, y + dy));
                for (int dx = -1; dx <= 1; ++dx) {
                    float v = s[size_t(yy) * w + std::min(w - 1, std::max(0, x + dx))];
                    if (std::fabs(v - c) < threshold) {
                        sum += v;
                        n += 1;
                    }
                }
            }
            o[size_t(y) * w + x] = c + (sum / n - c) * mix;
        }
    });
    return out;
}

std::vector<float> boxBlur(const std::vector<float> &p, int w, int h, int r, float mix) {
    std::vector<float> tmp = p, out = p;
    const float n = float(2 * r + 1);
    const float *pp = p.data();
    float *tp = tmp.data(), *op = out.data();
    parallel_for(h, [&](int y) {
        for (int x = 0; x < w; ++x) {
            float s = 0;
            for (int d = -r; d <= r; ++d) s += pp[size_t(y) * w + std::min(w - 1, std::max(0, x + d))];
            tp[size_t(y) * w + x] = s / n;
        }
    });
    parallel_for(h, [&](int y) {
        for (int x = 0; x < w; ++x) {
            float s = 0;
            for (int d = -r; d <= r; ++d) s += tp[size_t(std::min(h - 1, std::max(0, y + d))) * w + x];
            size_t i = size_t(y) * w + x;
            op[i] = pp[i] + (s / n - pp[i]) * mix;
        }
    });
    return out;
}
}  // namespace

// sRGB code value (0-255) -> linear light.
static const float *srgb_to_linear() {
    static const std::array<float, 256> lut = [] {
        std::array<float, 256> a{};
        for (int i = 0; i < 256; ++i) {
            float c = float(i) / 255;
            a[size_t(i)] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        return a;
    }();
    return lut.data();
}

// Linear light -> sRGB code value (0-255), unclipped above white so the knee
// can still see how far over a channel is.
static inline float linear_to_srgb(float l) {
    float c = l <= 0.0031308f ? l * 12.92f : 1.055f * std::pow(l, 1 / 2.4f) - 0.055f;
    return c * 255;
}

float Renderer::meteredMean(const std::vector<uint8_t> &bgra, int w, int h, float g0, float g1, float g2) {
    float sum = 0, weight = 0;
    const float *lut = srgb_to_linear();
    for (int y = 0; y < h; y += 2) {
        float dy = (float(y) + 0.5f) / float(h) * 2 - 1;
        for (int x = 0; x < w; x += 2) {
            float dx = (float(x) + 0.5f) / float(w) * 2 - 1;
            float wt = std::max(0.15f, 1 - 0.6f * (dx * dx + dy * dy));
            size_t i = (size_t(y) * w + x) * 4;
            float l = 0.2126f * lut[bgra[i + 2]] * g0 + 0.7152f * lut[bgra[i + 1]] * g1 + 0.0722f * lut[bgra[i]] * g2;
            sum += l * wt;
            weight += wt;
        }
    }
    return weight > 0 ? sum / weight : 0.18f;
}

// Upstream 0.21 (Low-light noise). The median, not the exposure meter's
// center-weighted mean: sources are footage some camera already exposed, and a
// night shot's lit subject in the middle meters as bright as daylight, while
// half its frame stays black.
float Renderer::gainKnee() { return srgb_to_linear()[100]; }

float Renderer::sensorGain(float m) { return std::min(8.0f, std::max(1.0f, gainKnee() / std::max(m, 1e-4f))); }

float Renderer::medianLuma(const std::vector<uint8_t> &bgra, int w, int h) {
    int counts[256] = {};
    int n = 0;
    for (int y = 0; y < h; y += 2) {
        for (int x = 0; x < w; x += 2) {
            size_t i = (size_t(y) * w + x) * 4;
            int l = (306 * int(bgra[i + 2]) + 601 * int(bgra[i + 1]) + 117 * int(bgra[i])) >> 10;
            counts[std::min(255, l)] += 1;
            n += 1;
        }
    }
    int seen = 0;
    for (int v = 0; v < 256; ++v) {
        seen += counts[v];
        if (seen * 2 >= n) return srgb_to_linear()[v];
    }
    return 1;
}

float CCDResponse::exposure(float m) const {
    float full = std::min(4.0f, std::max(0.5f, kTarget / std::max(m, 1e-4f)));
    return std::pow(full, strength);
}

PicturePtr Renderer::capture(const std::vector<uint8_t> &bgra, const Geometry &g, const Settings &settings,
                             int index, uint64_t seedMix) {
    const int w = g.width, h = g.height;
    float noise = float(settings[kSensorNoise]);
    const float lowLight = float(settings[kLowLightNoise]);
    const float nr = float(settings[kNoiseReduction]);
    const float warm = float(settings[kWhiteBalance]), magenta = float(settings[kTint]);
    const float ccd = float(settings[kCCDResponse]);
    const CCDResponse curve{ccd};
    float gainR = 1 + 0.25f * warm, gainB = 1 - 0.25f * warm;
    float gainG = 1 - 0.15f * magenta;
    float flare = 0;
    if (lowLight > 0 && noise > 0) noise *= 1 + lowLight * (sensorGain(medianLuma(bgra, w, h)) - 1);
    if (ccd > 0) {
        const float exposure = curve.exposure(meteredMean(bgra, w, h, gainR, gainG, gainB));
        gainR *= exposure;
        gainG *= exposure;
        gainB *= exposure;
        flare = curve.flare(meteredMean(bgra, w, h, gainR, gainG, gainB));
    }
    // Gains act on light, so they need linear values (decoded sRGB).
    const bool linear = gainR != 1 || gainG != 1 || gainB != 1 || flare > 0;
    const float *decode = srgb_to_linear();
    std::vector<float> yf(size_t(w) * h, 0), cbf(size_t(w) * h, 0), crf(size_t(w) * h, 0);
    {
        const uint8_t *s = bgra.data();
        float *Y = yf.data(), *CB = cbf.data(), *CR = crf.data();
        parallel_for(h, [&](int y) {
            // The row's seed hashed first (upstream 0.21): SplitMix steps its
            // state by the same constant the row number is multiplied by, so
            // unhashed, each row would replay the row above a few pixels along
            // (streaks instead of grain). The ctx seed mixes into the unhashed seed.
            SplitMix seeder(((uint64_t(index) * 0x2545F4914F6CDD1Dull) ^ (uint64_t(y) * 0x9E3779B97F4A7C15ull)) ^ seedMix);
            SplitMix rng(seeder.next());
            auto gauss = [&]() -> float {
                double a = rng.unit();
                double b = rng.unit();
                return float(a + b - 1) * 2.449f;
            };
            for (int x = 0; x < w; ++x) {
                size_t i = size_t(y) * w + x;
                float b = float(s[i * 4]), gr = float(s[i * 4 + 1]), r = float(s[i * 4 + 2]);
                if (linear) {
                    r = linear_to_srgb(decode[int(r)] * gainR + flare);
                    gr = linear_to_srgb(decode[int(gr)] * gainG + flare);
                    b = linear_to_srgb(decode[int(b)] * gainB + flare);
                }
                if (noise > 0) {
                    // Gain noise: worst in the shadows, and under the clip.
                    float shade = 1.3f - 0.6f * std::min(255.0f, 0.299f * r + 0.587f * gr + 0.114f * b) / 255;
                    float n = gauss() * 7 * noise * shade;
                    float nb = gauss() * 11 * noise * shade;
                    float nc = gauss() * 11 * noise * shade;
                    r += n + 1.402f * nc;
                    gr += n - 0.344136f * nb - 0.714136f * nc;
                    b += n + 1.772f * nb;
                }
                if (ccd > 0) {
                    r = curve.knee(r);
                    gr = curve.knee(gr);
                    b = curve.knee(b);
                }
                if (linear || noise > 0) {
                    r = clampf(r, 0, 255);
                    gr = clampf(gr, 0, 255);
                    b = clampf(b, 0, 255);
                }
                Y[i] = 0.299f * r + 0.587f * gr + 0.114f * b;
                CB[i] = 128 - 0.168736f * r - 0.331264f * gr + 0.5f * b;
                CR[i] = 128 + 0.5f * r - 0.418688f * gr - 0.081312f * b;
            }
        });
    }
    if (nr > 0) {
        yf = sigmaFilter(yf, w, h, 4 + 26 * nr, std::min(1.0f, nr * 1.2f));
        cbf = boxBlur(cbf, w, h, 2, nr);
        crf = boxBlur(crf, w, h, 2, nr);
    }
    const float sharpening = float(settings[kSharpening]);
    if (sharpening > 0) {
        // Detail: add back the difference from a local blur, so edges
        // overshoot both ways (halos), as wide as the blur. A fractional
        // width blends the two nearest whole radii.
        const float width = std::min(4.0f, std::max(1.0f, float(settings[kSharpeningWidth])));
        const int r = int(width);
        const float f = width - float(r);
        std::vector<float> soft = boxBlur(yf, w, h, r, 1);
        if (f > 0) {
            const std::vector<float> wider = boxBlur(yf, w, h, r + 1, 1);
            for (size_t i = 0; i < soft.size(); ++i) soft[i] += (wider[i] - soft[i]) * f;
        }
        for (size_t i = 0; i < yf.size(); ++i) yf[i] = clampf(yf[i] + 2.2f * sharpening * (yf[i] - soft[i]), 0, 255);
    }
    const float black = curve.setup() * 255;
    const float saturation = float(settings[kSaturation]);
    if (black > 0 || saturation != 1) {
        for (size_t i = 0; i < yf.size(); ++i) {
            yf[i] = black + yf[i] * (1 - black / 255);
            cbf[i] = 128 + (cbf[i] - 128) * saturation;
            crf[i] = 128 + (crf[i] - 128) * saturation;
        }
    }
    auto pic = std::make_shared<Picture>(g);
    // Luma, padded by repeating the edge.
    const int lw = pic->y.width;
    float *py = pic->y.p();
    const float *yp = yf.data(), *cbp = cbf.data(), *crp = crf.data();
    for (int y = 0; y < pic->y.height; ++y) {
        int sy = std::min(h - 1, y);
        for (int x = 0; x < lw; ++x) py[size_t(y) * lw + x] = yp[size_t(sy) * w + std::min(w - 1, x)];
    }
    // Color, averaged over each sample's footprint.
    const int fx = g.sx(), fy = g.sy();
    const int cw = pic->cb.width;
    const float n = float(fx * fy);
    float *pcb = pic->cb.p(), *pcr = pic->cr.p();
    for (int cy = 0; cy < pic->cb.height; ++cy) {
        for (int cx = 0; cx < cw; ++cx) {
            float sb = 0, sr = 0;
            for (int dy = 0; dy < fy; ++dy) {
                int yy = std::min(h - 1, cy * fy + dy);
                for (int dx = 0; dx < fx; ++dx) {
                    size_t i = size_t(yy) * w + std::min(w - 1, cx * fx + dx);
                    sb += cbp[i];
                    sr += crp[i];
                }
            }
            pcb[size_t(cy) * cw + cx] = sb / n;
            pcr[size_t(cy) * cw + cx] = sr / n;
        }
    }
    return pic;
}

// ---- pixels out ---------------------------------------------------------------------------------

std::vector<uint8_t> Renderer::render(const Picture &decoded, const Geometry &g, const Settings &settings,
                                      float step, double *deblock_ms) {
    const Picture *picture = &decoded;
    std::optional<Picture> deblocked;
    const double deblocking = settings.effective(kDeblocking);
    if (deblocking > 0) {
        auto t = ProfileClock::now();
        deblocked.emplace(decoded);
        codec::deblock(*deblocked, deblocking, step);
        picture = &*deblocked;
        if (deblock_ms) lap(*deblock_ms, t);
    }
    const int w = g.width, h = g.height;
    std::vector<uint8_t> out(size_t(w) * h * 4, 255);
    const float fx = float(g.sx()), fy = float(g.sy());
    const Plane &cb = picture->cb, &cr = picture->cr, &luma = picture->y;
    uint8_t *o = out.data();
    parallel_for(h, [&](int y) {
        float cyf = (float(y) + 0.5f) / fy - 0.5f;
        int cy0 = int(std::floor(cyf));
        float ty = cyf - float(cy0);
        for (int x = 0; x < w; ++x) {
            float cxf = (float(x) + 0.5f) / fx - 0.5f;
            int cx0 = int(std::floor(cxf));
            float tx = cxf - float(cx0);
            auto bil = [&](const Plane &p) {
                float a = p.clamped(cx0, cy0), b = p.clamped(cx0 + 1, cy0);
                float c = p.clamped(cx0, cy0 + 1), d = p.clamped(cx0 + 1, cy0 + 1);
                return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
            };
            float Y = luma.data[size_t(y) * luma.width + x];
            float u = bil(cb) - 128, v = bil(cr) - 128;
            float r = Y + 1.402f * v;
            float gg = Y - 0.344136f * u - 0.714136f * v;
            float b = Y + 1.772f * u;
            size_t i = (size_t(y) * w + x) * 4;
            o[i] = uint8_t(clampf(std::round(b), 0, 255));
            o[i + 1] = uint8_t(clampf(std::round(gg), 0, 255));
            o[i + 2] = uint8_t(clampf(std::round(r), 0, 255));
            o[i + 3] = 255;
        }
    });
    return out;
}

void Renderer::upscale(const std::vector<uint8_t> &picture, Size size, uint8_t *base, int width, int height,
                       int rowBytes) {
    const int cw = size.width, ch = size.height;
    if (picture.size() != size_t(cw) * ch * 4) return;
    std::vector<int> xs(static_cast<size_t>(width));
    for (int x = 0; x < width; ++x) xs[size_t(x)] = std::min(cw - 1, int((double(x) + 0.5) * double(cw) / double(width)));
    const int *map = xs.data();
    const uint8_t *src = picture.data();
    parallel_for(height, [&](int y) {
        int sy = std::min(ch - 1, int((double(y) + 0.5) * double(ch) / double(height)));
        const uint8_t *srow = src + size_t(sy) * cw * 4;
        uint8_t *drow = base + size_t(y) * rowBytes;
        for (int x = 0; x < width; ++x) std::memcpy(drow + size_t(x) * 4, srow + size_t(map[x]) * 4, 4);
    });
}

}  // namespace ntscrt::digital
