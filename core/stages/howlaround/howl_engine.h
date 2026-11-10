// howl_engine.h — the Screen Loop's sequencing (upstream HowlaroundLoop:
// ring, lag, runUp, fillHistory, advance) made random-access, independent of
// GPU vs CPU. `Ops` does the pixel work:
//
//   using Pic = <nullable shared handle to an immutable picture>;
//   Pic scene(int64_t f);                       // source frame f, or null
//   Pic camera(const Pic &scene, const Pic &tv /*null = dark TV*/,
//              const CameraUniforms &u, double sigma /*0 = no blur*/,
//              const CounterBitmap *counter /*null = none*/,
//              bool measure, double &scene_luma, double &camera_luma);
//   Pic tv_pass(const Pic &camera, const HowlPass &pass);   // camera -> TV picture
//
// Random access. The picture at frame N is defined as the result of running
// the loop from a window start ("anchor") A(N): with B = window frames,
// A(N) = 0 for N < 2B, else (floor(N/B) - 1) * B — so every frame has at
// least B frames of real history and the first 2B frames are exactly
// upstream's sequential render. At A the run-up builds the tunnel exactly as
// upstream does before frame 0 (standing still on the scene at A). History
// is kept keyed by frame index; asking for N when the history reaches N
// (sequential playback, or re-rendering one of the last few frames)
// continues it, anything else re-runs the window from A(N). The result for N
// depends only on the settings and the source, never on access order.
// window <= 0: A(N) = 0 always (exact upstream behaviour, unbounded cost).
#pragma once

#include <cstdint>
#include <map>
#include <optional>

#include "howl_counter.h"
#include "howl_hooks.h"
#include "howl_math.h"

namespace ntscrt::howl {

constexpr int64_t kDefaultWindow = 240;
constexpr int64_t kRetain = 4;  // extra frames kept so recent frames re-render without a re-run

inline int64_t anchor_for(int64_t n, int64_t window) {
    if (window <= 0 || n < 2 * window) return 0;
    return (n / window - 1) * window;
}

struct EngineConfig {
    Settings settings;
    double fps = 30;
    double length = 5;     // seconds: what a move spans / a loop repeats over
    double aspect = 1;     // scene width / height
    int chain_w = 0, chain_h = 0;  // chain input size (for the run-up plan, blur, feedback size)
    int64_t window = kDefaultWindow;
};

template <class Ops>
class Engine {
public:
    using Pic = typename Ops::Pic;

    explicit Engine(EngineConfig c) : cfg_(std::move(c)) {}
    const EngineConfig &config() const { return cfg_; }

    // What the camera sees at frame N (scene_N = the source at N).
    Pic camera(Ops &ops, int64_t n, const Pic &scene_n) {
        const int64_t a = anchor_for(n, cfg_.window);
        if (!can_reach(a, n)) {
            Pic scene_a = n == a ? scene_n : ops.scene(a);
            reset(ops, a, scene_a ? scene_a : scene_n);
        }
        Pic hold;
        while (next_ < n) {
            Pic sc = ops.scene(next_);
            if (sc) hold = sc;
            else sc = hold ? hold : scene_n;
            Pic cam = camera_step(ops, next_, sc, cfg_.settings.delay());
            store(next_, cfg_.settings.tv_in_view() ? tv_pass(ops, cam, normal_pass(next_)) : Pic());
        }
        return camera_step(ops, n, scene_n, cfg_.settings.delay());
    }

    // The main path finished frame N: does the loop want its TV picture?
    bool wants_tv(int64_t n) const { return valid_ && n == next_; }
    // Push frame N's TV picture (the main path's chain through the loop's CRT).
    void store_tv(int64_t n, const Pic &tv) {
        if (wants_tv(n)) store(n, cfg_.settings.tv_in_view() ? tv : Pic());
    }
    static HowlPass normal_pass(int64_t f, double fps) {
        HowlPass p;
        p.frame = f;
        p.ntsc_frame = p.shader_frame = f + 1;
        p.glitch_time = double(f) / fps;
        return p;
    }
    HowlPass normal_pass(int64_t f) const { return normal_pass(f, cfg_.fps); }

    void reset() { valid_ = false; tv_.clear(); expo_.clear(); }

    // ---- introspection (tests) ----
    bool valid() const { return valid_; }
    int64_t anchor() const { return anchor_; }
    int64_t next() const { return next_; }
    int64_t tv_passes() const { return tv_passes_; }
    int64_t resets() const { return resets_; }
    std::optional<double> exposure_at(int64_t f) const {
        auto it = expo_.find(f);
        if (it == expo_.end()) return std::nullopt;
        return it->second;
    }

private:
    EngineConfig cfg_;
    bool valid_ = false;
    int64_t anchor_ = 0, next_ = 0;
    std::map<int64_t, Pic> tv_;       // TV picture shown at frame f (run-up frames: f < anchor)
    std::map<int64_t, double> expo_;  // exposure at the start of frame f
    CounterBitmap counter_;
    int64_t tv_passes_ = 0, resets_ = 0;

    bool can_reach(int64_t a, int64_t n) const {
        if (!valid_ || a != anchor_) return false;
        const int d = cfg_.settings.delay();
        const int64_t from = n <= next_ ? n : next_;  // first frame we must render
        if (!expo_.count(from)) return false;
        if (cfg_.settings.tv_in_view() && !tv_.count(from - d)) return false;
        return true;
    }

    Pic tv_pass(Ops &ops, const Pic &cam, const HowlPass &p) {
        ++tv_passes_;
        return ops.tv_pass(cam, p);
    }

    void store(int64_t f, const Pic &tv) {
        tv_[f] = tv;
        next_ = f + 1;
        const int64_t keep = next_ - cfg_.settings.delay() - kRetain;
        tv_.erase(tv_.begin(), tv_.lower_bound(keep));
        expo_.erase(expo_.begin(), expo_.lower_bound(keep));
    }

    // One camera exposure at frame f, reading the TV `lag` frames back.
    Pic camera_step(Ops &ops, int64_t f, const Pic &scene, int lag) {
        const Settings &s = cfg_.settings;
        auto eit = expo_.find(f);
        const double e = eit != expo_.end() ? eit->second : 1.0;
        Pic in;
        if (auto it = tv_.find(f - lag); it != tv_.end()) in = it->second;
        const double time = double(f) / cfg_.fps;
        const Pose pose = s.pose(pose_time(s, time, cfg_.length), cfg_.length);
        const CounterBitmap *counter = nullptr;
        if (s.counter()) {
            std::string text = counter_text(time);
            if (text != counter_.text || counter_.width == 0) counter_ = render_counter(text);
            counter = &counter_;
        }
        CameraUniforms u = camera_uniforms(s, pose, cfg_.aspect, e, counter ? counter->aspect() : 0);
        double sigma = 0;
        if (s.tv_in_view() && in) {
            Size fb = feedback_size(cfg_.chain_w, cfg_.chain_h);
            double sg = blur_sigma(s, pose, fb.height, cfg_.chain_h);
            if (sg > 0.3) sigma = sg;
        }
        const double ae = s["auto_exposure"];
        const bool measure = ae > 0;
        double scene_luma = 0, camera_luma = 0;
        Pic cam = ops.camera(scene, in, u, sigma, counter, measure, scene_luma, camera_luma);
        // The exposure reacts to what it measured a frame late (upstream advance()).
        expo_[f + 1] = measure ? exposure_step(e, scene_luma, camera_luma, ae) : e;
        return cam;
    }

    // Upstream HowlaroundLoop.runUp, standing still on `scene` at the window
    // start `a`: `build` trips at lag 1, then `settle` at the real delay.
    void reset(Ops &ops, int64_t a, const Pic &scene) {
        ++resets_;
        tv_.clear();
        expo_.clear();
        const Settings &s = cfg_.settings;
        const auto plan = s.run_up_plan(cfg_.aspect, cfg_.chain_h);
        const int frames = plan.build + plan.settle;
        const int d = s.delay();
        expo_[a - frames] = 1.0;
        for (int k = 0; k < frames; ++k) {
            const int64_t v = a - frames + k;
            if (k == plan.build && plan.build > 0 && d > 1) {
                // fillHistory: the TV has shown the finished tunnel all along.
                Pic latest = tv_[v - 1];
                for (int j = 1; j <= d; ++j) tv_[v - j] = latest;
            }
            const int lag = k < plan.build ? 1 : d;
            Pic cam = camera_step(ops, v, scene, lag);
            HowlPass p;
            p.frame = a;
            p.run_up = true;
            p.ntsc_frame = p.shader_frame = 100000 + k;
            p.glitch_time = double(a) / cfg_.fps;
            tv_[v] = tv_pass(ops, cam, p);
        }
        anchor_ = a;
        next_ = a;
        valid_ = true;
        const int64_t keep = a - d - kRetain;
        tv_.erase(tv_.begin(), tv_.lower_bound(keep));
        expo_.erase(expo_.begin(), expo_.lower_bound(keep));
    }
};

}  // namespace ntscrt::howl
