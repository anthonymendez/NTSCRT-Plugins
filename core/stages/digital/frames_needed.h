// frames_needed.h — the earliest source frame the Digital video stage can read
// for a frame (ntscrt_source_frames_needed, for hosts that must declare the
// frames an effect fetches: OpenFX getFramesNeeded, FxPlug scheduleInputs).
#pragma once

#include <cstdint>

#include <nlohmann/json.hpp>

namespace ntscrt::digital {

// Mirrors Renderer::rerun: the stream restarts at the last real keyframe at or
// before frame n's capture (walking back over keyframes a datamosh drops), or
// at the capture before n when every frame is a keyframe. Uses the settings of
// `look` for every frame (keyframed Keyframe interval / Datamosh / Frame rate
// values at earlier frames may move the start). Returns n when the stage is off.
int64_t first_source_frame(const nlohmann::json &look, double fps, int64_t n, uint64_t seed);

}  // namespace ntscrt::digital
