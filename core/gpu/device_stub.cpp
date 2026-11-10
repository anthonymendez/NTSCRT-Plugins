// Wave-0 stub. Owner: agent D. Replace with core/gpu/d3d11/*.cpp and
// core/gpu/metal/*.mm, and remove this file from core/CMakeLists.txt.
#include "gpu.h"
namespace ntscrt::gpu {
std::unique_ptr<Device> Device::create(Backend, void *, void *) { return nullptr; }
}  // namespace ntscrt::gpu
