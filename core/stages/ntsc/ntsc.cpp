// Wave-0 stub. Owner: agent A. Replace with ntsc-rs via rust/ntscrs-capi.
#include "../stages.h"
namespace ntscrt {
struct NtscStage::Impl {};
NtscStage::NtscStage() : impl_(std::make_unique<Impl>()) {}
NtscStage::~NtscStage() = default;
bool NtscStage::active(const json &look) const {
    return look.contains("ntsc") && look["ntsc"].value("enabled", false);
}
void NtscStage::process(ImageBGRA8 &, const FrameContext &) {}
std::string NtscStage::descriptors_json() { return "[]"; }
}
