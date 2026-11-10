// glitch_cpu.h — the whole Glitch stage on the CPU (no GPU needed): receiver
// simulation (glitch_timeline.h) + CPU reference of the three GPU kernels
// (glitch_kernels.h). Same results as GlitchStage::run up to GPU float
// rounding; pixel-identical to the input for a healthy set.
// Owner: agent B.
#pragma once

#include "../../src/internal.h"
#include "glitch_kernels.h"
#include "glitch_timeline.h"

namespace ntscrt::glitch {

// look["glitch"]["enabled"]
bool glitch_active(const json &look);

class GlitchCpuStage {
public:
    bool active(const json &look) const { return glitch_active(look); }
    // In place; `inout` is the chain input (after NTSC + downscale). Runs even
    // when inactive (callers check active()).
    void process(ImageBGRA8 &inout, const FrameContext &ctx);
    // The field drawn by the last process() call.
    const GlitchFieldPlan &last_plan() const { return timeline_.last_plan(); }
    GlitchTimeline &timeline() { return timeline_; }
    void flush() { timeline_.flush(); }

private:
    GlitchTimeline timeline_;
};

}  // namespace ntscrt::glitch
