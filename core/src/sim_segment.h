// sim_segment.h — where the stages that simulate a history (the Glitch receiver,
// Digital video's stream) start it from.
//
// Upstream simulates from the start of the clip. A host's clip can be hours long, and
// some hosts (DaVinci Resolve on an Adjustment Clip) don't say where the clip starts, so
// frame numbers are timeline frames an hour or more in. Re-simulating from frame 0 after
// every edit then costs seconds to minutes. So the history restarts every
// kSimSegmentSeconds (in whole seconds of the rounded frame rate, so an hour or a
// minute of timecode is always a segment boundary): a frame's picture depends only on the
// frames since its segment's start, which is the upstream behaviour for the first
// segment (frames 0 .. kSimSegmentSeconds of the effect) and bounds what an edit costs
// anywhere else. Deterministic: the same frame gives the same result in any render order.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ntscrt {

constexpr int64_t kSimSegmentSeconds = 60;

// Frames in one segment at `fps`.
inline int64_t sim_segment_frames(double fps) {
    const int64_t r = fps > 0 ? int64_t(std::llround(fps)) : 30;
    return kSimSegmentSeconds * std::max<int64_t>(1, r);
}

// First frame of the segment holding frame n (n < 0: 0).
inline int64_t sim_segment_start(int64_t n, double fps) {
    if (n <= 0) return 0;
    const int64_t w = sim_segment_frames(fps);
    return n / w * w;
}

}  // namespace ntscrt
