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
// Optional:
//   static size_t bytes(const Pic &);           // memory a picture holds (checkpoint budget)
//   using Pending = <movable>;                  // with the three below: pipelined TV passes
//   int max_inflight();                         // passes that may be begun and not finished (0 = none)
//   Pending tv_begin(const Pic &camera, const HowlPass &pass);
//   Pic tv_end(Pending &&);                     // passes end in the order they were begun
//
// Random access. The picture at frame N is defined as the result of running
// the loop from a window start ("anchor") A(N): with B = window frames,
// A(N) = 0 for N < 2B, else (floor(N/B) - 1) * B — so every frame has at
// least B frames of real history and the first 2B frames are exactly
// upstream's sequential render. At A the run-up builds the tunnel exactly as
// upstream does before frame 0 (standing still on the scene at A). History
// is kept keyed by frame index; asking for N when the history reaches N
// (sequential playback, or re-rendering one of the last few frames)
// continues it; anything else resumes from the closest checkpoint of A(N)'s
// run at or before N, or re-runs the window from A(N). The result for N
// depends only on the settings and the source, never on access order.
// window <= 0: A(N) = 0 always (exact upstream behaviour, unbounded cost).
//
// Checkpoints. The loop's whole state at the start of frame c of A's run is
// the TV pictures of the last `delay` frames and the exposure; pictures are
// immutable, so a checkpoint only holds references to them. One is kept every `checkpoint_every` frames
// of a run (and at the window start, after the run-up), the least recently
// used dropped past `checkpoint_budget` bytes. A run that met a missing past
// source (or window start) took a stand-in, which depends on how it got
// there: no checkpoint is taken from it after that. The owner
// turns checkpoints off (set_checkpoints) while the TV pass keeps state of
// its own between frames (a temporal CRT preset).
//
// Running ahead. Sequential playback crossing a window start W (frame N =
// W + B, whose anchor is W) would re-run the loop from W: B source frames
// fetched again from the host, which (DaVinci Resolve) renders and keeps each
// one. So from frame W on, a second run of anchor W goes along with the one
// shown, on the same source pictures (no fetch of its own), one TV pass per
// frame; at W + B it takes over. It is the run a re-run would make, step for
// step, so the pictures are the same. Off with checkpoints (a temporal TV
// pass), and it never takes over from a history with stand-ins.
//
// Pipelining. With max_inflight() > 0 a TV pass can still be running while
// the next frame's camera is exposed: the camera reads the TV `delay` frames
// back, so with a delay of 2 or more the previous pass needn't be done.
// Passes begin and end in frame order, so the pictures are the same.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <utility>

#include "howl_counter.h"
#include "howl_hooks.h"
#include "howl_math.h"

namespace ntscrt::howl {

constexpr int64_t kDefaultWindow = 240;
constexpr int64_t kRetain = 4;  // extra frames kept so recent frames re-render without a re-run
constexpr int64_t kDefaultCheckpointEvery = 30;

inline int64_t anchor_for(int64_t n, int64_t window) {
    if (window <= 0 || n < 2 * window) return 0;
    return (n / window - 1) * window;
}

// NTSCRT_SCREENLOOP_CHECKPOINT_MB (0 = no checkpoints), default 256 MB:
// about 11 checkpoints of a 6-frame delay with 1280x720 TV pictures.
inline size_t default_checkpoint_budget() {
    if (const char *e = std::getenv("NTSCRT_SCREENLOOP_CHECKPOINT_MB"); e && *e) {
        char *end = nullptr;
        double mb = std::strtod(e, &end);
        if (end != e && mb >= 0) return size_t(mb * 1024 * 1024);
    }
    return size_t(256) << 20;
}

// NTSCRT_SCREENLOOP_AHEAD=0: no run ahead (each window start crossed re-runs its window,
// as 0.5.0-rc.5 did; the pictures are the same either way). For A/B timing.
inline bool default_run_ahead() {
    const char *e = std::getenv("NTSCRT_SCREENLOOP_AHEAD");
    return !(e && std::string(e) == "0");
}

struct EngineConfig {
    Settings settings;
    double fps = 30;
    double length = 5;     // seconds: what a move spans / a loop repeats over
    double aspect = 1;     // scene width / height
    int chain_w = 0, chain_h = 0;  // chain input size (for the run-up plan, blur, feedback size)
    int64_t window = kDefaultWindow;
    int64_t checkpoint_every = kDefaultCheckpointEvery;  // <= 0: none
    size_t checkpoint_budget = default_checkpoint_budget();
    bool run_ahead = default_run_ahead();
};

namespace detail {
template <class O>
struct PendingOf {
    using type = int;
};
template <class O>
    requires requires { typename O::Pending; }
struct PendingOf<O> {
    using type = typename O::Pending;
};
}  // namespace detail

template <class Ops>
class Engine {
public:
    using Pic = typename Ops::Pic;

    explicit Engine(EngineConfig c) : cfg_(std::move(c)) {}
    const EngineConfig &config() const { return cfg_; }

    // What the camera sees at frame N (scene_N = the source at N).
    Pic camera(Ops &ops, int64_t n, const Pic &scene_n) {
        // A failure mid-run (device lost) leaves a partial state: start over next time.
        struct Guard {
            Engine &e;
            bool ok = false;
            ~Guard() {
                if (!ok) {
                    e.inflight_.clear();
                    e.valid_ = false;
                    e.ahead_.drop();
                }
            }
        } guard{*this};
        // A missing past source stands in the last one this call fetched, else
        // the frame asked for. Resumed from a checkpoint at c, the call goes
        // on as the run from the window start would: holding frame c - 1.
        int64_t hold_frame = start(ops, anchor_for(n, cfg_.window), n, scene_n);
        Pic hold;
        while (next_ < n) {
            const int64_t f = next_;
            Pic sc = ops.scene(f);
            const bool got = bool(sc);
            if (sc) {
                hold = sc;
                hold_frame = -1;
            } else {
                complete_ = false;  // stand-ins: what this history shows depends on how it got here
                if (!hold && hold_frame >= 0) hold = ops.scene(hold_frame);
                hold_frame = -1;
                sc = hold ? hold : scene_n;
            }
            Pic cam = camera_step(ops, f, sc, cfg_.settings.delay());
            if (cfg_.settings.tv_in_view()) push_tv(ops, f, cam, normal_pass(f));
            else store(f, Pic());
            step_ahead(ops, f, sc, got);
        }
        drain(ops);
        Pic cam = camera_step(ops, n, scene_n, cfg_.settings.delay());
        step_ahead(ops, n, scene_n, true);
        guard.ok = true;
        return cam;
    }

    // The main path finished frame N: does the loop want its TV picture?
    bool wants_tv(int64_t n) const { return valid_ && n == next_; }
    // Push frame N's TV picture (the main path's chain through the loop's CRT).
    void store_tv(int64_t n, const Pic &tv) {
        if (!wants_tv(n)) return;
        store(n, cfg_.settings.tv_in_view() ? tv : Pic());
    }
    static HowlPass normal_pass(int64_t f, double fps) {
        HowlPass p;
        p.frame = f;
        p.ntsc_frame = p.shader_frame = f + 1;
        p.glitch_time = double(f) / fps;
        return p;
    }
    HowlPass normal_pass(int64_t f) const { return normal_pass(f, cfg_.fps); }

    void reset() {
        inflight_.clear();
        valid_ = false;
        tv_.clear();
        expo_.clear();
        ckpts_.clear();
        ckpt_bytes_ = 0;
        ahead_.drop();
    }
    // Off: drops the checkpoints and neither takes nor uses any until on again
    // (nor runs ahead: both assume a TV pass is a function of its camera picture).
    void set_checkpoints(bool on) {
        if (!on) {
            ckpts_.clear();
            ckpt_bytes_ = 0;
            ahead_.drop();
        }
        ckpt_on_ = on;
    }

    // ---- introspection (tests) ----
    bool valid() const { return valid_; }
    int64_t anchor() const { return anchor_; }
    int64_t next() const { return next_; }
    int64_t tv_passes() const { return tv_passes_; }
    int64_t resets() const { return resets_; }
    int64_t restores() const { return restores_; }
    int64_t ahead_passes() const { return ahead_passes_; }  // TV passes of the run ahead (not in tv_passes)
    int64_t takeovers() const { return takeovers_; }        // window starts crossed without a re-run
    bool ahead_valid() const { return ahead_.valid; }
    int64_t ahead_anchor() const { return ahead_.anchor; }
    int64_t ahead_next() const { return ahead_.next; }
    size_t checkpoints() const { return ckpts_.size(); }
    size_t checkpoints(int64_t anchor) const {  // of that window start's run
        size_t k = 0;
        for (const auto &c : ckpts_) k += c.first.first == anchor;
        return k;
    }
    size_t checkpoint_bytes() const { return ckpt_bytes_; }
    std::optional<double> exposure_at(int64_t f) const {
        auto it = expo_.find(f);
        if (it == expo_.end()) return std::nullopt;
        return it->second;
    }

private:
    static constexpr bool kAsync = requires(Ops &o, const Pic &p, const HowlPass &h) {
        { o.max_inflight() };
        o.tv_end(o.tv_begin(p, h));
    };
    using Pending = typename detail::PendingOf<Ops>::type;

    struct Checkpoint {
        std::map<int64_t, Pic> tv;  // frames c - delay .. c - 1
        double exposure = 1;
        size_t bytes = 0;
        uint64_t used = 0;
    };
    using CheckpointMap = std::map<std::pair<int64_t, int64_t>, Checkpoint>;  // by (anchor, frame c)

    EngineConfig cfg_;
    bool valid_ = false;
    bool complete_ = true;  // no stand-in source in this run so far: checkpoints may be taken
    int64_t anchor_ = 0, next_ = 0;
    std::map<int64_t, Pic> tv_;       // TV picture shown at frame f (run-up frames: f < anchor)
    std::map<int64_t, double> expo_;  // exposure at the start of frame f
    std::deque<std::pair<int64_t, Pending>> inflight_;  // TV passes begun, not finished (frame order)
    CheckpointMap ckpts_;
    size_t ckpt_bytes_ = 0;
    uint64_t ckpt_tick_ = 0;
    bool ckpt_on_ = true;
    CounterBitmap counter_;
    int64_t tv_passes_ = 0, resets_ = 0, restores_ = 0;

    // The run ahead (see "Running ahead"): the same state as the members above,
    // swapped in while it steps.
    struct Run {
        bool valid = false, complete = true;
        int64_t anchor = 0, next = 0;
        std::map<int64_t, Pic> tv;
        std::map<int64_t, double> expo;
        std::deque<std::pair<int64_t, Pending>> inflight;
        void drop() {
            valid = false;
            tv.clear();
            expo.clear();
            inflight.clear();
        }
    };
    Run ahead_;
    bool in_ahead_ = false;  // the run ahead is swapped in: TV passes synchronous
    int64_t ahead_passes_ = 0, takeovers_ = 0;

    void swap_run() {
        std::swap(valid_, ahead_.valid);
        std::swap(complete_, ahead_.complete);
        std::swap(anchor_, ahead_.anchor);
        std::swap(next_, ahead_.next);
        std::swap(tv_, ahead_.tv);
        std::swap(expo_, ahead_.expo);
        std::swap(inflight_, ahead_.inflight);
    }

    // Frame f's source picture `sc` (got: the host's, not a stand-in) was just
    // used by the run shown: the run of the next window start takes its step
    // too, or starts there (its run-up stands on sc, so it fetches nothing).
    void step_ahead(Ops &ops, int64_t f, const Pic &sc, bool got) {
        const int64_t b = cfg_.window;
        if (b <= 0 || !cfg_.run_ahead || !ckpt_on_ || !sc) return;
        const int64_t a2 = f / b * b;  // the anchor of frames a2 + b .. a2 + 2b - 1
        if (a2 < b || a2 <= anchor_) return;
        const bool cont = ahead_.valid && ahead_.anchor == a2 && ahead_.next == f;
        if (!cont && f != a2) return;
        struct Back {
            Engine &e;
            int64_t passes, resets;
            bool ok = false;
            ~Back() {
                e.ahead_passes_ += e.tv_passes_ - passes;
                e.tv_passes_ = passes;
                e.resets_ = resets;
                e.in_ahead_ = false;
                e.swap_run();
                if (!ok) e.ahead_.drop();
            }
        };
        swap_run();
        in_ahead_ = true;
        Back back{*this, tv_passes_, resets_};
        if (!cont) reset(ops, a2, sc, got);
        else if (!got) complete_ = false;
        Pic cam = camera_step(ops, f, sc, cfg_.settings.delay());
        if (cfg_.settings.tv_in_view()) store(f, tv_pass(ops, cam, normal_pass(f)));
        else store(f, Pic());
        back.ok = true;
    }

    static size_t pic_bytes(const Pic &p) {
        if constexpr (requires { Ops::bytes(p); }) return p ? size_t(Ops::bytes(p)) : 0;
        else return 0;
    }

    bool can_reach(int64_t a, int64_t n) const {
        if (!valid_ || a != anchor_) return false;
        const int d = cfg_.settings.delay();
        const int64_t from = n <= next_ ? n : next_;  // first frame we must render
        if (!expo_.count(from)) return false;
        if (cfg_.settings.tv_in_view() && !tv_.count(from - d)) return false;
        return true;
    }

    // Brings the state to a frame <= n of anchor a's run, the cheapest way.
    // Returns the source frame a missing next one stands in for (-1: none).
    int64_t start(Ops &ops, int64_t a, int64_t n, const Pic &scene_n) {
        constexpr int64_t kNever = std::numeric_limits<int64_t>::max();
        const int64_t cont = can_reach(a, n) ? (n > next_ ? n - next_ : 0) : kNever;
        if (cont == kNever && ckpt_on_ && ahead_.valid && ahead_.complete && ahead_.anchor == a && ahead_.next == n) {
            // Crossing into the window the run ahead started: it takes over.
            swap_run();
            ahead_.drop();
            inflight_.clear();
            ++takeovers_;
            return -1;
        }
        if (ckpt_on_ && !ckpts_.empty()) {
            auto it = ckpts_.upper_bound({a, n});  // the first after (a, n)
            if (it != ckpts_.begin()) {
                --it;
                if (it->first.first == a && n - it->first.second < cont) {
                    restore(it);
                    return next_ > a ? next_ - 1 : -1;
                }
            }
        }
        if (cont == kNever) {
            Pic scene_a = n == a ? scene_n : ops.scene(a);
            reset(ops, a, scene_a ? scene_a : scene_n, bool(scene_a));
        }
        return -1;
    }

    void restore(typename CheckpointMap::iterator it) {
        ++restores_;
        inflight_.clear();
        Checkpoint &c = it->second;
        c.used = ++ckpt_tick_;
        anchor_ = it->first.first;
        next_ = it->first.second;
        tv_ = c.tv;
        expo_.clear();
        expo_[next_] = c.exposure;
        complete_ = true;
        valid_ = true;
    }

    // At the start of frame next_ of a run: keep the state if it's due.
    bool checkpoint_due() const {
        return ckpt_on_ && complete_ && cfg_.checkpoint_every > 0 && cfg_.checkpoint_budget > 0 &&
               (next_ - anchor_) % cfg_.checkpoint_every == 0 && !ckpts_.count({anchor_, next_});
    }
    void take_checkpoint() {
        const int64_t c = next_;
        auto e = expo_.find(c);
        if (e == expo_.end()) return;
        Checkpoint ck;
        ck.exposure = e->second;
        if (cfg_.settings.tv_in_view()) {
            const int d = cfg_.settings.delay();
            for (int64_t f = c - d; f < c; ++f) {
                auto t = tv_.find(f);
                if (t == tv_.end()) return;
                ck.tv[f] = t->second;
                ck.bytes += pic_bytes(t->second);
            }
        }
        ck.used = ++ckpt_tick_;
        ckpt_bytes_ += ck.bytes;
        ckpts_.emplace(std::pair<int64_t, int64_t>{anchor_, c}, std::move(ck));
        while (ckpt_bytes_ > cfg_.checkpoint_budget && !ckpts_.empty()) {
            auto lru = ckpts_.begin();
            for (auto i = ckpts_.begin(); i != ckpts_.end(); ++i)
                if (i->second.used < lru->second.used) lru = i;
            ckpt_bytes_ -= lru->second.bytes;
            ckpts_.erase(lru);
        }
    }

    Pic tv_pass(Ops &ops, const Pic &cam, const HowlPass &p) {
        ++tv_passes_;
        return ops.tv_pass(cam, p);
    }

    // Begins a TV pass for frame f if passes can overlap; false = do it now.
    bool begin_tv(Ops &ops, int64_t f, const Pic &cam, const HowlPass &p) {
        if constexpr (kAsync) {
            const int limit = ops.max_inflight();
            if (limit <= 0 || cfg_.settings.delay() < 2 || in_ahead_) return false;
            while (int(inflight_.size()) >= limit) finish_one(ops);
            ++tv_passes_;
            inflight_.emplace_back(f, ops.tv_begin(cam, p));
            return true;
        } else {
            (void)ops, (void)f, (void)cam, (void)p;
            return false;
        }
    }
    void finish_one(Ops &ops) {
        if constexpr (kAsync) {
            auto [f, pend] = std::move(inflight_.front());
            inflight_.pop_front();
            tv_[f] = ops.tv_end(std::move(pend));
        } else {
            (void)ops;
        }
    }
    void drain(Ops &ops) {
        while (!inflight_.empty()) finish_one(ops);
    }
    // The TV picture shown at frame f (finishing the passes up to it).
    Pic tv_at(Ops &ops, int64_t f) {
        while (!inflight_.empty() && inflight_.front().first <= f) finish_one(ops);
        auto it = tv_.find(f);
        return it != tv_.end() ? it->second : Pic();
    }

    void prune() {
        const int64_t keep = next_ - cfg_.settings.delay() - kRetain;
        tv_.erase(tv_.begin(), tv_.lower_bound(keep));
        expo_.erase(expo_.begin(), expo_.lower_bound(keep));
    }

    void store(int64_t f, const Pic &tv) {
        tv_[f] = tv;
        next_ = f + 1;
        prune();
        if (checkpoint_due()) take_checkpoint();
    }

    // History frame f's TV pass, possibly overlapping the next camera.
    void push_tv(Ops &ops, int64_t f, const Pic &cam, const HowlPass &p) {
        if (!begin_tv(ops, f, cam, p)) {
            store(f, tv_pass(ops, cam, p));
            return;
        }
        next_ = f + 1;
        prune();  // passes in flight are newer than anything pruned
        if (checkpoint_due()) {
            drain(ops);
            take_checkpoint();
        }
    }

    // One camera exposure at frame f, reading the TV `lag` frames back.
    Pic camera_step(Ops &ops, int64_t f, const Pic &scene, int lag) {
        const Settings &s = cfg_.settings;
        Pic in = tv_at(ops, f - lag);
        auto eit = expo_.find(f);
        const double e = eit != expo_.end() ? eit->second : 1.0;
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
    // scene_is_a: `scene` is the source at a (else a stand-in: no checkpoints).
    void reset(Ops &ops, int64_t a, const Pic &scene, bool scene_is_a) {
        ++resets_;
        inflight_.clear();
        tv_.clear();
        expo_.clear();
        valid_ = false;
        const Settings &s = cfg_.settings;
        const auto plan = s.run_up_plan(cfg_.aspect, cfg_.chain_h);
        const int frames = plan.build + plan.settle;
        const int d = s.delay();
        expo_[a - frames] = 1.0;
        for (int k = 0; k < frames; ++k) {
            const int64_t v = a - frames + k;
            if (k == plan.build && plan.build > 0 && d > 1) {
                // fillHistory: the TV has shown the finished tunnel all along.
                Pic latest = tv_at(ops, v - 1);
                for (int j = 1; j <= d; ++j) tv_[v - j] = latest;
            }
            const int lag = k < plan.build ? 1 : d;
            Pic cam = camera_step(ops, v, scene, lag);
            HowlPass p;
            p.frame = a;
            p.run_up = true;
            p.ntsc_frame = p.shader_frame = 100000 + k;
            p.glitch_time = double(a) / cfg_.fps;
            // While building, the next trip reads this one (lag 1): no overlap.
            if (k < plan.build || !begin_tv(ops, v, cam, p)) tv_[v] = tv_pass(ops, cam, p);
        }
        drain(ops);
        anchor_ = a;
        next_ = a;
        complete_ = scene_is_a;
        valid_ = true;
        const int64_t keep = a - d - kRetain;
        tv_.erase(tv_.begin(), tv_.lower_bound(keep));
        expo_.erase(expo_.begin(), expo_.lower_bound(keep));
        if (checkpoint_due()) take_checkpoint();
    }
};

}  // namespace ntscrt::howl
