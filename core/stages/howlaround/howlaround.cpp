// Wave-0 stub. Owner: agent D. Replace with a port of upstream Howlaround.swift / HowlaroundPad.swift.
#include "../stages.h"
namespace ntscrt {
struct Howlaround::Impl {};
Howlaround::Howlaround(gpu::Device &) : impl_(std::make_unique<Impl>()) {}
Howlaround::~Howlaround() = default;
bool Howlaround::active(const json *fb) const { return fb && fb->value("enabled", false); }
std::shared_ptr<gpu::Texture> Howlaround::camera(const std::shared_ptr<gpu::Texture> &s, const json &, const FrameContext &) { return s; }
void Howlaround::after_frame(const gpu::Texture &, const json &, const FrameContext &) {}
void Howlaround::flush() {}
}
