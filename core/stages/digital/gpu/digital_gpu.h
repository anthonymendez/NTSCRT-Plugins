// digital_gpu.h — the full-size pictures of the Digital video stage on the GPU
// (design: docs/dev/DIGITAL-GPU.md). The capture scale-down (Renderer::scale)
// and the upscale back to the frame's size (Renderer::upscale) run as kernels
// (shaders/digital_gpu.slang); the camera, the codec and the chroma upsample,
// all at the capture size, stay on the CPU (Renderer::code). The CPU code is the
// reference and the fallback: the route is used only where the kernels give the
// same bytes (digital_gpu_route).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../../../gpu/gpu.h"
#include "../../../gpu/gpu_route.h"
#include "../renderer.h"

namespace ntscrt {

class DigitalGpu {
public:
    explicit DigitalGpu(gpu::Device &dev);
    ~DigitalGpu();
    // Renderer::scale on the GPU: `in` (BGRA8, the frame's picture) to the capture
    // size with `optics` (aliasing, fields), read back: BGRA8 bytes, rows tight.
    // `p`: its scale (kernels) and download times.
    std::vector<uint8_t> scale(const gpu::Texture &in, digital::Size capture, const digital::Settings &optics,
                               digital::Renderer::Profile *p = nullptr);
    // Renderer::upscale on the GPU: `picture` (BGRA8, capture size) up to
    // width x height, a new BGRA8 texture. `p`: its upload and upscale times.
    std::shared_ptr<gpu::Texture> upscale(const std::vector<uint8_t> &picture, digital::Size capture, int width,
                                          int height, digital::Renderer::Profile *p = nullptr);
    // How this device rounds the kernels' arithmetic (the probe, run once):
    // "float" (a precise product and sum are each rounded to nearest float, as
    // on the CPU: the plain kernels), "double" (they aren't, e.g. fused; the *_x
    // kernels compute them in double, which is exact), "inexact" (neither).
    const char *arith_mode();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// NTSCRT_DIGITAL_GPU (gpu_route.h): unset / "" / "auto" / "1" = the GPU route
// where it is exact, "0" = the CPU code always, "force" = also on software
// devices (WARP), on Metal (unverified) and where the probe says "inexact".
GpuRouteEnv digital_gpu_env();
std::string digital_gpu_env_text();
// Whether Digital video frames may take the GPU route on `dev` at all: not with
// NTSCRT_DIGITAL_GPU=0, not on Metal (compiled, unverified: Metal compiles with
// fast math), not on a software device (WARP: the CPU code is faster), not
// where `g`'s probe says "inexact" (each unless "force"). `why` gets the
// reason either way: on, it names the setting, the adapter and the arithmetic.
bool digital_gpu_route(gpu::Device &dev, std::string *why = nullptr, DigitalGpu *g = nullptr);

}  // namespace ntscrt
