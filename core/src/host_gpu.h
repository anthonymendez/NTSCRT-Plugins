// host_gpu.h — the host's GPU frames <-> the core's GPU device (ntscrt_render_host_gpu).
//
// A Bridge moves pixels between the host's GPU memory (CUDA device pointers on
// Windows, MTLBuffers on macOS) and textures of the core's own HAL device, which
// must be on the same GPU: GPU-to-GPU copies, never through the CPU. Textures it
// hands out are its own (registered with the host API once, reused frame after
// frame). Ordering: work the host queued before the render call happens before
// import; export is queued on the host's queue after the core's GPU work.
// See docs/dev/HOST-GPU.md.
#pragma once

#include <ntscrt/ntscrt.h>

#include <memory>
#include <string>

#include "../gpu/gpu.h"

namespace ntscrt::hostgpu {

// HAL format holding a host image of `f` unchanged (RGBA32F / RGBA16F / RGBA8),
// false for formats the GPU path doesn't take.
bool hal_format(ntscrt_pixel_format f, gpu::Format &out);

class Bridge {
public:
    virtual ~Bridge() = default;
    // Same GPU as `dev`? "" if so, else why not (the GPU path can't be used).
    virtual std::string check(gpu::Device &dev) = 0;
    // The host image in a texture of its format, size and memory row order.
    virtual std::shared_ptr<gpu::Texture> import(gpu::Device &dev, const ntscrt_gpu_image &img) = 0;
    // A shader-writable texture for `img`'s format and size: write the result
    // there, then export_() copies it into the host image.
    virtual std::shared_ptr<gpu::Texture> target(gpu::Device &dev, const ntscrt_gpu_image &img) = 0;
    virtual void export_(gpu::Device &dev, const ntscrt_gpu_image &img) = 0;
    // CPU copies, waited for. Rows of `cpu` top-down (row_bytes may be negative);
    // img rows in img's memory order.
    virtual void read(const ntscrt_gpu_image &img, void *cpu, ptrdiff_t row_bytes) = 0;
    virtual void write(const void *cpu, ptrdiff_t row_bytes, const ntscrt_gpu_image &img) = 0;
    // Use this bridge for a render on `host` if it's the same API and GPU context
    // (CUDA: the stream's context; registrations are per context): switches to
    // its queue and returns true. False: make a new bridge.
    virtual bool rebind(const ntscrt_host_gpu &host) = 0;
    // Host GPU, for logs ("CUDA: NVIDIA GeForce RTX 5090").
    virtual std::string describe() const = 0;
};

// nullptr + err when `api` isn't available in this build / on this machine.
std::unique_ptr<Bridge> make_bridge(const ntscrt_host_gpu &host, std::string &err);
bool supported(ntscrt_host_gpu_api api);

}  // namespace ntscrt::hostgpu
