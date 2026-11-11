// glitch_timeline.cpp — see glitch_timeline.h.
#include "glitch_timeline.h"

#include <algorithm>
#include <cmath>
#include <optional>

#include "glitch_kernels.h"

namespace ntscrt::glitch {

uint64_t receiver_seed(uint64_t ctx_seed) {
    return ctx_seed == 0 ? ReceiverSimulator::kDefaultSeed
                         : ReceiverSimulator::kDefaultSeed ^ GlitchRandom::mix(ctx_seed);
}

uint32_t noise_seed(uint64_t ctx_seed) {
    return ctx_seed == 0 ? kDefaultNoiseSeed
                         : kDefaultNoiseSeed ^ uint32_t(GlitchRandom::mix(ctx_seed ^ 0x6E6F697365ull) >> 32);
}

void GlitchTimeline::flush() {
    sim_.reset();
    sim_frame_ = -1;
    hist_.clear();
    checkpoints_.clear();
    ckpt_bytes_ = 0;
    frames_simulated_last_ = 0;
}

void GlitchTimeline::invalidate_from(int64_t frame) {
    if (frame < 0) frame = 0;
    if (int64_t(hist_.size()) > frame) hist_.resize(size_t(frame));
    for (auto it = checkpoints_.lower_bound(frame); it != checkpoints_.end();) {
        ckpt_bytes_ -= it->second.bytes();
        it = checkpoints_.erase(it);
    }
    if (sim_frame_ >= frame) sim_frame_ = -1;
}

void GlitchTimeline::add_checkpoint(int64_t frame) {
    auto snap = sim_->snapshot();
    ckpt_bytes_ += snap.bytes();
    checkpoints_[frame] = std::move(snap);
    if (ckpt_bytes_ <= kMaxCheckpointBytes || checkpoints_.size() < 4) return;
    // Over budget: thin to every other checkpoint (keeping the first and the
    // newest), halving the density instead of forgetting a stretch of time.
    int64_t newest = checkpoints_.rbegin()->first;
    bool keep = true;
    for (auto it = checkpoints_.begin(); it != checkpoints_.end();) {
        if (!keep && it->first != newest) {
            ckpt_bytes_ -= it->second.bytes();
            it = checkpoints_.erase(it);
        } else {
            ++it;
        }
        keep = !keep;
    }
}

const GlitchFieldPlan &GlitchTimeline::plan_for(const FrameContext &ctx, int width, int height) {
    ReceiverRaster raster(width, height);
    uint64_t seed = receiver_seed(ctx.seed);
    double fps = ctx.fps > 0 ? ctx.fps : 30.0;
    if (!sim_ || sim_->raster() != raster || seed_ != seed || fps_ != fps) {
        flush();
        sim_ = std::make_unique<ReceiverSimulator>(raster, seed);
        seed_ = seed;
        fps_ = fps;
    }
    const int64_t F = std::max<int64_t>(0, ctx.frame);

    // Settings at frame k: the host's keyframes, read once per call.
    // Frames whose look is this frame's own (no keyframes: all of them) share
    // one parse.
    std::map<int64_t, GlitchSettings> memo;
    std::optional<GlitchSettings> current;
    auto settings_at = [&](int64_t k) -> const GlitchSettings & {
        auto it = memo.find(k);
        if (it != memo.end()) return it->second;
        const json *j = k == ctx.frame && ctx.look ? ctx.look : ctx.look_at ? &ctx.look_at(k) : ctx.look;
        GlitchSettings s;
        if (j && j == ctx.look) {
            if (!current) current = GlitchSettings::from_look(*ctx.look);
            s = *current;
        } else if (j) {
            s = GlitchSettings::from_look(*j);
        }
        return memo.emplace(k, s).first->second;
    };

    // The latest valid state at or before F: the live simulation or a checkpoint.
    auto pick = [&]() -> int64_t {
        int64_t best = -1;
        if (sim_frame_ >= 0 && sim_frame_ <= F) best = sim_frame_;
        auto it = checkpoints_.upper_bound(F);
        if (it != checkpoints_.begin()) {
            --it;
            best = std::max(best, it->first);
        }
        return best;
    };

    // Verify the history that state was built from still holds.
    for (;;) {
        int64_t c = pick();
        if (c < 0) break;
        std::vector<int64_t> probes;
        probes.push_back(c);
        if (c > 0) {
            probes.push_back(c - 1);
            // Spread over 0..c-1, on whole seconds: the same frames from one
            // render to the next, so a host that caches looks per frame
            // builds no new ones while playing.
            for (int i = 0; i < kProbes; i++) {
                const double f = double((c - 1) * i / (kProbes - 1));
                probes.push_back(std::min<int64_t>(c - 1, std::llround(std::floor(f / fps) * fps)));
            }
        }
        std::sort(probes.begin(), probes.end());
        probes.erase(std::unique(probes.begin(), probes.end()), probes.end());
        int64_t bad = -1;
        for (int64_t p : probes) {
            if (p >= int64_t(hist_.size()) || hist_[size_t(p)] != settings_at(p).timing_hash()) {
                bad = p;
                break;
            }
        }
        if (bad < 0) break;
        invalidate_from(bad);
    }

    auto record = [&](int64_t k, const GlitchSettings &s) {
        uint64_t h = s.timing_hash();
        if (k < int64_t(hist_.size())) {
            if (hist_[size_t(k)] == h) return;
            invalidate_from(k);
        }
        hist_.push_back(h);  // k == hist_.size() here
    };

    int64_t c = pick();
    if (c < 0) {
        // Fresh set: warm up from t = -2 s to time zero under frame 0's settings.
        sim_->reset();
        const GlitchSettings &s0 = settings_at(0);
        sim_->advance(0, s0);
        record(0, s0);
        sim_frame_ = 0;
        add_checkpoint(0);
        c = 0;
    } else if (c != sim_frame_) {
        sim_->restore(checkpoints_.at(c));
        sim_frame_ = c;
    }
    frames_simulated_last_ = F - c;
    for (int64_t k = c + 1; k <= F; k++) {
        const GlitchSettings &s = settings_at(k);
        sim_->advance(double(k) / fps, s);
        record(k, s);
        sim_frame_ = k;
        auto it = checkpoints_.upper_bound(k);
        int64_t prev = it == checkpoints_.begin() ? -1 : std::prev(it)->first;
        if (prev != k && (prev < 0 || std::floor(double(k) / fps) > std::floor(double(prev) / fps)))
            add_checkpoint(k);
        if (memo.size() > 256) memo.clear();
    }
    // Knobs for drawing: frame F's full settings (ghost, captions, dropouts...).
    // The simulation already sits past t_F, so this steps nothing.
    sim_->advance(double(F) / fps, settings_at(F));
    plan_ = sim_->plan();
    return plan_;
}

}  // namespace ntscrt::glitch
