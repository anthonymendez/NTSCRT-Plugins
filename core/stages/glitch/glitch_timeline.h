// glitch_timeline.h — port of upstream GlitchRenderer.swift's simulation side,
// adapted to a host that may ask for frames in any order.
//
// The receiver's state at frame F is a pure function of (raster, seed, the
// settings at frames 0..F): the set warms up from t = -2 s with frame 0's
// settings, then advances frame by frame to t = k/fps with frame k's settings
// (ctx.look_at(k)) — upstream's resimulate(settingsAt:) with
// settingsAt(t) = look at round(t*fps). Rendering frame after frame continues
// the live simulation; any other request resumes from the nearest checkpoint
// (one per simulated second) so seeking gives the same result as playing.
//
// Cache validity: the per-frame timing settings each checkpoint was built from
// are remembered (as hashes). Before resuming, the resume frame itself and up
// to kProbes earlier recorded frames are re-read through ctx.look_at and
// compared; the first mismatch drops everything from there on. Edits that
// change only frames between probes can go unnoticed until flush(), so hosts
// should flush on parameter edits (OFX instanceChanged / FxPlug).
// Owner: agent B.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include "../../src/internal.h"
#include "receiver.h"

namespace ntscrt::glitch {

// Seeds: ctx.seed == 0 keeps upstream's constants exactly.
uint64_t receiver_seed(uint64_t ctx_seed);
uint32_t noise_seed(uint64_t ctx_seed);

class GlitchTimeline {
public:
    static constexpr size_t kMaxCheckpointBytes = size_t(128) << 20;
    static constexpr int kProbes = 16;

    // Brings the receiver to ctx.frame for a `width` x `height` chain input
    // and returns the field it displays.
    const GlitchFieldPlan &plan_for(const FrameContext &ctx, int width, int height);
    void flush();

    const GlitchFieldPlan &last_plan() const { return plan_; }

    // Introspection (tests).
    size_t checkpoint_count() const { return checkpoints_.size(); }
    size_t checkpoint_bytes() const { return ckpt_bytes_; }
    int64_t steps_frames_last() const { return frames_simulated_last_; }

private:
    void invalidate_from(int64_t frame);
    void add_checkpoint(int64_t frame);

    std::unique_ptr<ReceiverSimulator> sim_;
    uint64_t seed_ = 0;
    double fps_ = 0;
    int64_t sim_frame_ = -1;              // frame the live simulation sits at (-1: none)
    std::vector<uint64_t> hist_;          // timing hash per frame 0..hist_.size()-1
    std::map<int64_t, ReceiverSimulator::Snapshot> checkpoints_;
    size_t ckpt_bytes_ = 0;
    int64_t frames_simulated_last_ = 0;
    GlitchFieldPlan plan_;
};

}  // namespace ntscrt::glitch
