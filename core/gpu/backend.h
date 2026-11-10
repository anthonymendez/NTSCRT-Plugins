// backend.h — private to core/gpu: what the backends share.
#pragma once

#include <memory>
#include <string>

#include "gpu.h"
#include "shader_registry.h"

namespace ntscrt::gpu {

// The opaque pipeline handle of gpu.h; each backend derives from it.
class Kernel {
public:
    virtual ~Kernel() = default;
    const ShaderEntry *entry = nullptr;
    const ShaderFile *file = nullptr;
};

inline int bytes_per_pixel(Format f) {
    switch (f) {
    case Format::BGRA8:
    case Format::RGBA8:
    case Format::R32F: return 4;
    case Format::RGBA16F:
    case Format::RG32F: return 8;
    case Format::RGBA32F: return 16;
    }
    return 4;
}

inline const char *format_name(Format f) {
    switch (f) {
    case Format::BGRA8: return "BGRA8";
    case Format::RGBA8: return "RGBA8";
    case Format::RGBA16F: return "RGBA16F";
    case Format::RGBA32F: return "RGBA32F";
    case Format::R32F: return "R32F";
    case Format::RG32F: return "RG32F";
    }
    return "?";
}

// Backend factories: return nullptr and fill `err` when the backend can't
// start (no GPU, wrong platform). Never throw.
std::unique_ptr<Device> create_d3d11_device(void *native_device, std::string &err);
std::unique_ptr<Device> create_metal_device(void *native_device, void *native_queue, std::string &err);

}  // namespace ntscrt::gpu
