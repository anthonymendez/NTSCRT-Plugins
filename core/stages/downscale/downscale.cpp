// Wave-0 stub. Owner: agent D. Replace with a port of upstream Downscaler.swift.
#include "../stages.h"
namespace ntscrt {
struct Downscaler::Impl {};
Downscaler::Downscaler(gpu::Device &) : impl_(std::make_unique<Impl>()) {}
Downscaler::~Downscaler() = default;
std::shared_ptr<gpu::Texture> Downscaler::run(const std::shared_ptr<gpu::Texture> &in, const FrameContext &) { return in; }
}
