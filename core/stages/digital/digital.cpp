// Wave-0 stub. Owner: agent C. Replace with a port of upstream Sources/CrtCodec.
#include "../stages.h"
namespace ntscrt {
struct DigitalStage::Impl {};
DigitalStage::DigitalStage() : impl_(std::make_unique<Impl>()) {}
DigitalStage::~DigitalStage() = default;
bool DigitalStage::active(const json &look) const {
    return look.contains("digital") && look["digital"].value("enabled", false);
}
void DigitalStage::process(ImageBGRA8 &, const FrameContext &) {}
void DigitalStage::flush() {}
}
