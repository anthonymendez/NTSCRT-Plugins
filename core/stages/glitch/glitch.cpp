// Wave-0 stub. Owner: agent B. Replace with ports of Receiver.swift, GlitchRenderer.swift, GlitchStage.swift.
#include "../stages.h"
namespace ntscrt {
struct GlitchStage::Impl {};
GlitchStage::GlitchStage(gpu::Device &) : impl_(std::make_unique<Impl>()) {}
GlitchStage::~GlitchStage() = default;
bool GlitchStage::active(const json &look) const {
    return look.contains("glitch") && look["glitch"].value("enabled", false);
}
std::shared_ptr<gpu::Texture> GlitchStage::run(const std::shared_ptr<gpu::Texture> &in, const FrameContext &) { return in; }
void GlitchStage::flush() {}
}
