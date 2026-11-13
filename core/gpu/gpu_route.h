// gpu_route.h — what the GPU routes of the CPU stages share (NTSC: NTSCRT_NTSC_GPU,
// docs/dev/NTSC-GPU.md; Digital video: NTSCRT_DIGITAL_GPU, docs/dev/DIGITAL-GPU.md):
// the switch's values and the facts about the device the routes are decided on.
#pragma once

#include <string>

#include "gpu.h"

namespace ntscrt {

// A route switch: unset / "" / "auto" = Auto (the default), "0" = Off (the CPU
// code always), "1" (or anything else) = On (as Auto), "force" = also where Auto
// says no (software devices, and the cases each route names).
enum class GpuRouteEnv { Auto, Off, On, Force };
// The variable's raw value ("" when unset): a change re-decides an instance's route.
std::string gpu_route_env_text(const char *var);
GpuRouteEnv gpu_route_env(const char *var);
// How the route was asked for, for its reason text: "auto", "VAR=1", "VAR=force".
std::string gpu_route_env_label(const char *var, GpuRouteEnv env);

// The D3D11 adapter's name ("" on Metal or when unknown).
std::string gpu_adapter_description(gpu::Device &dev);
// The D3D11 device is Microsoft's software rasterizer (WARP).
bool gpu_is_software(gpu::Device &dev);

}  // namespace ntscrt
