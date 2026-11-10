// metal_interop.h — Metal objects behind HAL handles, for code that talks to
// Metal directly (librashader). macOS only; plain C++ (bridged void*).
#pragma once

#include "../gpu.h"

namespace ntscrt::gpu::metal {

// id<MTLCommandQueue> of a Metal HAL device (bridged, not retained), or
// nullptr if `d` isn't one. Device::native_context() gives the current
// id<MTLCommandBuffer> to encode on.
void *command_queue(Device &d);

}  // namespace ntscrt::gpu::metal
