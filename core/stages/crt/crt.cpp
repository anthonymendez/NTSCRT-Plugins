// Wave-0 stub. Owner: agent D. Replace with librashader + ShaderBypass port.
#include "../stages.h"
namespace ntscrt {
struct CrtStage::Impl { gpu::Device *dev; };
CrtStage::CrtStage(gpu::Device &d, const std::string &) : impl_(std::make_unique<Impl>(Impl{&d})) {}
CrtStage::~CrtStage() = default;
void CrtStage::render(const gpu::Texture &in, gpu::Texture &out, const FrameContext &) {
    if (in.desc().width == out.desc().width && in.desc().height == out.desc().height) impl_->dev->copy(in, out);
}
void CrtStage::flush() {}
}
